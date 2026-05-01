// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_touchscreen.c — MPro USB touchscreen driver.
 *
 * Lukee 14-tavuisia touch-paketteja interrupt-endpointista 0x81.
 * Tukee 2 yhtäaikaista kosketuspistettä. Seuraa DRM-puolen rotaatiota
 * koordinaattimuunnoksessa, ja per-mallin kalibrointiflagit
 * (touch_invert_x/y, touch_swap_xy) sovelletaan RAW-vaiheessa ennen
 * rotation:ia.
 *
 * URB pyörii vain kun userspace on avannut input-laitteen JA DRM-pipe
 * on aktiivinen — kummankin ehdon vapautuminen pysäyttää sen.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/timer.h>
#include <linux/slab.h>

#include <drm/drm_blend.h>
#include "../drm/mpro_drm.h"
#include "mpro_touchscreen.h"

/* ------------------------------------------------------------------ */
/* Coordinate transformation                                          */
/* ------------------------------------------------------------------ */

static u16 mpro_ts__get_rotation(struct mpro_touch *mt)
{
	if (!mt->mpro->drm)
		return DRM_MODE_ROTATE_0;
	return READ_ONCE(mt->mpro->drm->rotation);
}

/*
 * Muunna laite-koordinaatit DRM-koordinaateiksi rotaation mukaan.
 * Laitteen natiiviresoluutio on (model->width, model->height); DRM:n
 * näkyvä resoluutio voi olla swappiina rotation 90/270 kanssa.
 */
static void mpro_ts__rotate(struct mpro_touch *mt, u16 *x, u16 *y)
{
	const u16 dw = mt->mpro->model->width;
	const u16 dh = mt->mpro->model->height;
	u16 r = mpro_ts__get_rotation(mt);
	bool refl_x = !!(r & DRM_MODE_REFLECT_X);
	bool refl_y = !!(r & DRM_MODE_REFLECT_Y);
	u16 nx = *x, ny = *y;

	if (refl_x)
		nx = dw - 1 - nx;
	if (refl_y)
		ny = dh - 1 - ny;

	switch (r & ~(DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
	case DRM_MODE_ROTATE_90:
		*x = ny;
		*y = dw - 1 - nx;
		break;
	case DRM_MODE_ROTATE_180:
		*x = dw - 1 - nx;
		*y = dh - 1 - ny;
		break;
	case DRM_MODE_ROTATE_270:
		*x = dh - 1 - ny;
		*y = nx;
		break;
	default:
		*x = nx;
		*y = ny;
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Touch release                                                      */
/* ------------------------------------------------------------------ */

static void mpro_ts__release_timeout(struct timer_list *t)
{
	struct mpro_touch *mt = timer_container_of(mt, t, release_timer);
	struct input_dev *input = mt->input;
	int i;

	dev_dbg(&mt->mpro->intf->dev,
		"release timeout — vapautetaan kaikki sormet\n");

	for (i = 0; i < MPRO_TOUCH_MAX_SLOTS; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(input);
	input_sync(input);

	mt->any_active = false;
}

/* ------------------------------------------------------------------ */
/* Packet handling                                                    */
/* ------------------------------------------------------------------ */

static void mpro_ts__handle_packet(struct mpro_touch *mt,
				   const struct mpro_touch_packet *pkt)
{
	const struct mpro_model_info *model = mt->mpro->model;
	struct input_dev *input = mt->input;
	bool any_active = false;
	int i;

	for (i = 0; i < MPRO_TOUCH_MAX_SLOTS; i++) {
		const struct mpro_touch_point *p = &pkt->p[i];
		u16 x, y;
		u8 slot, state;
		bool active;

		slot = p->yh.y.id;
		state = p->xh.x.f;

		/* Slot >= MPRO_TOUCH_MAX_SLOTS: ei käytössä */
		if (slot >= MPRO_TOUCH_MAX_SLOTS)
			continue;

		x = ((u16) p->xh.x.h << 8) | p->xl;
		y = ((u16) p->yh.y.h << 8) | p->yl;

		/* Touch-piirin orientaation kompensointi (RAW-vaihe) */
		if (model->touch_swap_xy) {
			u16 t = x;
			x = y;
			y = t;
		}
		if (model->touch_invert_x)
			x = model->width - 1 - x;
		if (model->touch_invert_y)
			y = model->height - 1 - y;

		/*
		 * State-koodit firmware v0.25 (vrt. eroavaisuus
		 * alkuperäisestä userspace-koodista, varmistettu
		 * USB-protokolla-analyysillä):
		 *
		 *   0 = uuden kosketuksen aloitusmerkki
		 *       (koordinaatit voivat olla vanhentuneita)
		 *   1 = release (sormi nostettu, koordinaatit hylätään)
		 *   2 = aktiivinen kosketus / drag (koordinaatit valideja)
		 *   3 = ei havaittu käytössä
		 *
		 * Vain state=2 raportoi todellista kosketusta. State=1
		 * triggeröi automaattisen release:n input_mt_sync_frame:n
		 * kautta kun active=false.
		 */
		active = (state == 2);

		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, active);

		if (active) {
			mpro_ts__rotate(mt, &x, &y);
			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
			any_active = true;
		}

		dev_dbg(&mt->mpro->intf->dev,
			"slot=%u state=%u active=%d x=%u y=%u%s\n",
			slot, state, active, x, y,
			(state == 1) ? " [release]" : "");
	}

	input_mt_sync_frame(input);
	input_sync(input);

	/*
	 * Release-watchdog: firmware lähettää state=1-paketin sormen
	 * lähdössä, mikä vapauttaa kosketuksen välittömästi tämän
	 * funktion sisällä. Watchdog-timer on turvaverkko sille
	 * tilanteelle, että firmware-bugin tai USB-pakettihäviön
	 * vuoksi state=1 ei tule lainkaan — tällöin sormi vapautuu
	 * MPRO_TOUCH_RELEASE_WATCHDOG_MS-viiveen jälkeen.
	 */
	if (any_active) {
		mt->any_active = true;
		mod_timer(&mt->release_timer,
			  jiffies +
			  msecs_to_jiffies(MPRO_TOUCH_RELEASE_WATCHDOG_MS));
	} else if (mt->any_active) {
		/* Paketti vahvisti release:n itse → ei tarvita timeria */
		timer_delete(&mt->release_timer);
		mt->any_active = false;
	}
}

/* ------------------------------------------------------------------ */
/* URB lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void mpro_ts__irq_complete(struct urb *urb)
{
	struct mpro_touch *mt = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		if (urb->actual_length >= MPRO_TOUCH_PKT_SIZE)
			mpro_ts__handle_packet(mt, urb->transfer_buffer);
		break;

	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPROTO:
		/* Fatal: laite irti, suspend, tai protokollavirhe.
		 * Älä resubmit — uusi käynnistys vasta resync:in kautta. */
		mt->submitted = false;
		return;

	default:
		/* Tilapäinen virhe (esim. -EILSEQ CRC) — yritetään uudelleen */
		dev_dbg(&mt->mpro->intf->dev,
			"touch URB status %d, retrying\n", urb->status);
		break;
	}

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		dev_warn_ratelimited(&mt->mpro->intf->dev,
				     "touch URB resubmit failed: %d\n", ret);
		mt->submitted = false;
	}
}

static int mpro_ts__start(struct mpro_touch *mt)
{
	int ret;

	if (mt->submitted || !mt->urb) {
		dev_dbg(&mt->mpro->intf->dev,
			"start: skip (submitted=%d urb=%p)\n",
			mt->submitted, mt->urb);
		return 0;
	}

	mt->urb->dev = mt->mpro->udev;
	ret = usb_submit_urb(mt->urb, GFP_KERNEL);
	if (ret) {
		dev_warn(&mt->mpro->intf->dev,
			 "touch URB submit failed: %d\n", ret);
		return ret;
	}
	mt->submitted = true;
	dev_dbg(&mt->mpro->intf->dev, "start: URB submitted\n");
	return 0;
}

static void mpro_ts__stop(struct mpro_touch *mt)
{
	if (!mt->submitted || !mt->urb)
		return;
	usb_kill_urb(mt->urb);
	mt->submitted = false;
}

/*
 * Käynnistys/pysäytys: URB pyörii kun (opened && screen_on),
 * muuten pysähtyy. Kutsutaan kummankin ehdon muuttuessa.
 */
static void mpro_ts__resync(struct mpro_touch *mt)
{
	bool should_run;

	mutex_lock(&mt->lock);
	should_run = mt->opened && mt->screen_on;
	if (should_run && !mt->submitted)
		mpro_ts__start(mt);
	else if (!should_run && mt->submitted)
		mpro_ts__stop(mt);
	mutex_unlock(&mt->lock);
}

/* ------------------------------------------------------------------ */
/* input_dev callbacks                                                */
/* ------------------------------------------------------------------ */

static int mpro_ts__input_open(struct input_dev *input)
{
	struct mpro_touch *mt = input_get_drvdata(input);

	mpro_autopm_get_interface(mt->mpro);

	mutex_lock(&mt->lock);
	mt->opened = true;
	mutex_unlock(&mt->lock);

	mpro_ts__resync(mt);
	return 0;
}

static void mpro_ts__input_close(struct input_dev *input)
{
	struct mpro_touch *mt = input_get_drvdata(input);
	int i;

	mutex_lock(&mt->lock);
	mt->opened = false;
	mutex_unlock(&mt->lock);

	mpro_ts__resync(mt);

	/*
	 * Pysäytä release-timer ja vapauta MT-tila. Tämä varmistaa
	 * että seuraava input_open alkaa puhtaalta pöydältä, vaikka
	 * edellinen client (esim. Weston) olisi sulkeutunut
	 * epäsiististi keskellä kosketusta.
	 */
	timer_delete_sync(&mt->release_timer);

	for (i = 0; i < MPRO_TOUCH_MAX_SLOTS; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(input);
	input_sync(input);
	mt->any_active = false;

	mpro_autopm_put_interface(mt->mpro);
}

/* ------------------------------------------------------------------ */
/* Screen state listener                                              */
/* ------------------------------------------------------------------ */

static void mpro_ts__screen_off(void *priv)
{
	struct mpro_touch *mt = priv;

	mutex_lock(&mt->lock);
	mt->screen_on = false;
	mutex_unlock(&mt->lock);

	mpro_ts__resync(mt);
}

static void mpro_ts__screen_on(void *priv)
{
	struct mpro_touch *mt = priv;

	mutex_lock(&mt->lock);
	mt->screen_on = true;
	mutex_unlock(&mt->lock);

	mpro_ts__resync(mt);
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int mpro_ts__probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpro_device *mpro = dev_get_drvdata(dev->parent);
	struct mpro_touch *mt;
	struct input_dev *input;
	int pipe;
	int ret;

	if (!mpro || !mpro->model || !mpro->udev)
		return -ENODEV;

	mt = devm_kzalloc(dev, sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return -ENOMEM;

	mt->mpro = mpro;
	mt->screen_on = true;
	mutex_init(&mt->lock);
	timer_setup(&mt->release_timer, mpro_ts__release_timeout, 0);

	/* IRQ-buffer + URB */
	mt->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!mt->urb)
		return -ENOMEM;

	mt->buf = usb_alloc_coherent(mpro->udev, MPRO_TOUCH_PKT_SIZE,
				     GFP_KERNEL, &mt->buf_dma);
	if (!mt->buf) {
		ret = -ENOMEM;
		goto err_urb;
	}

	pipe = usb_rcvintpipe(mpro->udev, MPRO_TOUCH_EP);
	usb_fill_int_urb(mt->urb, mpro->udev, pipe,
			 mt->buf, MPRO_TOUCH_PKT_SIZE,
			 mpro_ts__irq_complete, mt, MPRO_TOUCH_INTERVAL_MS);
	mt->urb->transfer_dma = mt->buf_dma;
	mt->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* input device */
	input = devm_input_allocate_device(dev);
	if (!input) {
		ret = -ENOMEM;
		goto err_buf;
	}

	mt->input = input;
	input_set_drvdata(input, mt);

	input->name = "MPro touchscreen";
	input->id.bustype = BUS_USB;
	input->id.vendor = 0xc872;
	input->id.product = 0x1004;
	input->open = mpro_ts__input_open;
	input->close = mpro_ts__input_close;
	input->dev.parent = dev;

	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_MT_POSITION_X,
			     0, mpro->model->width - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y,
			     0, mpro->model->height - 1, 0, 0);

	/*
	 * INPUT_MT_DIRECT — kosketusnäyttö (suora kosketus, ei trackpad).
	 *
	 * INPUT_MT_DROP_UNUSED on tarkoituksella jätetty pois: firmware
	 * ei aina lähetä molempia slot-tietoja jokaisessa paketissa, ja
	 * DROP_UNUSED-flag voisi vapauttaa aktiivisen sloten vahingossa.
	 * Release hoidetaan eksplisiittisesti state=1-paketilla ja
	 * watchdog-timerilla.
	 */
	ret = input_mt_init_slots(input, MPRO_TOUCH_MAX_SLOTS, INPUT_MT_DIRECT);
	if (ret)
		goto err_buf;

	ret = input_register_device(input);
	if (ret)
		goto err_buf;

	/* Screen state listener */
	mt->listener.screen_off = mpro_ts__screen_off;
	mt->listener.screen_on = mpro_ts__screen_on;
	mt->listener.priv = mt;

	ret = mpro_screen_listener_register(mpro, &mt->listener);
	if (ret)
		goto err_buf;

	platform_set_drvdata(pdev, mt);

	dev_info(dev, "touchscreen registered (%ux%u, %d-finger MT)\n",
		 mpro->model->width, mpro->model->height, MPRO_TOUCH_MAX_SLOTS);
	return 0;

err_buf:
	usb_free_coherent(mpro->udev, MPRO_TOUCH_PKT_SIZE,
			  mt->buf, mt->buf_dma);
err_urb:
	usb_free_urb(mt->urb);
	return ret;
}

static void mpro_ts__remove(struct platform_device *pdev)
{
	struct mpro_touch *mt = platform_get_drvdata(pdev);

	if (!mt)
		return;

	/* Estä screen state -callbackit ennen URB-purkua */
	mpro_screen_listener_unregister(mt->mpro, &mt->listener);

	timer_delete_sync(&mt->release_timer);
	mpro_ts__stop(mt);

	/*
	 * NULL-ataan viittaukset ennen vapautusta. Jos input_close
	 * laukeaa devm-purussa tämän jälkeen ja kutsuu mpro_ts__stop:ia,
	 * NULL-tarkistus estää use-after-free:n.
	 */
	usb_free_coherent(mt->mpro->udev, MPRO_TOUCH_PKT_SIZE,
			  mt->buf, mt->buf_dma);
	mt->buf = NULL;

	usb_free_urb(mt->urb);
	mt->urb = NULL;

	/* devm_input_*: rekisteröinti puretaan automaattisesti */
}

static struct platform_driver mpro_ts__driver = {
	.probe = mpro_ts__probe,
	.remove = mpro_ts__remove,
	.driver = {.name = "mpro_touchscreen" },
};

module_platform_driver(mpro_ts__driver);

MODULE_AUTHOR("Oskari Rauta");
MODULE_DESCRIPTION("MPro USB touchscreen driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mpro_touchscreen");
