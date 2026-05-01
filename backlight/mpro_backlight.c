// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_backlight.c — MPro USB display backlight driver.
 *
 * Rekisteröi standardin Linux backlight class -laitteen ja seuraa
 * DRM-puolen pipe enable/disable-tilaa screen state -listener:in
 * kautta. Sisältää ohjelmistollisen gamma-käyrän käyttäjän
 * havaitseman kirkkauden lineaarisemmaksi tekemiseen.
 *
 * Laitteella ei ole readback:iä taustavalon arvolle, joten asetamme
 * oletusarvon (50%) probe-vaiheessa.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/math.h>
#include <linux/string.h>

#include "mpro_backlight.h"

/* ------------------------------------------------------------------ */
/* Module parameters                                                  */
/* ------------------------------------------------------------------ */

static int mpro_bl_default = MPRO_BL_DEFAULT;
module_param_named(default_brightness, mpro_bl_default, int, 0444);
MODULE_PARM_DESC(default_brightness,
	"Initial backlight brightness on probe (0..255, default "
	__stringify(MPRO_BL_DEFAULT) "). The device has no readback for "
	"the current brightness, so this value is sent on each probe. "
	"Can be set on the kernel command line: mpro_backlight.default_brightness=N");

/* ------------------------------------------------------------------ */
/* Gamma curve                                                        */
/* ------------------------------------------------------------------ */

/*
 * y = round(255 * (x/255)^(g_x100/100))
 *
 * Sama kaava kuin mpro_drm_color.c:n mpro_drm__pow_lut, duplikoitu
 * jotta backlight on itsenäinen DRM-moduulista (eivät ole pakollisia
 * rinnakkain). Kaava on niin lyhyt että jaettu header ei ole vaivan
 * arvoinen.
 */
static u8 mpro_bl__pow_curve(u32 x, u32 g_x100)
{
	u32 n = g_x100 / 100;
	u32 f = g_x100 % 100;
	u64 y_n, y_np1, y;

	if (x == 0)
		return 0;
	if (x >= 255)
		return 255;
	if (g_x100 == 100)
		return (u8) x;

	if (n == 0)
		y_n = 255;
	else
		y_n = div64_u64(int_pow(x, n), int_pow(255, n - 1));

	y_np1 = div64_u64(int_pow(x, n + 1), int_pow(255, n));

	y = (y_n * (100 - f) + y_np1 * f) / 100;
	if (y > 255)
		y = 255;
	return (u8) y;
}

/*
 * Sovella gamma-käyrää raw-arvolle ennen laitteelle lähetystä.
 * raw 0..255 -> device 0..255, jossa device = 255 * (raw/255)^(gamma/100).
 *
 * Silmä havaitsee kirkkautta logaritmisesti, joten lineaarinen 0..255
 * tuntuu siltä että matalat arvot eivät tee mitään ja sitten yhtäkkiä
 * on kirkasta. Gamma > 1 antaa pehmeämmän käyrän matalille arvoille.
 */
static u8 mpro_bl__curve(struct mpro_backlight *mb, u8 raw)
{
	u32 g = READ_ONCE(mb->gamma_x100);

	if (g == 100 || raw == 0 || raw == 255)
		return raw;

	return mpro_bl__pow_curve(raw, g);
}

/* ------------------------------------------------------------------ */
/* Hardware command                                                   */
/* ------------------------------------------------------------------ */

static int mpro_bl__send_value(struct mpro_backlight *mb, u8 raw)
{
	/*
	 * Backlight-komennon layout (USB control transferiin sopiva 8 tavua):
	 *   [0] = 0x00 (reserved)
	 *   [1] = 0x51 (komento-byte)
	 *   [2] = 0x02 (alikomento: backlight)
	 *   [3..5] = nolla
	 *   [6] = brightness 0..255
	 *   [7] = nolla
	 */
	u8 cmd[8] = { 0x00, 0x51, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
	u8 device_value = mpro_bl__curve(mb, raw);

	cmd[6] = device_value;
	return mpro_send_command(mb->mpro, cmd, sizeof(cmd));
}

/* ------------------------------------------------------------------ */
/* Backlight class callback                                           */
/* ------------------------------------------------------------------ */

static int mpro_bl__update(struct backlight_device *bl)
{
	struct mpro_backlight *mb = bl_get_data(bl);
	int requested = backlight_get_brightness(bl);
	u8 value = (u8) clamp(requested, 0, MPRO_BL_MAX);
	int ret = 0;

	mutex_lock(&mb->lock);
	mb->stored_value = value;
	if (mb->screen_on)
		ret = mpro_bl__send_value(mb, value);
	mutex_unlock(&mb->lock);

	return ret;
}

static const struct backlight_ops mpro_bl__ops = {
	.update_status = mpro_bl__update,
};

/* ------------------------------------------------------------------ */
/* Screen state listener                                              */
/* ------------------------------------------------------------------ */

static void mpro_bl__screen_off(void *priv)
{
	struct mpro_backlight *mb = priv;

	mutex_lock(&mb->lock);
	mb->screen_on = false;
	mpro_bl__send_value(mb, 0);
	mutex_unlock(&mb->lock);
}

static void mpro_bl__screen_on(void *priv)
{
	struct mpro_backlight *mb = priv;

	mutex_lock(&mb->lock);
	mb->screen_on = true;
	mpro_bl__send_value(mb, mb->stored_value);
	mutex_unlock(&mb->lock);
}

/* ------------------------------------------------------------------ */
/* Sysfs: gamma                                                       */
/* ------------------------------------------------------------------ */

static ssize_t gamma_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct backlight_device *bl = to_backlight_device(dev);
	struct mpro_backlight *mb = bl_get_data(bl);

	return sysfs_emit(buf, "%u.%02u\n",
			  mb->gamma_x100 / 100, mb->gamma_x100 % 100);
}

static ssize_t gamma_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct backlight_device *bl = to_backlight_device(dev);
	struct mpro_backlight *mb = bl_get_data(bl);
	unsigned int whole, frac = 0;
	const char *dot;
	int n;
	u32 new_gamma;

	n = sscanf(buf, "%u.%u", &whole, &frac);
	if (n < 1)
		return -EINVAL;

	if (n == 2) {
		dot = strchr(buf, '.');
		if (dot) {
			const char *p = dot + 1;
			int frac_digits = 0;

			while (*p >= '0' && *p <= '9') {
				frac_digits++;
				p++;
			}
			if (frac_digits == 1)
				frac *= 10;
			else if (frac_digits > 2)
				return -EINVAL;
		}
	}

	new_gamma = whole * 100 + frac;
	if (new_gamma < MPRO_BL_GAMMA_MIN || new_gamma > MPRO_BL_GAMMA_MAX)
		return -EINVAL;

	WRITE_ONCE(mb->gamma_x100, new_gamma);

	mutex_lock(&mb->lock);
	if (mb->screen_on)
		mpro_bl__send_value(mb, mb->stored_value);
	mutex_unlock(&mb->lock);

	return count;
}

static DEVICE_ATTR_RW(gamma);

static struct attribute *mpro_bl__attrs[] = {
	&dev_attr_gamma.attr,
	NULL,
};

static const struct attribute_group mpro_bl__attr_group = {
	.attrs = mpro_bl__attrs,
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int mpro_bl__probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpro_device *mpro = dev_get_drvdata(dev->parent);
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_LINEAR,
		.max_brightness = MPRO_BL_MAX,
		.brightness = mpro_bl_default,
	};
	struct mpro_backlight *mb;
	char name[64];
	int ret;
	u8 initial;

	if (!mpro)
		return -ENODEV;

	/* Validoi parametri — clampataan turvalliseen rajaan */
	if (mpro_bl_default < 0 || mpro_bl_default > MPRO_BL_MAX) {
		dev_warn(dev,
			 "default_brightness=%d invalid, clamping to 0..%u\n",
			 mpro_bl_default, MPRO_BL_MAX);
		mpro_bl_default = clamp(mpro_bl_default, 0, MPRO_BL_MAX);
	}

	initial = (u8)mpro_bl_default;

	mb = devm_kzalloc(dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return -ENOMEM;

	mb->mpro = mpro;
	mb->screen_on = true;
	mb->stored_value = initial;
	mb->gamma_x100 = MPRO_BL_GAMMA_DEFAULT;
	mutex_init(&mb->lock);

	snprintf(name, sizeof(name), "mpro-%s", dev_name(dev->parent));

	mb->bl = devm_backlight_device_register(dev, name, dev, mb,
						&mpro_bl__ops, &props);
	if (IS_ERR(mb->bl))
		return dev_err_probe(dev, PTR_ERR(mb->bl),
				     "backlight register failed\n");

	platform_set_drvdata(pdev, mb);

	mb->listener.screen_off = mpro_bl__screen_off;
	mb->listener.screen_on = mpro_bl__screen_on;
	mb->listener.priv = mb;

	ret = mpro_screen_listener_register(mpro, &mb->listener);
	if (ret)
		return dev_err_probe(dev, ret,
				     "screen listener register failed\n");

	/* Aseta laite oletusarvoon — laitteella ei ole readback:iä */
	mpro_bl__send_value(mb, initial);

	ret = sysfs_create_group(&pdev->dev.kobj, &mpro_bl__attr_group);
	if (ret) {
		mpro_screen_listener_unregister(mpro, &mb->listener);
		return dev_err_probe(dev, ret, "sysfs create failed\n");
	}

	dev_info(dev, "backlight registered: %s (default %u/%u)\n",
		 name, MPRO_BL_DEFAULT, MPRO_BL_MAX);
	return 0;
}

static void mpro_bl__remove(struct platform_device *pdev)
{
	struct mpro_backlight *mb = platform_get_drvdata(pdev);

	if (!mb)
		return;

	sysfs_remove_group(&pdev->dev.kobj, &mpro_bl__attr_group);
	mpro_screen_listener_unregister(mb->mpro, &mb->listener);
	/* devm hoitaa backlight_device_unregister:n */
}

static struct platform_driver mpro_bl__driver = {
	.probe = mpro_bl__probe,
	.remove = mpro_bl__remove,
	.driver = {.name = "mpro_backlight" },
};

module_platform_driver(mpro_bl__driver);

MODULE_AUTHOR("Oskari Rauta");
MODULE_DESCRIPTION("MPro USB display backlight driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mpro_backlight");
