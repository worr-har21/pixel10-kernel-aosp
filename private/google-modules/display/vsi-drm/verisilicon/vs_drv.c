// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#include <linux/component.h>
#include <linux/iommu.h>
#include <linux/of_graph.h>

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_of.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <trace/dpu_trace.h>

#include "vs_dc.h"
#include "vs_dc_pre.h"
#include "vs_dc_post.h"
#include "vs_drm_atomic.h"
#include "vs_drv.h"
#include "vs_gem.h"
#include "vs_simple_enc.h"

#define DRV_NAME "vs-drm"
#define DRV_DESC "VeriSilicon DRM driver"
#define DRV_DATE "20191101"
#define DRV_MAJOR 1
#define DRV_MINOR 0

static bool has_iommu = true;
struct drm_device *dev_drm;

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.mmap = vs_gem_mmap,
};

static const struct drm_ioctl_desc vs_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VS_GET_FBC_OFFSET, vs_get_fbc_offset_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_SW_RESET, vs_sw_reset_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_GEM_QUERY, vs_gem_query_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_GET_FEATURE_CAP, vs_get_feature_cap_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_GET_HW_CAP, vs_get_hw_cap_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_GET_HIST_BINS, vs_get_hist_bins_query_ioctl, DRM_MASTER),
	DRM_IOCTL_DEF_DRV(VS_GET_LTM_HIST, vs_get_ltm_hist_ioctl, DRM_MASTER),
};

static void vs_drm_lastclose(struct drm_device *dev)
{
	drm_atomic_helper_shutdown(dev);
	drm_mode_config_reset(dev);
}

static struct drm_driver vs_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,
	.lastclose = vs_drm_lastclose,
	.gem_prime_import = vs_gem_prime_import,
	.gem_prime_import_sg_table = vs_gem_prime_import_sg_table,
	.dumb_create = vs_gem_dumb_create,
	.ioctls = vs_ioctls,
	.num_ioctls = ARRAY_SIZE(vs_ioctls),
	.fops = &fops,
	.name = DRV_NAME,
	.desc = DRV_DESC,
	.date = DRV_DATE,
	.major = DRV_MAJOR,
	.minor = DRV_MINOR,
};

int vs_drm_iommu_attach_device(struct drm_device *drm_dev, struct device *dev)
{
	struct vs_drm_private *priv = drm_dev->dev_private;
	int ret;

	if (!has_iommu)
		return 0;

	if (!priv->domain) {
		priv->domain = iommu_get_domain_for_dev(dev);
		if (!priv->domain)
			return -EINVAL;
		priv->dma_dev = dev;
	}

	ret = iommu_attach_device(priv->domain, dev);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to attach iommu device\n");
		return ret;
	}

	return 0;
}

void vs_drm_iommu_detach_device(struct drm_device *drm_dev, struct device *dev)
{
	struct vs_drm_private *priv = drm_dev->dev_private;

	if (!has_iommu)
		return;

	iommu_detach_device(priv->domain, dev);

	if (priv->dma_dev == dev)
		priv->dma_dev = drm_dev->dev;
	priv->domain = NULL;
}

void vs_drm_update_alignment(struct drm_device *drm_dev, unsigned int pitch_align,
			     unsigned int addr_align)
{
	struct vs_drm_private *priv = drm_dev->dev_private;

	if (pitch_align > priv->pitch_alignment)
		priv->pitch_alignment = pitch_align;

	if (addr_align > priv->addr_alignment)
		priv->addr_alignment = addr_align;
}

static const struct drm_mode_config_funcs vs_mode_config_funcs = {
	.fb_create = vs_fb_create,
	.get_format_info = vs_get_format_info,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = vs_drm_atomic_check,
	.atomic_commit = vs_drm_atomic_commit,
};

static struct drm_mode_config_helper_funcs vs_mode_config_helpers = {
	.atomic_commit_tail = vs_drm_atomic_commit_tail,
};

static void vs_mode_config_init(struct drm_device *dev)
{
	if (dev->mode_config.max_width == 0 || dev->mode_config.max_height == 0) {
		dev->mode_config.min_width = 0;
		dev->mode_config.min_height = 0;
		dev->mode_config.max_width = 4096;
		dev->mode_config.max_height = 4096;
	}
	dev->mode_config.funcs = &vs_mode_config_funcs;
	dev->mode_config.helper_private = &vs_mode_config_helpers;
	dev->mode_config.normalize_zpos = true;
}

/* platfrom driver */
static int vs_drm_bind(struct device *dev)
{
	struct drm_device *drm_dev;
	struct vs_drm_private *priv;
	int ret;

	drm_dev = drm_dev_alloc(&vs_drm_driver, dev);
	if (IS_ERR(drm_dev))
		return PTR_ERR(drm_dev);

	dev_set_drvdata(dev, drm_dev);

	priv = devm_kzalloc(drm_dev->dev, sizeof(struct vs_drm_private), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_put_dev;
	}

	priv->pitch_alignment = 64;
	priv->addr_alignment = 128;
	priv->dma_dev = drm_dev->dev;

	drm_dev->dev_private = priv;

	drm_mode_config_init(drm_dev);

	/* Now try and bind all our sub-components */
	ret = component_bind_all(dev, drm_dev);
	if (ret)
		goto err_mode;

	vs_mode_config_init(drm_dev);

	ret = drm_vblank_init(drm_dev, drm_dev->mode_config.num_crtc);
	if (ret)
		goto err_bind;

	drm_mode_config_reset(drm_dev);

	drm_kms_helper_poll_init(drm_dev);

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto err_helper;

	drm_fbdev_generic_setup(drm_dev, 32);

	return 0;

err_helper:
	drm_kms_helper_poll_fini(drm_dev);
err_bind:
	component_unbind_all(drm_dev->dev, drm_dev);
err_mode:
	drm_mode_config_cleanup(drm_dev);
err_put_dev:
	drm_dev->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
	drm_dev_put(drm_dev);
	return ret;
}

static void vs_drm_unbind(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);

	drm_dev_unregister(drm_dev);

	drm_kms_helper_poll_fini(drm_dev);

	component_unbind_all(drm_dev->dev, drm_dev);

	drm_mode_config_cleanup(drm_dev);

	drm_dev->dev_private = NULL;
	dev_set_drvdata(dev, NULL);
	drm_dev_put(drm_dev);
}

static const struct component_master_ops vs_drm_ops = {
	.bind = vs_drm_bind,
	.unbind = vs_drm_unbind,
};

static struct platform_driver vs_drm_platform_driver;

#if IS_ENABLED(CONFIG_VERISILICON_MIPI_DSI2H)
extern struct platform_driver vs_mipi_dsi2h_driver;
#endif
#if IS_ENABLED(CONFIG_VERISILICON_DISPLAYPORT)
extern struct platform_driver vs_dp_driver;
#endif

static struct platform_driver *drm_sub_drivers[] = {
	/* put display control driver at start */
	&dc_platform_driver,
	&dc_be_platform_driver,
	&dc_fe0_platform_driver,
	&dc_fe1_platform_driver,
	&dc_wb_platform_driver,

/* bridge */
#if IS_ENABLED(CONFIG_VERISILICON_MIPI_DSI2H)
	&vs_mipi_dsi2h_driver,
#endif
	/* encoder */
#if IS_ENABLED(CONFIG_VERISILICON_DISPLAYPORT)
	&vs_dp_driver,
#else
	&simple_encoder_driver,
#endif
};

#define NUM_DRM_DRIVERS (sizeof(drm_sub_drivers) / sizeof(struct platform_driver *))

static int compare_dev(struct device *dev, void *data)
{
	return dev == (struct device *)data;
}

static struct component_match *vs_drm_match_add(struct device *dev)
{
	struct component_match *match = NULL;
	int i;

	for (i = 0; i < NUM_DRM_DRIVERS; ++i) {
		struct platform_driver *drv = drm_sub_drivers[i];
		struct device *p = NULL, *d;

		while ((d = platform_find_device_by_driver(p, &drv->driver))) {
			put_device(p);

			component_match_add(dev, &match, compare_dev, d);
			p = d;
		}
		put_device(p);
	}

	return match ?: ERR_PTR(-ENODEV);
}

static int vs_drm_platform_of_probe(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct device_node *port;
	bool found = false;
	int i;

	if (!np)
		return -ENODEV;

	for (i = 0;; i++) {
		struct device_node *iommu;

		port = of_parse_phandle(np, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		iommu = of_parse_phandle(port->parent, "iommus", 0);

		/*
		 * if there is a crtc not support iommu, force set all
		 * crtc use non-iommu buffer.
		 */
		if (!iommu || !of_device_is_available(iommu->parent))
			has_iommu = false;

		found = true;

		of_node_put(iommu);
		of_node_put(port);
	}

	if (i == 0) {
		DRM_DEV_ERROR(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!found) {
		DRM_DEV_ERROR(dev, "No available DC found.\n");
		return -ENODEV;
	}

	return 0;
}

static int vs_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match;
	int ret;

	ret = vs_drm_platform_of_probe(dev);
	if (ret)
		return ret;

	match = vs_drm_match_add(dev);
	if (IS_ERR(match))
		return PTR_ERR(match);

	return component_master_add_with_match(dev, &vs_drm_ops, match);
}

static int vs_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &vs_drm_ops);
	return 0;
}

static void vs_drm_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (!drm)
		return;

	drm_atomic_helper_shutdown(drm);
}

#if IS_ENABLED(CONFIG_PM_SLEEP)
static int vs_drm_atomic_suspend(struct drm_device *dev)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	int ret;

	DPU_ATRACE_BEGIN(__func__);

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);

	state = drm_atomic_helper_duplicate_state(dev, &ctx);
	if (IS_ERR(state))
		ret = PTR_ERR(state);
	else
		ret = vs_drm_atomic_disable_all(dev, &ctx);

	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);

	if (ret > 0) /* some crtcs were disabled */
		dev->mode_config.suspend_state = state;
	else /* nothing was disabled */
		drm_atomic_state_put(state);

	DPU_ATRACE_END(__func__);

	return ret;
}

static int vs_drm_suspend(struct drm_device *dev)
{
	int ret;

	if (!dev)
		return 0;

	DPU_ATRACE_BEGIN(__func__);
	/*
	 * Don't disable polling if it was never initialized
	 */
	if (dev->mode_config.poll_enabled)
		drm_kms_helper_poll_disable(dev);

	drm_fb_helper_set_suspend_unlocked(dev->fb_helper, 1);

	ret = vs_drm_atomic_suspend(dev);
	if (ret) {
		drm_fb_helper_set_suspend_unlocked(dev->fb_helper, 0);
		/*
		 * Don't enable polling if it was never initialized
		 */
		if (dev->mode_config.poll_enabled)
			drm_kms_helper_poll_enable(dev);
	}
	DPU_ATRACE_END(__func__);

	return ret;
}

static int vs_drm_resume(struct drm_device *dev)
{
	int ret = 0;

	if (!dev)
		return 0;

	DPU_ATRACE_BEGIN(__func__);
	if (dev->mode_config.suspend_state) {
		ret = drm_atomic_helper_resume(dev, dev->mode_config.suspend_state);
		if (ret)
			drm_err(dev, "Failed to resume (%d)\n", ret);

		dev->mode_config.suspend_state = NULL;
	}

	drm_fb_helper_set_suspend_unlocked(dev->fb_helper, 0);
	/*
	 * Don't enable polling if it is not initialized
	 */
	if (dev->mode_config.poll_enabled)
		drm_kms_helper_poll_enable(dev);

	DPU_ATRACE_END(__func__);

	return ret;
}

static int vs_drm_prepare(struct device *dev)
{
	int ret;
	struct drm_device *drm = dev_get_drvdata(dev);

	DPU_ATRACE_BEGIN(__func__);
	dev_dbg(dev, "suspend drm mode config\n");

	ret = vs_drm_suspend(drm);
	if (ret < 0)
		DRM_DEV_ERROR(dev, "failed to config helper suspend.\n");

	DPU_ATRACE_END(__func__);
	return ret;
}

static void vs_drm_complete(struct device *dev)
{
	int ret;
	struct drm_device *drm = dev_get_drvdata(dev);

	dev_dbg(dev, "resume drm mode config\n");
	DPU_ATRACE_BEGIN(__func__);
	ret = vs_drm_resume(drm);
	if (ret < 0)
		DRM_DEV_ERROR(dev, "failed to config helper resume.\n");

	DPU_ATRACE_END(__func__);
}

static const struct dev_pm_ops vs_drm_pm_ops = {
	.prepare = vs_drm_prepare,
	.complete = vs_drm_complete,
};
#endif

static const struct of_device_id vs_drm_dt_ids[] = {

	{
		.compatible = "verisilicon,display-subsystem",
	},

	{ /* sentinel */ },

};

MODULE_DEVICE_TABLE(of, vs_drm_dt_ids);

static struct platform_driver vs_drm_platform_driver = {
	.probe = vs_drm_platform_probe,
	.remove = vs_drm_platform_remove,
	.shutdown = vs_drm_platform_shutdown,

	.driver = {
		.name = DRV_NAME,
		.of_match_table = vs_drm_dt_ids,
#if IS_ENABLED(CONFIG_PM_SLEEP)
		.pm = &vs_drm_pm_ops,
#endif
	},
};

static int __init vs_drm_init(void)
{
	int ret;
	ret = platform_register_drivers(drm_sub_drivers, NUM_DRM_DRIVERS);
	if (ret)
		return ret;

	ret = platform_driver_register(&vs_drm_platform_driver);
	if (ret)
		platform_unregister_drivers(drm_sub_drivers, NUM_DRM_DRIVERS);
	return ret;
}

static void __exit vs_drm_exit(void)
{
	platform_driver_unregister(&vs_drm_platform_driver);
	platform_unregister_drivers(drm_sub_drivers, NUM_DRM_DRIVERS);
}

module_init(vs_drm_init);
module_exit(vs_drm_exit);

MODULE_DESCRIPTION("VeriSilicon DRM Driver");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DMA_BUF);
