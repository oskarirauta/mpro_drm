// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_drm_main.c — DRM driver registration, probe, remove.
 *
 * Sisältää platform_driver-rakenteen ja itse drm_driver-määrittelyn.
 * Pipe/connector-callbackit, värilogiikka, sysfs ja EDID ovat omissa
 * tiedostoissaan.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_managed.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_client.h>
#include <drm/drm_vblank.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_edid.h>

#include "mpro_drm.h"

/* ------------------------------------------------------------------ */
/* Module parameters                                                  */
/* ------------------------------------------------------------------ */

static bool mpro_drm__disable_partial_default;
module_param_named(disable_partial, mpro_drm__disable_partial_default, bool,
		   0644);
MODULE_PARM_DESC(disable_partial,
		 "Disable partial screen updates (force full-frame). "
		 "Required on firmware versions that don't support partial "
		 "rendering. Default: 0 (partial enabled).");

/* ------------------------------------------------------------------ */
/* drm_driver                                                         */
/* ------------------------------------------------------------------ */

static int mpro_drm__dumb_create(struct drm_file *file,
				 struct drm_device *dev,
				 struct drm_mode_create_dumb *args)
{
	if (args->bpp != 16 && args->bpp != 32)
		return -EINVAL;

	if (args->bpp == 16)
		args->pitch = args->width * 2;
	else
		args->pitch = args->width * 4;

	args->size = args->pitch * args->height;

	return drm_gem_shmem_dumb_create(file, dev, args);
}

DEFINE_DRM_GEM_FOPS(mpro_drm__fops);

static struct drm_driver mpro_drm__driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &mpro_drm__fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.dumb_create = mpro_drm__dumb_create,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

/* ------------------------------------------------------------------ */
/* PM ops                                                             */
/* ------------------------------------------------------------------ */

static int mpro_drm__suspend(struct mpro_drm *mdrm)
{
	if (mdrm->suspended)
		return 0;

	mdrm->vblank_was_enabled_pre_suspend = READ_ONCE(mdrm->vblank_enabled);

	if (mdrm->vblank_was_enabled_pre_suspend) {
		WRITE_ONCE(mdrm->vblank_enabled, false);
		hrtimer_cancel(&mdrm->vblank_timer);
	}

	memset(mdrm->data, 0, mdrm->data_size);
	mpro_drm__request_update(mdrm, 0, 0, mdrm->width, mdrm->height, false);

	mdrm->suspended = true;
	return 0;
}

static int mpro_drm__resume(struct mpro_drm *mdrm)
{
	if (!mdrm->suspended)
		return 0;

	mdrm->suspended = false;

	mpro_drm__request_update(mdrm, 0, 0, mdrm->width, mdrm->height, false);

	if (mdrm->vblank_was_enabled_pre_suspend) {
		WRITE_ONCE(mdrm->vblank_enabled, true);
		hrtimer_start(&mdrm->vblank_timer, mdrm->frame_time,
			      HRTIMER_MODE_REL);
	}
	return 0;
}

static int mpro_drm__pm_suspend(struct device *dev)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);

	return mdrm ? mpro_drm__suspend(mdrm) : 0;
}

static int mpro_drm__pm_resume(struct device *dev)
{
	struct mpro_drm *mdrm = dev_get_drvdata(dev);

	return mdrm ? mpro_drm__resume(mdrm) : 0;
}

static const struct dev_pm_ops mpro_drm__pm_ops = {
	.suspend = mpro_drm__pm_suspend,
	.resume = mpro_drm__pm_resume,
	.runtime_suspend = mpro_drm__pm_suspend,
	.runtime_resume = mpro_drm__pm_resume,
};

/* ------------------------------------------------------------------ */
/* Format support                                                     */
/* ------------------------------------------------------------------ */

static const uint32_t mpro_drm__formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const uint64_t mpro_drm__pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/* ------------------------------------------------------------------ */
/* Data buffer allocation                                             */
/* ------------------------------------------------------------------ */

static int mpro_drm__data_alloc(struct mpro_drm *mdrm)
{
	size_t block_size = (size_t)mdrm->mpro->model->width *
	    (size_t)mdrm->mpro->model->height *
	    MPRO_BPP / 8 + (size_t)mdrm->mpro->model->margin;

	mdrm->data = drmm_kmalloc(&mdrm->drm, block_size, GFP_KERNEL);
	if (!mdrm->data)
		return -ENOMEM;

	/*
	 * Margin-tavut näytön lopussa: laite vaatii että ne ovat nollia.
	 * Kirjoitamme nollat kerran, ja koska DRM koskee vain kuva-alueeseen
	 * (width*height*2 ensimmäistä tavua), margin pysyy nollissa.
	 */
	memset(mdrm->data, 0, block_size);
	mdrm->data_size = block_size;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int mpro_drm__probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpro_device *mpro;
	struct mpro_drm *mdrm;
	struct drm_device *drm;
	bool swap;
	int ret;

	mpro = dev_get_drvdata(dev->parent);
	if (!mpro || !mpro->model) {
		DRM_ERROR("Failed to get parent mpro instance or model data\n");
		return -ENODEV;
	}

	mdrm = devm_drm_dev_alloc(dev, &mpro_drm__driver, struct mpro_drm, drm);
	if (IS_ERR(mdrm)) {
		DRM_ERROR("Failed to allocate memory for drm driver\n");
		return PTR_ERR(mdrm);
	}

	drm = &mdrm->drm;

	mdrm->mpro = mpro;
	mdrm->pdev = pdev;
	mdrm->dev = dev;
	mdrm->rotation = mpro->model->native_rotation;
	mdrm->frame_time = ktime_set(0, NSEC_PER_SEC / 60);
	mdrm->brightness = MPRO_BRIGHTNESS_DEFAULT;
	mdrm->gamma_x100 = 100;
	mdrm->blanked = false;
	mdrm->disable_partial = mpro->model->margin > 0
	    ? true : mpro_drm__disable_partial_default;

	swap = mpro_drm__rotation_swaps_dims(mdrm->rotation);
	mdrm->width = swap ? mpro->model->height : mpro->model->width;
	mdrm->height = swap ? mpro->model->width : mpro->model->height;

	if (mpro->model->margin > 0)
		drm_info(drm,
			 "Device with margin detected, partial updates disabled\n");

	platform_set_drvdata(pdev, mdrm);

	ret = drm_mode_config_init(drm);
	if (ret) {
		DRM_ERROR("Failed to initialize mode configuration\n");
		return ret;
	}

	ret = mpro_drm__data_alloc(mdrm);
	if (ret) {
		drm_err(drm, "Failed to allocate frame buffer\n");
		return ret;
	}

	drm->mode_config.min_width = mdrm->width;
	drm->mode_config.max_width = mdrm->width;
	drm->mode_config.min_height = mdrm->height;
	drm->mode_config.max_height = mdrm->height;
	drm->mode_config.funcs = &mpro_drm__mode_config_funcs;
	drm->mode_config.preferred_depth = 16;
	drm->mode_config.prefer_shadow = 1;
	drm->mode_config.cursor_width = 0;
	drm->mode_config.cursor_height = 0;

	/*
	 * Älä aseta drm->vblank_disable_immediate = true. Hrtimer-pohjaisessa
	 * vblankissa se aiheuttaa race:n kun disable_vblank-callback peruuttaa
	 * timerin samaan aikaan kun se on juuri laukeamassa. Käytetään
	 * oletusta: 5 sekunnin viive ennen vblank-sammutusta. Hyvin pieni
	 * energianhukka, mutta vakaata.
	 */

	drm_connector_helper_add(&mdrm->connector,
				 &mpro_drm__connector_helper_funcs);

	ret = drm_connector_init(drm, &mdrm->connector,
				 &mpro_drm__connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		drm_err(drm, "Failed to initialize connector: %d\n", ret);
		return ret;
	}

	mdrm->connector.status = connector_status_connected;
	mdrm->connector.polled = 0;	/* USB-näyttö on aina kytkettynä */

	ret =
	    drm_simple_display_pipe_init(drm, &mdrm->pipe,
					 &mpro_drm__pipe_funcs,
					 mpro_drm__formats,
					 ARRAY_SIZE(mpro_drm__formats),
					 mpro_drm__pipe_modifiers,
					 &mdrm->connector);
	if (ret) {
		drm_err(drm, "Failed to initialize pipe: %d\n", ret);
		return ret;
	}

	drm_plane_enable_fb_damage_clips(&mdrm->pipe.plane);

	ret =
	    drm_plane_create_rotation_property(&mdrm->pipe.plane,
					       DRM_MODE_ROTATE_0,
					       DRM_MODE_ROTATE_0 |
					       DRM_MODE_ROTATE_90 |
					       DRM_MODE_ROTATE_180 |
					       DRM_MODE_ROTATE_270);
	if (ret)
		drm_warn(drm, "Failed to create rotation property: %d\n", ret);

	ret = drm_vblank_init(drm, 1);
	if (ret)
		drm_warn(drm, "Failed to initialize vblank: %d\n", ret);

	hrtimer_setup(&mdrm->vblank_timer, mpro_drm__vblank_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	/*
	 * BUG FIX: älä luo gamma_lut_property käsin ennen
	 * drm_crtc_enable_color_mgmt-kutsua. drm_crtc_enable_color_mgmt
	 * luo propertyt itse; manuaalinen luonti johti duplikaatteihin
	 * ja rikki olevaan tilarakenteeseen.
	 */
	drm_crtc_enable_color_mgmt(&mdrm->pipe.crtc, 0, false, 256);

	mdrm->brightness_prop = drm_property_create_range(drm, 0, "brightness",
							  MPRO_BRIGHTNESS_MIN,
							  MPRO_BRIGHTNESS_MAX);
	if (mdrm->brightness_prop)
		drm_object_attach_property(&mdrm->connector.base,
					   mdrm->brightness_prop,
					   MPRO_BRIGHTNESS_DEFAULT);

	drm_format_conv_state_init(&mdrm->conv_state);
	mpro_drm__rebuild_combined_lut(mdrm);

	drm_connector_attach_edid_property(&mdrm->connector);

	ret = mpro_drm__create_edid(mdrm);
	if (ret)
		drm_warn(drm, "EDID creation failed: %d\n", ret);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm_err(drm, "Failed to register drm device: %d\n", ret);
		drm_format_conv_state_release(&mdrm->conv_state);
		return ret;
	}

	ret = mpro_drm__sysfs_create(mdrm);
	if (ret)
		drm_warn(drm, "Failed to create sysfs attributes: %d\n", ret);

	if (mdrm->mpro->fbdev_enabled)
		drm_client_setup(drm, NULL);

	mpro->drm = mdrm;
	return 0;
}

static void mpro_drm__remove(struct platform_device *pdev)
{
	struct mpro_drm *mdrm = platform_get_drvdata(pdev);

	if (!mdrm) {
		DRM_ERROR("Failed to get mpro_drm instance on removal\n");
		return;
	}

	mpro_drm__sysfs_remove(mdrm);

	mdrm->vblank_enabled = false;
	hrtimer_cancel(&mdrm->vblank_timer);

	if (mdrm->mpro)
		WRITE_ONCE(mdrm->mpro->drm, NULL);

	drm_format_conv_state_release(&mdrm->conv_state);

	/* unplug ensin (estää uudet operaatiot), sitten shutdown */
	drm_dev_unplug(&mdrm->drm);
	drm_atomic_helper_shutdown(&mdrm->drm);
}

/* ------------------------------------------------------------------ */

static struct platform_driver mpro_drm__platform_driver = {
	.probe = mpro_drm__probe,
	.remove = mpro_drm__remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .pm = &mpro_drm__pm_ops,
		    },
};

module_platform_driver(mpro_drm__platform_driver);

MODULE_AUTHOR("Oskari Rauta");
MODULE_DESCRIPTION("MPro DRM Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
