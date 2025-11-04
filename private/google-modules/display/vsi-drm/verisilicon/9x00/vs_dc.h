/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_DC_H__
#define __VS_DC_H__

#include <linux/debugfs.h>
#include <linux/devfreq.h>
#include <linux/mm_types.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <drm/drm_modes.h>
#include <drm/drm_color_mgmt.h>

#include "vs_crtc.h"
#include "vs_dc_hw.h"
#include "vs_plane.h"
#include "vs_writeback.h"
#include "preprocess/vs_dc_pvric.h"

static inline u8 to_vs_rotation(u32 rotation)
{
	u8 rot;

	switch (rotation & (DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK)) {
	case DRM_MODE_ROTATE_0:
		rot = ROT_0;
		break;
	case DRM_MODE_ROTATE_90:
		rot = ROT_90;
		break;
	case DRM_MODE_ROTATE_180:
		rot = ROT_180;
		break;
	case DRM_MODE_ROTATE_270:
		rot = ROT_270;
		break;
	case DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_0:
		rot = FLIP_X;
		break;
	case DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_0:
		rot = FLIP_Y;
		break;
	/* b/318783657 - FLIPX_90 and FLIPY_90 are unsupported in HW and behave as a no-op*/
	case DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_90:
		rot = FLIPX_90;
		break;
	case DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_90:
		rot = FLIPY_90;
		break;
	default:
		rot = ROT_0;
		break;
	}

	return rot;
}

static inline u8 to_vs_yuv_gamut(u32 color_space)
{
	u8 gamut;

	switch (color_space) {
	case DRM_COLOR_YCBCR_BT601:
		gamut = CSC_GAMUT_601;
		break;
	case DRM_COLOR_YCBCR_BT709:
		gamut = CSC_GAMUT_709;
		break;
	case DRM_COLOR_YCBCR_BT2020:
		gamut = CSC_GAMUT_2020;
		break;
	default:
		gamut = CSC_GAMUT_2020;
		break;
	}

	return gamut;
}

struct vs_dc_plane {
	enum dc_hw_plane_id id;
	struct vs_plane *base;
	struct dc_pvric pvric;
};

struct vs_dc {
	/* TBD !
	 * developer should modify this item according to
	 * actual requirements during developing !
	 */
	struct vs_crtc *crtc[DC_DISPLAY_NUM];
	struct dc_hw hw;

	struct devfreq *core_devfreq;
	struct devfreq *fabrt_devfreq;
	int irq_num;
	int *irqs;
	int irq_enable_count;
	spinlock_t int_lock;
	struct mutex dc_lock; /* protect state and data */
	struct mutex dc_qos_lock;

	bool first_frame;
	bool enabled;

	struct vs_dc_plane planes[DC_PLANE_NUM];

	int num_pds;
	struct {
		struct device *dev;
		struct device_link *devlink;
	} *pds;

	struct vs_writeback_connector *writeback[DC_WB_NUM];

	struct dentry *debugfs;
	struct google_icc_path *path;
	struct platform_device *tzprot_pdev;
	struct vs_qos_config min_qos_config;
	struct vs_fe_qos_config fe_qos_config;

	u32 crtc_mask;

	bool disable_hw_reset;
	enum dc_hw_reg_dump_options hw_reg_dump_options;
	/** @disable_crtc_recovery: whether to disable crtc recovery on flip_done timeout */
	bool disable_crtc_recovery;
	/** @disable_urgent: whether to disable QoS urgent level feature */
	bool disable_urgent;

	/** @boost_fabrt_freq: define the value to boost FABRT frequency */
	u32 boost_fabrt_freq;
};

extern struct platform_driver dc_platform_driver;

static inline struct vs_dc *to_vs_dc(const struct dc_hw *hw)
{
	return container_of(hw, struct vs_dc, hw);
}

int vs_sw_reset_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int vs_get_feature_cap_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int vs_get_hw_cap_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int vs_get_hist_bins_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int vs_get_ltm_hist_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
bool vs_dc_is_yuv_format(u32 format);
void vs_dc_check_interrupts(struct device *dev);
int vs_dc_power_get(struct device *dev, bool sync);
int vs_dc_power_put(struct device *dev, bool sync);
bool is_display_cmd_sw_trigger(struct dc_hw_display *display);

#if IS_ENABLED(CONFIG_DEBUG_FS)
void vs_crtc_set_last_crc(u32 crtc_id, struct drm_vs_color vaule);
#endif

#endif /* __VS_DC_H__ */
