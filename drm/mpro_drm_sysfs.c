// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_drm_sysfs.c — DRM-puolen sysfs-attribuutit.
 *
 * Per-laite attribuutit:
 *   rotation         — laitteen näytön orientaatio (0..7, ks. mpro_rotation_map)
 *   brightness       — ohjelmistollinen kirkkaus (0..100)
 *   gamma            — gamma-korjaus (0.50..4.00)
 *   disable_partial  — pakota täyspäivitykset
 *   gamma_lut        — binääriattr, suora 768-tavun LUT (3×256)
 *
 * Tämä tiedosto myös sisältää mpro_drm__apply_rotation:in koska se
 * triggeröidään sysfs-puolelta.
 */

#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#include <drm/drm_blend.h>
#include <drm/drm_probe_helper.h>

#include "mpro_drm.h"

/* ------------------------------------------------------------------ */
/* Rotation map                                                       */
/* ------------------------------------------------------------------ */

static const u16 mpro_drm__rotation_map[] = {
	DRM_MODE_ROTATE_0,
	DRM_MODE_ROTATE_90,
	DRM_MODE_ROTATE_180,
	DRM_MODE_ROTATE_270,
	DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X,
	DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X,
	DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X,
	DRM_MODE_ROTATE_270 | DRM_MODE_REFLECT_X,
};

void mpro_drm__apply_rotation(struct mpro_drm *mdrm, u16 new_rotation)
{
	struct drm_device *drm = &mdrm->drm;
	bool swap = mpro_drm__rotation_swaps_dims(new_rotation);
	u32 base_w = mdrm->mpro->model->width;
	u32 base_h = mdrm->mpro->model->height;

	if (mdrm->rotation == new_rotation)
		return;

	mdrm->rotation = new_rotation;
	mdrm->width = swap ? base_h : base_w;
	mdrm->height = swap ? base_w : base_h;

	mutex_lock(&drm->mode_config.mutex);
	drm->mode_config.min_width = mdrm->width;
	drm->mode_config.max_width = mdrm->width;
	drm->mode_config.min_height = mdrm->height;
	drm->mode_config.max_height = mdrm->height;
	mutex_unlock(&drm->mode_config.mutex);

	if (mdrm->data)
		memset(mdrm->data, 0, mdrm->data_size);
	mpro_drm__request_update(mdrm, 0, 0, mdrm->width, mdrm->height, false);

	drm_kms_helper_hotplug_event(drm);
}

/* ------------------------------------------------------------------ */
/* gamma_lut binary attribute                                         */
/* ------------------------------------------------------------------ */

static ssize_t gamma_lut_write(struct file *f, struct kobject *kobj,
			       const struct bin_attribute *a, char *buf,
			       loff_t off, size_t count)
{
	struct mpro_drm *mdrm = dev_get_drvdata(kobj_to_dev(kobj));

	if (off != 0 || count != 768)
		return -EINVAL;

	memcpy(mdrm->lut[0], buf, 256);
	memcpy(mdrm->lut[1], buf + 256, 256);
	memcpy(mdrm->lut[2], buf + 512, 256);
	mdrm->gamma_valid = true;
	mpro_drm__rebuild_combined_lut(mdrm);
	return count;
}

static const BIN_ATTR_WO(gamma_lut, MPRO_DRM_LUT_SIZE);

static const struct bin_attribute *mpro_drm__bin_attrs[] = {
	&bin_attr_gamma_lut,
	NULL,
};

/* ------------------------------------------------------------------ */
/* Text attributes                                                    */
/* ------------------------------------------------------------------ */

static ssize_t rotation_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);
	u32 i;

	for (i = 0; i < ARRAY_SIZE(mpro_drm__rotation_map); i++)
		if (mdrm->rotation == mpro_drm__rotation_map[i])
			return sysfs_emit(buf, "%u\n", i);
	return sysfs_emit(buf, "0\n");
}

static ssize_t rotation_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);
	u32 val;

	if (kstrtou32(buf, 10, &val) ||
	    val >= ARRAY_SIZE(mpro_drm__rotation_map))
		return -EINVAL;

	mpro_drm__apply_rotation(mdrm, mpro_drm__rotation_map[val]);
	return count;
}

static DEVICE_ATTR_RW(rotation);

static ssize_t brightness_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", mdrm->brightness);
}

static ssize_t brightness_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);
	u32 val;

	if (kstrtou32(buf, 10, &val))
		return -EINVAL;
	if (val > MPRO_BRIGHTNESS_MAX)
		return -EINVAL;

	mdrm->brightness = val;
	mpro_drm__rebuild_combined_lut(mdrm);
	return count;
}

static DEVICE_ATTR_RW(brightness);

static ssize_t gamma_show(struct device *dev, struct device_attribute *a,
			  char *buf)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u.%02u\n",
			  mdrm->gamma_x100 / 100, mdrm->gamma_x100 % 100);
}

static ssize_t gamma_store(struct device *dev, struct device_attribute *a,
			   const char *buf, size_t count)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);
	unsigned int whole, frac = 0;
	const char *dot;
	int n;
	int frac_digits = 0;

	dot = strchr(buf, '.');
	n = sscanf(buf, "%u.%u", &whole, &frac);

	if (n < 1)
		return -EINVAL;

	if (n == 2 && dot) {
		/* Laske montako numeroa pisteen jälkeen on */
		const char *p = dot + 1;
		while (*p && *p >= '0' && *p <= '9') {
			frac_digits++;
			p++;
		}
		if (frac_digits == 1)
			frac *= 10;	/* "2.5" → 50 */
		else if (frac_digits > 2)
			frac /= int_pow(10, frac_digits - 2);
	}

	mdrm->gamma_x100 = whole * 100 + frac;
	if (mdrm->gamma_x100 < 50 || mdrm->gamma_x100 > 400)
		return -EINVAL;

	mpro_drm__build_power_lut(mdrm, mdrm->gamma_x100);
	return count;
}

static DEVICE_ATTR_RW(gamma);

static ssize_t disable_partial_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", mdrm->disable_partial ? 1 : 0);
}

static ssize_t disable_partial_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	mdrm->disable_partial = val;
	return count;
}

static DEVICE_ATTR_RW(disable_partial);

static struct attribute *mpro_drm__attrs[] = {
	&dev_attr_rotation.attr,
	&dev_attr_brightness.attr,
	&dev_attr_gamma.attr,
	&dev_attr_disable_partial.attr,
	NULL,
};

static const struct attribute_group mpro_drm__attr_group = {
	.attrs = mpro_drm__attrs,
	.bin_attrs = mpro_drm__bin_attrs,
};

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int mpro_drm__sysfs_create(struct mpro_drm *mdrm)
{
	return sysfs_create_group(&mdrm->pdev->dev.kobj, &mpro_drm__attr_group);
}

void mpro_drm__sysfs_remove(struct mpro_drm *mdrm)
{
	sysfs_remove_group(&mdrm->pdev->dev.kobj, &mpro_drm__attr_group);
}
