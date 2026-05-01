/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MPro backlight driver — internal header.
 *
 * Vakiot ja per-instanssin tila-rakenne. Yksittäinen lähdetiedosto
 * mpro_backlight.c käyttää näitä; header on lähinnä siisti tapa
 * erottaa tyypit toiminnallisuudesta.
 */
#ifndef _MPRO_BACKLIGHT_H_
#define _MPRO_BACKLIGHT_H_ 1

#include <linux/backlight.h>
#include <linux/mutex.h>
#include "../mpro.h"

#define MPRO_BL_DEFAULT		100
#define MPRO_BL_MAX		255

#define MPRO_BL_GAMMA_DEFAULT	100	/* 1.00 — lineaarinen */
#define MPRO_BL_GAMMA_MIN	50	/* 0.50 */
#define MPRO_BL_GAMMA_MAX	400	/* 4.00 */

struct mpro_backlight {
	struct mpro_device *mpro;
	struct backlight_device *bl;
	struct mpro_screen_listener listener;

	struct mutex lock;	/* suojaa state-kentät */
	bool screen_on;
	u8 stored_value;	/* käyttäjän viimeisin pyyntö */
	u32 gamma_x100;		/* 100 = lineaarinen */
};

#endif /* _MPRO_BACKLIGHT_H_ */
