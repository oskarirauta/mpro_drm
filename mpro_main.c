// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_main.c — MPro USB display driver, MFD parent.
 *
 * USB-tason perus-ajuri (probe, disconnect, suspend, resume) joka
 * rekisteröi MFD-rakenteen ja kolme lapsi-platform-laitetta:
 * mpro_drm, mpro_touchscreen ja mpro_backlight.
 *
 * Kaikki USB-I/O kulkee tämän moduulin kautta. Lapset käyttävät
 * julkista API:a (mpro.h): mpro_send_full_frame, mpro_send_command,
 * mpro_make_request, jne.
 */

#include <linux/module.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/lz4.h>

#include "mpro_internal.h"
#include "drm/mpro_drm.h"
#include "mpro.h"

/* ------------------------------------------------------------------ */
/* Module parameters                                                  */
/* ------------------------------------------------------------------ */

static bool mpro_fbdev_enabled;
module_param_named(fbdev, mpro_fbdev_enabled, bool, 0444);
MODULE_PARM_DESC(fbdev,
		 "Enable fbdev console emulation on attached displays (default: 0). "
		 "When enabled, USB autosuspend is disabled because fbdev keeps "
		 "the display pipe permanently active. Enable this only if you "
		 "want the MPro to act as a kernel console.");

static int mpro_lz4_level;
module_param_named(lz4_level, mpro_lz4_level, int, 0644);
MODULE_PARM_DESC(lz4_level,
		 "LZ4 compression: 0=off (default), 1=fast, 2-12=HC levels");

static int mpro_lz4_threshold = 1024;
module_param_named(lz4_threshold, mpro_lz4_threshold, int, 0644);
MODULE_PARM_DESC(lz4_threshold,
	"Minimum frame size in bytes to apply LZ4 compression "
	"(default: 1024). Frames smaller than this are sent uncompressed "
	"because the compression overhead outweighs the bandwidth saved.");

/* ------------------------------------------------------------------ */
/* PM                                                                 */
/* ------------------------------------------------------------------ */

static int mpro_usb_suspend(struct usb_interface *intf, pm_message_t msg)
{
	struct mpro_device *mpro = usb_get_intfdata(intf);

	if (!mpro)
		return 0;

	dev_dbg(&intf->dev, "USB suspend\n");

	/*
	 * "Enter sleep mode" -opcode on dokumentoimaton — laite vain pysähtyy
	 * USB-tasolla. DRM on jo lähettänyt mustan kuvan ja backlight on
	 * sammutettu screen_off-listenerin kautta.
	 */

	return 0;
}

static int mpro_usb_resume(struct usb_interface *intf)
{
	struct mpro_device *mpro = usb_get_intfdata(intf);
	u8 wake[6] = { 0x00, 0x29, 0x00, 0x00, 0x00, 0x00 };	/* quit sleep */

	if (!mpro)
		return 0;

	dev_dbg(&intf->dev, "USB resume\n");

	/* mpro_send_command käyttää usb_control_msg_send:iä joka sallii
	 * stack-puskurin (sisäinen kmemdup) — ei tarvitse heap-allokointia. */
	mpro_send_command(mpro, wake, sizeof(wake));
	return 0;
}

void mpro_autopm_put_interface(struct mpro_device *mpro)
{
	if (mpro->fbdev_enabled)
		return;

	usb_autopm_put_interface(mpro->intf);
}

EXPORT_SYMBOL_GPL(mpro_autopm_put_interface);

int mpro_autopm_get_interface(struct mpro_device *mpro)
{
	if (mpro->fbdev_enabled)
		return 0;

	return usb_autopm_get_interface(mpro->intf);
}

EXPORT_SYMBOL_GPL(mpro_autopm_get_interface);

/* ------------------------------------------------------------------ */
/* MFD cells                                                          */
/* ------------------------------------------------------------------ */

static struct mfd_cell mpro_cells[] = {
	{.name = "mpro_drm" },
	{.name = "mpro_touchscreen" },
	{.name = "mpro_backlight" },
};

/* ------------------------------------------------------------------ */
/* devm release callbacks                                             */
/* ------------------------------------------------------------------ */

static void mpro_dma_dev_release(void *data)
{
	struct mpro_device *mpro = data;

	if (mpro->dma_dev)
		put_device(mpro->dma_dev);
}

static void mpro_lz4_workmem_release(void *data)
{
	struct mpro_device *mpro = data;

	if (mpro->lz4_workmem) {
		kvfree(mpro->lz4_workmem);
		mpro->lz4_workmem = NULL;
	}
}

static void mpro_wq_release(void *data)
{
	struct mpro_device *mpro = data;

	mpro_io_shutdown(mpro);
	if (mpro->wq) {
		destroy_workqueue(mpro->wq);
		mpro->wq = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* LZ4 helpers                                                        */
/* ------------------------------------------------------------------ */

int mpro_lz4_workmem_alloc(struct mpro_device *mpro)
{
	int ret;

	if (mpro->lz4_workmem)
		return 0;

	mpro->lz4_workmem_size = max((size_t)LZ4_MEM_COMPRESS,
				     (size_t)LZ4HC_MEM_COMPRESS);
	mpro->lz4_workmem = kvmalloc(mpro->lz4_workmem_size, GFP_KERNEL);
	if (!mpro->lz4_workmem)
		return -ENOMEM;

	ret = devm_add_action_or_reset(&mpro->intf->dev,
				       mpro_lz4_workmem_release, mpro);
	if (ret)
		return ret;
	return 0;
}

EXPORT_SYMBOL_GPL(mpro_lz4_workmem_alloc);

static int mpro_setup_lz4(struct mpro_device *mpro, struct usb_interface *intf)
{
	int requested;
	int ret;

	if (mpro_lz4_level <= 0)
		return 0;

	requested = clamp(mpro_lz4_level, 0, 12);

	if (mpro->model->margin > 0) {
		dev_info(&intf->dev,
			 "LZ4 requested but disabled: margin device "
			 "does not support compression\n");
		return 0;
	}

	if (!mpro_firmware_supports_lz4(mpro)) {
		dev_warn(&intf->dev,
			 "LZ4 requested but disabled: firmware %d.%d "
			 "does not support compression (need >= %d.%d)\n",
			 mpro->fw_major, mpro->fw_minor,
			 MPRO_LZ4_MIN_MAJOR, MPRO_LZ4_MIN_MINOR);
		return 0;
	}

	ret = mpro_lz4_workmem_alloc(mpro);
	if (ret)
		return ret;

	mpro->lz4_level = requested;
	dev_info(&intf->dev, "LZ4 compression enabled: level %d\n",
		 mpro->lz4_level);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Probe / disconnect                                                 */
/* ------------------------------------------------------------------ */

static int mpro_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct device *dev = &intf->dev;
	struct mpro_device *mpro;
	int ret;

	if (usb_get_intfdata(intf))
		return -EBUSY;

	mpro = devm_kzalloc(dev, sizeof(*mpro), GFP_KERNEL);
	if (!mpro)
		return -ENOMEM;

	mpro->intf = intf;
	mpro->udev = interface_to_usbdev(intf);
	mpro->request_delay = 5;
	mpro->running = false;
	mpro->fbdev_enabled = mpro_fbdev_enabled;

	mutex_init(&mpro->lock);
	mutex_init(&mpro->lz4_lock);
	mutex_init(&mpro->listeners_lock);
	INIT_LIST_HEAD(&mpro->screen_listeners);

	/* DMA device — child driver buffer-sharing */
	mpro->dma_dev = usb_intf_get_dma_device(intf);
	if (mpro->dma_dev) {
		ret = devm_add_action_or_reset(dev, mpro_dma_dev_release, mpro);
		if (ret)
			return ret;
	} else {
		dev_warn(dev, "buffer sharing not supported\n");
	}

	/* --- Synchronous probe queries --- */

	ret = mpro_request_u32(mpro, cmd_get_screen, sizeof(cmd_get_screen),
			       &mpro->screen);
	if (ret)
		return dev_err_probe(dev, ret, "get_screen failed\n");

	ret = mpro_request_u32(mpro, cmd_get_version, sizeof(cmd_get_version),
			       &mpro->version);
	if (ret)
		return dev_err_probe(dev, ret, "get_version failed\n");

	ret = mpro_make_request(mpro, cmd_get_id, sizeof(cmd_get_id),
				mpro->id, sizeof(mpro->id));
	if (ret)
		return dev_err_probe(dev, ret, "get_id failed\n");

	ret = mpro_make_request(mpro, cmd_quit_sleepmode,
				sizeof(cmd_quit_sleepmode), NULL, 0);
	if (ret)
		dev_warn(dev, "quit_sleepmode failed: %d\n", ret);

	ret = mpro_get_model(mpro);
	if (ret)
		return dev_err_probe(dev, ret,
				     "unsupported model 0x%x\n", mpro->screen);

	dev_info(dev, "Detected %s\n", mpro->model->description);

	ret = mpro_get_firmware(mpro);
	if (ret)
		dev_warn(dev, "firmware probe failed: %d\n", ret);

	if (mpro->fw_major >= 0)
		dev_info(dev, "Firmware: %s\n", mpro->fw_string);
	else
		dev_warn(dev,
			 "Firmware: %s (version parsing failed)\n",
			 mpro->fw_string);

	/* --- LZ4 (käyttää firmware-versiota → tämän jälkeen) --- */

	mpro->lz4_threshold = clamp(mpro_lz4_threshold, 0, INT_MAX);
	ret = mpro_setup_lz4(mpro, intf);
	if (ret)
		return ret;

	/* --- Pipeline --- */

	mpro->wq = alloc_ordered_workqueue("mpro_wq", WQ_MEM_RECLAIM);
	if (!mpro->wq)
		return -ENOMEM;

	mpro_io_init(mpro);

	/* mpro_io_shutdown + destroy_workqueue automaattisesti
	 * probe-fail/disconnect-tilanteissa */
	ret = devm_add_action_or_reset(dev, mpro_wq_release, mpro);
	if (ret)
		return ret;

	/* Sysfs-attribuutit (devm-pohjaiset jo) */
	ret = mpro_sysfs_add(mpro);
	if (ret)
		return dev_err_probe(dev, ret, "sysfs add failed\n");

	usb_set_intfdata(intf, mpro);
	mpro->running = true;

	/* MFD-lapset */
	ret = devm_mfd_add_devices(dev, -1, mpro_cells,
				   ARRAY_SIZE(mpro_cells), NULL, 0, NULL);
	if (ret) {
		mpro->running = false;
		usb_set_intfdata(intf, NULL);
		return dev_err_probe(dev, ret, "MFD register failed\n");
	}

	/* Autosuspend (kun fbdev-tila ei ole päällä) */
	if (!mpro->fbdev_enabled) {
		pm_runtime_set_autosuspend_delay(&mpro->udev->dev, 30000);
		pm_runtime_use_autosuspend(&mpro->udev->dev);
		dev_info(dev, "USB autosuspend enabled (delay 30s)\n");
	} else {
		dev_info(dev, "fbdev console mode: USB autosuspend disabled\n");
	}

	dev_info(dev, "MPRO core registered\n");
	return 0;
}

static void mpro_usb_disconnect(struct usb_interface *intf)
{
	struct mpro_device *mpro = usb_get_intfdata(intf);
	struct mpro_drm *mdrm;
	unsigned long flags;

	if (!mpro)
		return;

	/*
	 * Estä uudet lähetykset pipeline:hen. Lapsetkin voivat vielä
	 * olla kutsumassa meitä omien teardown:iensa aikana — saavat
	 * -ESHUTDOWN.
	 */
	spin_lock_irqsave(&mpro->state_lock, flags);
	mpro->running = false;
	spin_unlock_irqrestore(&mpro->state_lock, flags);

	/*
	 * DRM unplug ennen kuin MFD-lapset puretaan. Tämä estää uudet
	 * DRM-operaatiot mutta jättää mdrm->mpro-viittauksen ennalleen
	 * — DRM:n oma remove-callback nullaa sen turvallisesti kun
	 * kaikki callbackit ovat valmistuneet.
	 */
	mdrm = READ_ONCE(mpro->drm);
	if (mdrm) {
		drm_dev_unplug(&mdrm->drm);
		WRITE_ONCE(mpro->drm, NULL);
	}

	usb_set_intfdata(intf, NULL);

	dev_info(&intf->dev, "MPRO disconnected\n");

	/*
	 * Loput resurssit (workqueue + pipeline shutdown, lz4_workmem,
	 * dma_dev, MFD-lapset, sysfs) puretaan automaattisesti devres:in
	 * kautta tämän funktion palautuksen jälkeen, devm_add_action_or_reset
	 * -kutsujen rekisteröimien funktioiden mukaan. Järjestys on
	 * käänteinen rekisteröintijärjestyksestä.
	 */
}

/* ------------------------------------------------------------------ */
/* USB driver                                                         */
/* ------------------------------------------------------------------ */

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(0xc872, 0x1004) },
	{ }
};

MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver mpro_usb_driver = {
	.name = "mpro",
	.probe = mpro_usb_probe,
	.disconnect = mpro_usb_disconnect,
	.suspend = mpro_usb_suspend,
	.resume = mpro_usb_resume,
	.reset_resume = mpro_usb_resume,
	.supports_autosuspend = 1,
	.id_table = id_table,
};

module_usb_driver(mpro_usb_driver);

MODULE_DESCRIPTION("MPRO USB driver (MFD infrastructure)");
MODULE_AUTHOR("Oskari Rauta <oskari.rauta@gmail.com>");
MODULE_LICENSE("GPL");
