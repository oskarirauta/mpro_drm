// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_touchscreen.c — MPro USB touchscreen driver.
 *
 * Reads 14-byte touch packets from interrupt endpoint 0x81. Supports
 * two simultaneous touch points. Tracks the DRM-side rotation for
 * coordinate transformation, and applies per-model calibration flags
 * (touch_invert_x/y, touch_swap_xy) at the raw stage before rotation.
 *
 * The URB only runs while userspace has the input device open AND the
 * DRM pipe is active — releasing either of those conditions stops it.
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
 * Convert device coordinates to DRM coordinates according to the
 * current rotation. The device's native resolution is
 * (model->width, model->height); the DRM-visible resolution may be
 * swapped for ROTATE_90 / ROTATE_270.
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
		"release timeout — releasing all fingers\n");

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

		slot  = p->yh.y.id;
		state = p->xh.x.f;

		/* Slot >= MPRO_TOUCH_MAX_SLOTS means "unused" */
		if (slot >= MPRO_TOUCH_MAX_SLOTS)
			continue;

		x = ((u16)p->xh.x.h << 8) | p->xl;
		y = ((u16)p->yh.y.h << 8) | p->yl;

		/* Touch panel orientation compensation, raw stage */
		if (model->touch_swap_xy) {
			u16 t = x;

			x = y;
			y = t;
		}
		if (model->touch_invert_x)
			x = model->width  - 1 - x;
		if (model->touch_invert_y)
			y = model->height - 1 - y;

		/*
		 * State codes on firmware v0.25 (verified by USB-protocol
		 * analysis; differs from the original userspace assumption):
		 *
		 *   0 = touch start marker
		 *       (coordinates may be stale)
		 *   1 = release (finger lifted, coordinates discarded)
		 *   2 = active touch / drag (coordinates valid)
		 *   3 = not observed in use
		 *
		 * Only state=2 reports an actual touch. state=1 triggers
		 * the automatic release through input_mt_sync_frame()
		 * because active=false.
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
	 * Release watchdog: the firmware sends a state=1 packet when a
	 * finger is lifted, which releases the touch immediately above.
	 * The watchdog timer is only a safety net for the case where a
	 * firmware bug or a dropped USB packet causes state=1 to never
	 * arrive — in that case the finger is released after
	 * MPRO_TOUCH_RELEASE_WATCHDOG_MS.
	 */
	if (any_active) {
		mt->any_active = true;
		mod_timer(&mt->release_timer,
			  jiffies +
			  msecs_to_jiffies(MPRO_TOUCH_RELEASE_WATCHDOG_MS));
	} else if (mt->any_active) {
		/* Packet itself confirmed the release — no timer needed */
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
		/*
		 * Fatal: device gone, suspend, or protocol error.
		 * Do not resubmit — restart goes through resync().
		 */
		mt->submitted = false;
		return;

	default:
		/* Transient error (e.g. -EILSEQ CRC) — try again */
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
 * Start/stop gate: the URB runs while (opened && screen_on), and stops
 * otherwise. Called whenever either condition changes.
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

	mpro_active_get(mt->mpro, &mt->pm_active);

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
	 * Stop the release timer and clear MT state. This makes sure the
	 * next input_open() starts from a clean slate, even if the
	 * previous client (e.g. Weston) closed mid-touch without
	 * releasing first.
	 */
	timer_delete_sync(&mt->release_timer);

	for (i = 0; i < MPRO_TOUCH_MAX_SLOTS; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(input);
	input_sync(input);
	mt->any_active = false;

	mpro_active_put(mt->mpro, &mt->pm_active);
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

	/* Interrupt buffer + URB */
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

	input->name		= "MPro touchscreen";
	input->id.bustype	= BUS_USB;
	input->id.vendor	= 0xc872;
	input->id.product	= 0x1004;
	input->open		= mpro_ts__input_open;
	input->close		= mpro_ts__input_close;
	input->dev.parent	= dev;

	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_MT_POSITION_X,
			     0, mpro->model->width  - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y,
			     0, mpro->model->height - 1, 0, 0);

	/*
	 * INPUT_MT_DIRECT — touchscreen (direct touch, not a trackpad).
	 *
	 * INPUT_MT_DROP_UNUSED is deliberately omitted: the firmware does
	 * not always report both slots in every packet, and DROP_UNUSED
	 * could release an active slot by accident. Releases are handled
	 * explicitly through the state=1 packet and the watchdog timer.
	 */
	ret = input_mt_init_slots(input, MPRO_TOUCH_MAX_SLOTS, INPUT_MT_DIRECT);
	if (ret)
		goto err_buf;

	ret = input_register_device(input);
	if (ret)
		goto err_buf;

	/* Screen state listener */
	mt->listener.screen_off	= mpro_ts__screen_off;
	mt->listener.screen_on	= mpro_ts__screen_on;
	mt->listener.priv	= mt;

	ret = mpro_screen_listener_register(mpro, &mt->listener);
	if (ret)
		goto err_buf;

	platform_set_drvdata(pdev, mt);

	dev_info(dev, "touchscreen registered (%ux%u, %d-finger MT)\n",
		 mpro->model->width, mpro->model->height,
		 MPRO_TOUCH_MAX_SLOTS);
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

	/* Block screen-state callbacks before tearing down the URB */
	mpro_screen_listener_unregister(mt->mpro, &mt->listener);

	timer_delete_sync(&mt->release_timer);
	mpro_ts__stop(mt);

	/*
	 * Null the references before freeing. If input_close() runs from
	 * the devm teardown after this point and reaches mpro_ts__stop(),
	 * the NULL check there prevents a use-after-free.
	 */
	usb_free_coherent(mt->mpro->udev, MPRO_TOUCH_PKT_SIZE,
			  mt->buf, mt->buf_dma);
	mt->buf = NULL;

	usb_free_urb(mt->urb);
	mt->urb = NULL;

	/* devm_input_*: the registration is released automatically */
}

static struct platform_driver mpro_ts__driver = {
	.probe	= mpro_ts__probe,
	.remove	= mpro_ts__remove,
	.driver	= { .name = "mpro_touchscreen" },
};

module_platform_driver(mpro_ts__driver);

MODULE_AUTHOR("Oskari Rauta");
MODULE_DESCRIPTION("MPro USB touchscreen driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mpro_touchscreen");
