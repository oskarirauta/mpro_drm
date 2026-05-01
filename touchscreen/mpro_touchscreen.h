/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MPro touchscreen driver — internal header.
 *
 * Wire-protocol struct määrittelyt ja per-instanssin tila.
 */
#ifndef _MPRO_TOUCHSCREEN_H_
#define _MPRO_TOUCHSCREEN_H_ 1

#include <linux/usb.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include "../mpro.h"

#define MPRO_TOUCH_PKT_SIZE	14
#define MPRO_TOUCH_EP		0x81
#define MPRO_TOUCH_MAX_SLOTS	2
#define MPRO_TOUCH_INTERVAL_MS	8	/* USB bInterval (~125 Hz) */

/*
 * Release-watchdog timeout: kuinka kauan odotetaan ilman paketteja
 * ennen oletusta että kaikki sormet on poistettu. Normaalisti firmware
 * lähettää state=1-paketin sormen nousessa, joten release tapahtuu
 * välittömästi — tämä on vain turvaverkko firmware-bugeja ja
 * pakettihäviöitä varten.
 */
#define MPRO_TOUCH_RELEASE_WATCHDOG_MS	850

/*
 * Touch-paketin layout — vrt. struct touch + struct point libusb-koodissa.
 *
 * Bitfield-tulkinta:
 *   xh: high 4 bits of x | 2 reserved | 2 bits state
 *   yh: high 4 bits of y | 4 bits slot id (0..1, > 1 = ei käytössä)
 *
 * 12-bittiset koordinaatit yhdistetään: x = (xh.h << 8) | xl
 *
 * State-koodien semantiikka (USB-protokolla-analyysillä firmware v0.25,
 * vrt. eroavaisuus alkuperäisestä userspace-koodista):
 *   0 = uuden kosketuksen aloitusmerkki
 *   1 = release (sormi nostettu)
 *   2 = aktiivinen kosketus (sormi näytöllä, koordinaatit valideja)
 *   3 = ei havaittu käytössä
 */
union mpro_axis {
	struct {
		u8 h:4;
		u8 u:2;
		u8 f:2;		/* state */
	} x;
	struct {
		u8 h:4;
		u8 id:4;	/* slot id */
	} y;
	u8 c;
} __packed;

struct mpro_touch_point {
	union mpro_axis xh;
	u8 xl;
	union mpro_axis yh;
	u8 yl;
	u8 weight;		/* TODO: ehkä ABS_MT_PRESSURE jos validi */
	u8 misc;
} __packed;

struct mpro_touch_packet {
	u8 unused[2];
	u8 count;		/* TODO: tutki onko hyödynnettävissä */
	struct mpro_touch_point p[MPRO_TOUCH_MAX_SLOTS];
} __packed;

struct mpro_touch {
	struct mpro_device *mpro;
	struct input_dev *input;
	struct mpro_screen_listener listener;

	/* IRQ-URB resources */
	struct urb *urb;
	void *buf;
	dma_addr_t buf_dma;

	struct mutex lock;
	bool opened;		/* userspace open() */
	bool screen_on;		/* DRM pipe state */
	bool submitted;		/* URB liikkeellä */

	/* Release-watchdog turvaverkkona firmware-state=1-pakettia varten */
	struct timer_list release_timer;
	bool any_active;
};

#endif /* _MPRO_TOUCHSCREEN_H_ */
