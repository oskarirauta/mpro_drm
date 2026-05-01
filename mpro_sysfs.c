// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_sysfs.c — parent-level sysfs attributes.
 *
 * Exposes device identity (model, screen_id, version, etc.) and
 * pipeline tunables (lz4_level, stats) under the parent's sysfs.
 *
 * Per-child attributes (rotation, brightness, gamma) live in the
 * respective child driver's sysfs.
 */

#include <linux/module.h>
#include <linux/lz4.h>

#include "mpro_internal.h"
#include "mpro.h"

static ssize_t model_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	if (!mpro->model)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", mpro->model->name);
}

static DEVICE_ATTR_RO(model);

static ssize_t description_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	if (!mpro->model)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", mpro->model->description);
}

static DEVICE_ATTR_RO(description);

static ssize_t resolution_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	if (!mpro->model)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%u %u\n", mpro->model->width,
			  mpro->model->height);
}

static DEVICE_ATTR_RO(resolution);

static ssize_t physical_size_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	if (!mpro->model)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%u %u\n", mpro->model->width_mm,
			  mpro->model->height_mm);
}

static DEVICE_ATTR_RO(physical_size);

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%08x\n", mpro->version);
}

static DEVICE_ATTR_RO(version);

static ssize_t screen_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%08x\n", mpro->screen);
}

static DEVICE_ATTR_RO(screen_id);

static ssize_t margin_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	if (!mpro->model)
		return sysfs_emit(buf, "-1\n");
	return sysfs_emit(buf, "%u\n", mpro->model->margin);
}

static DEVICE_ATTR_RO(margin);

static ssize_t device_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%*phC\n", (int)sizeof(mpro->id), mpro->id);
}

static DEVICE_ATTR_RO(device_id);

static ssize_t fbdev_enabled_show(struct device *dev,
				  struct device_attribute *a, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", mpro->fbdev_enabled ? 1 : 0);
}

static DEVICE_ATTR_RO(fbdev_enabled);

static ssize_t lz4_level_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);

	if (mpro->model && mpro->model->margin > 0)
		return sysfs_emit(buf, "unsupported (margin device)\n");
	if (!mpro_firmware_supports_lz4(mpro))
		return sysfs_emit(buf, "unsupported (firmware too old)\n");

	return sysfs_emit(buf, "%d\n", READ_ONCE(mpro->lz4_level));
}

static ssize_t lz4_level_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	int val;
	int ret;

	if (kstrtoint(buf, 10, &val) || val < 0 || val > 12)
		return -EINVAL;

	if (val > 0) {
		if (mpro->model && mpro->model->margin > 0)
			return -EOPNOTSUPP;

		if (mpro->model && mpro->model->margin > 0) {
			dev_warn(&mpro->intf->dev,
				 "LZ4 is not available on margin device\n");
			return -EOPNOTSUPP;
		}

		if (!mpro_firmware_supports_lz4(mpro)) {
			dev_warn(&mpro->intf->dev,
				 "LZ4 requires firmware >= %d.%d (have %d.%d)\n",
				 MPRO_LZ4_MIN_MAJOR, MPRO_LZ4_MIN_MINOR,
				 mpro->fw_major, mpro->fw_minor);
			return -EOPNOTSUPP;
		}

		ret = mpro_lz4_workmem_alloc(mpro);
		if (ret)
			return ret;
	}

	WRITE_ONCE(mpro->lz4_level, val);
	return count;
}

static DEVICE_ATTR_RW(lz4_level);

static ssize_t firmware_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n", mpro->fw_string);
}

static DEVICE_ATTR_RO(firmware);

static ssize_t fw_minor_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", mpro->fw_minor);
}

static DEVICE_ATTR_RO(fw_minor);

static ssize_t fw_major_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", mpro->fw_major);
}

static DEVICE_ATTR_RO(fw_major);

static ssize_t stats_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct mpro_device *mpro = dev_get_drvdata(dev);

	return sysfs_emit(buf, "submitted=%d displayed=%d dropped=%d\n",
			  atomic_read(&mpro->stats_submitted),
			  atomic_read(&mpro->stats_displayed),
			  atomic_read(&mpro->stats_dropped));
}

static DEVICE_ATTR_RO(stats);

static struct attribute *mpro_attrs[] = {
	&dev_attr_model.attr,
	&dev_attr_description.attr,
	&dev_attr_resolution.attr,
	&dev_attr_physical_size.attr,
	&dev_attr_version.attr,
	&dev_attr_screen_id.attr,
	&dev_attr_device_id.attr,
	&dev_attr_margin.attr,
	&dev_attr_fbdev_enabled.attr,
	&dev_attr_lz4_level.attr,
	&dev_attr_firmware.attr,
	&dev_attr_fw_minor.attr,
	&dev_attr_fw_major.attr,
	&dev_attr_stats.attr,
	NULL,
};

static const struct attribute_group mpro_attr_group = {
	.attrs = mpro_attrs,
};

int mpro_sysfs_add(struct mpro_device *mpro)
{
	return devm_device_add_group(&mpro->intf->dev, &mpro_attr_group);
}
