// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode.h>
#include <drm/vs_drm_fourcc.h>
#include <drm/g2d_drm.h>

#include "g2d_sc.h"
#include "g2d_sc_hw.h"
#include "g2d_drv.h"
#include "g2d_plane.h"
#include "g2d_plane_hw.h"
#include "g2d_crtc.h"
#include "g2d_writeback.h"
#include "g2d_writeback_hw.h"

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

static inline void update_format(u32 format, struct sc_hw_fb *fb)
{
	u8 f = FORMAT_A8R8G8B8;

	switch (format) {
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_BGRX4444:
		f = FORMAT_X4R4G4B4;
		break;
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_BGRA4444:
		f = FORMAT_A4R4G4B4;
		break;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_BGRX5551:
		f = FORMAT_X1R5G5B5;
		break;
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_BGRA5551:
		f = FORMAT_A1R5G5B5;
		break;
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
		f = FORMAT_R5G6B5;
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_BGRX8888:
		f = FORMAT_X8R8G8B8;
		break;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_BGRA8888:
		f = FORMAT_A8R8G8B8;
		break;
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRA1010102:
		f = FORMAT_A2R10G10B10;
		break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_BGRX1010102:
		f = FORMAT_X2R10G10B10;
		break;
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		f = FORMAT_A16R16G16B16;
		break;
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		f = FORMAT_YV12;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		f = FORMAT_NV12;
		break;
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
		f = FORMAT_NV16;
		break;
	case DRM_FORMAT_P010:
		f = FORMAT_P010;
		break;
	case DRM_FORMAT_P210:
		f = FORMAT_P210;
		break;
	default:
		break;
	}

	fb->format = f;
}

static inline void update_wb_format(u32 format, struct sc_hw_fb *fb)
{
	u8 f = WB_FORMAT_RGB888;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_BGRA8888:
		f = WB_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_BGRX8888:
		f = WB_FORMAT_XRGB8888;
		break;
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRA1010102:
		f = WB_FORMAT_A2RGB101010;
		break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_BGRX1010102:
		f = WB_FORMAT_X2RGB101010;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		f = WB_FORMAT_NV12;
		break;
	case DRM_FORMAT_P010:
		f = WB_FORMAT_P010;
		break;
	default:
		break;
	}

	fb->format = f;
}

static inline void update_swizzle(u32 format, struct sc_hw_fb *fb)
{
	fb->swizzle = SWIZZLE_ARGB;
	fb->uv_swizzle = 0;

	switch (format) {
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
		fb->swizzle = SWIZZLE_RGBA;
		break;
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR16161616F:
		fb->swizzle = SWIZZLE_ABGR;
		break;
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_BGRA5551:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
		fb->swizzle = SWIZZLE_BGRA;
		break;
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_YVU420:
		fb->uv_swizzle = 1;
		break;
	default:
		break;
	}
}

static inline void update_tile_mode(u64 modifier, struct sc_hw_fb *fb)
{
	u8 norm_mode, tile = TILE_MODE_LINEAR;

	norm_mode = modifier & DRM_FORMAT_MOD_VS_NORM_MODE_MASK;

	switch (norm_mode) {
	case DRM_FORMAT_MOD_VS_TILE_16X4:
		tile = TILE_MODE_16X4;
		break;
	case DRM_FORMAT_MOD_VS_TILE_16X8:
		tile = TILE_MODE_16X8;
		break;
	case DRM_FORMAT_MOD_VS_TILE_32X2:
		tile = TILE_MODE_32X2;
		break;
	case DRM_FORMAT_MOD_VS_TILE_32X4:
		tile = TILE_MODE_32X4;
		break;
	case DRM_FORMAT_MOD_VS_TILE_32X8:
		tile = TILE_MODE_32X8;
		break;
	case DRM_FORMAT_MOD_VS_TILE_16X16:
		tile = TILE_MODE_16X16;
		break;
	default:
		break;
	}

	fb->tile_mode = tile;
}

static inline void update_wb_tile_mode(u64 modifier, struct sc_hw_fb *fb)
{
	u8 norm_mode, tile = WB_TILE_MODE_LINEAR;

	norm_mode = modifier & DRM_FORMAT_MOD_VS_NORM_MODE_MASK;
	if (norm_mode == DRM_FORMAT_MOD_VS_TILE_16X16)
		tile = WB_TILE_MODE_16X16;

	fb->tile_mode = tile;
}

static void update_plane_fb(struct g2d_sc *sc, struct g2d_plane *plane, u8 display_id,
			    struct sc_hw_fb *fb)
{
	struct drm_plane_state *state = plane->base.state;
	struct drm_framebuffer *drm_fb = state->fb;

	fb->display_id = display_id;
	fb->address = (u64)plane->dma_addr[0];
	fb->stride = drm_fb->pitches[0];
	fb->u_address = (u64)plane->dma_addr[1];
	fb->v_address = (u64)plane->dma_addr[2];
	fb->u_stride = drm_fb->pitches[1];
	fb->v_stride = drm_fb->pitches[2];
	fb->width = drm_fb->width;
	fb->height = drm_fb->height;
	fb->rotation = to_vs_rotation(state->rotation);
	fb->zpos = state->normalized_zpos;
	fb->enable = state->visible;
	update_format(drm_fb->format->format, fb);
	update_swizzle(drm_fb->format->format, fb);
	update_tile_mode(drm_fb->modifier, fb);

	sc_hw_update_plane(&sc->hw, plane->id, fb);
}

static void update_wb_fb(struct g2d_sc *sc, u32 display_id,
			 struct g2d_writeback_connector *wb_connector, struct drm_framebuffer *fb)
{
	struct sc_hw_fb wb_fb = { 0 };

	wb_fb.display_id = display_id;
	wb_fb.address = (u64)wb_connector->dma_addr[0];
	wb_fb.u_address = (u64)wb_connector->dma_addr[1];
	wb_fb.v_address = (u64)wb_connector->dma_addr[2];
	wb_fb.stride = wb_connector->pitch[0];
	wb_fb.u_stride = wb_connector->pitch[1];
	wb_fb.v_stride = wb_connector->pitch[2];
	wb_fb.width = fb->width;
	wb_fb.height = fb->height;
	wb_fb.tile_mode = 0;
	wb_fb.enable = true;
	update_wb_format(fb->format->format, &wb_fb);
	update_swizzle(fb->format->format, &wb_fb);
	update_wb_tile_mode(fb->modifier, &wb_fb);

	sc_hw_update_wb_fb(&sc->hw, wb_connector->id, &wb_fb);
}

static void update_roi(struct g2d_sc *sc, u8 id, struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *drm_fb = plane_state->fb;
	struct sc_hw_roi roi = { 0 };

	roi.in_rect.x = plane_state->src.x1 >> 16;
	roi.in_rect.y = plane_state->src.y1 >> 16;
	roi.in_rect.w = drm_rect_width(&plane_state->src) >> 16;
	roi.in_rect.h = drm_rect_height(&plane_state->src) >> 16;

	roi.mode = (roi.in_rect.w >> 16 != drm_fb->width || roi.in_rect.h >> 16 != drm_fb->height) ?
			   G2D_DMA_ONE_ROI :
			   G2D_DMA_NORMAL;

	dev_dbg(sc->hw.dev, "%s: computed input Roi as: %dx%d @ %d,%d for hw_id %d", __func__,
		roi.in_rect.w, roi.in_rect.h, roi.in_rect.x, roi.in_rect.y, id);

	sc_hw_update_plane_roi(&sc->hw, id, &roi);
}

static void update_plane_y2r(struct g2d_sc *sc, u8 id, struct drm_plane_state *plane_state,
			     struct drm_atomic_state *state)
{
	struct sc_hw_y2r y2r_conf = { 0 };

	struct device *dev = sc->hw.dev;
	bool is_plane_yuv = plane_state->fb->format->is_yuv;
	bool is_wb_yuv = sc->writeback[id]->is_yuv;

	dev_dbg(sc->hw.dev, "%s: is plane yuv: %d, is_wb_yuv: %d", __func__, is_plane_yuv,
		is_wb_yuv);

	if (is_plane_yuv == is_wb_yuv)
		y2r_conf.enable = false;
	else if (is_plane_yuv)
		y2r_conf.enable = true;
	else
		dev_err(dev, "%s: R2Y not supported yet!", __func__);

	y2r_conf.gamut = to_vs_yuv_gamut(plane_state->color_encoding);

	if (plane_state->color_range == DRM_COLOR_YCBCR_FULL_RANGE)
		y2r_conf.mode = CSC_MODE_F2F;
	else
		y2r_conf.mode = CSC_MODE_L2F;

	dev_dbg(dev, "%s: color encoding %d. y2r gamut: %d. y2r mode: %d for hw_id %d", __func__,
		plane_state->color_encoding, y2r_conf.gamut, y2r_conf.mode, id);
	sc_hw_update_plane_y2r(&sc->hw, id, &y2r_conf);
}

static void populate_layer_scale(const struct drm_plane_state *plane_state,
				 struct sc_hw_scale *scale)
{
	scale->src_w = drm_rect_width(&plane_state->src);
	scale->src_h = drm_rect_height(&plane_state->src);
	scale->dst_w = drm_rect_width(&plane_state->dst);
	scale->dst_h = drm_rect_height(&plane_state->dst);

	if (plane_state->rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270))
		swap(scale->src_w, scale->src_h);

	/*
	 * Since src_w/h are in 16.16 and crtc_w/h are plain ints, this division will
	 * result in 16.16 output. Note that subtracting 1 is a HW constraint for no-stretch mode
	 */
	scale->factor_x = (scale->src_w - (1 << 16)) / (scale->dst_w - 1);
	scale->factor_y = (scale->src_h - (1 << 16)) / (scale->dst_h - 1);

	/* Strech mode scale factors do not require -1. See b/294939884 */
	if (scale->factor_x != scale->factor_y) {
		scale->stretch_mode = true;
		scale->factor_x = scale->src_w / scale->dst_w;
		scale->factor_y = scale->src_h / scale->dst_h;
	}

	/*
	 * See b/294939884 for details on offset calculation.
	 * Note that factors are computed as src/dest, so scale factors < 1 are _upscaling_
	 */
	if (scale->stretch_mode) {
		scale->offset_x = (scale->factor_x < DRM_PLANE_NO_SCALING) ?
					  (scale->factor_x >> 1) + (8 << 7) :
					  0x0;
		scale->offset_y = (scale->factor_y < DRM_PLANE_NO_SCALING) ?
					  (scale->factor_y >> 1) + (8 << 7) :
					  0x0;
	} else {
		scale->offset_x = (scale->factor_x < DRM_PLANE_NO_SCALING) ? 0x8000 : 0x0;
		scale->offset_y = (scale->factor_y < DRM_PLANE_NO_SCALING) ? 0x8000 : 0x0;
	}

	scale->enable = (scale->factor_x != DRM_PLANE_NO_SCALING ||
			 scale->factor_y != DRM_PLANE_NO_SCALING);
}

static void update_scale(struct g2d_sc *sc, u8 id, struct drm_plane_state *plane_state)
{
	struct sc_hw_scale scale = { 0 };

	populate_layer_scale(plane_state, &scale);

	sc_hw_update_plane_scale(&sc->hw, id, &scale);
}

static void g2d_sc_commit(struct device *dev, struct drm_crtc *crtc)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;
	// TODO(b/355089225): display ID is currently hardcoded to 0.
	// Update this when we support 2 pipelines.
	u8 display_id = 0;

	sc_hw_commit(&sc->hw, display_id);

	/*
	 * TODO(b/355089225): display ID is currently hardcoded to 0.
	 * Update this when we support 2 pipelines.
	 */
	sc_hw_start_trigger(&sc->hw, 0 /*display_id*/);
}

static void g2d_sc_crtc_enable(struct device *dev, struct drm_crtc *crtc)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;

	pm_runtime_get_sync(dev);

	dev_dbg(dev, "%s: Enabling interrupts on crtc enable.", __func__);
	sc_hw_enable_interrupts(&sc->hw);
}

static void g2d_sc_crtc_disable(struct device *dev, struct drm_crtc *crtc)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;

	dev_dbg(dev, "%s: Disablng interrupts on crtc disable.", __func__);

	sc_hw_disable_interrupts(&sc->hw);

	pm_runtime_put_sync(dev);
}

void g2d_wb_hw_configure(struct g2d_writeback_connector *g2d_wb_connector,
			 struct drm_framebuffer *fb)
{
	struct drm_writeback_connector *wb_connector;
	struct drm_connector *connector;
	struct drm_device *drm;
	struct g2d_device *g2d_device;
	struct g2d_sc *sc;

	if (!fb || !g2d_wb_connector) {
		pr_err("%s: Invalid parameters!", __func__);
		return;
	}

	wb_connector = &g2d_wb_connector->base;
	connector = &wb_connector->base;
	drm = connector->dev;
	g2d_device = container_of(drm, struct g2d_device, drm);
	sc = g2d_device->sc;

	update_wb_fb(sc, 0 /*display_id is hard coded for now*/, g2d_wb_connector, fb);
	wb_set_fb(&sc->hw, 0, &sc->hw.wb[0].fb); /*hw_id set to 0 for now*/
}

static void update_plane(struct g2d_sc *sc, struct g2d_plane *g2d_plane,
			 struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = g2d_plane->base.state;

	if (!plane_state) {
		dev_err(state->dev->dev, "%s: Invalid drm_plane_state!", __func__);
		return;
	}

	struct sc_hw_fb fb = { 0 };

	update_plane_fb(sc, g2d_plane, g2d_plane->id, &fb);

	update_roi(sc, g2d_plane->id, plane_state);

	update_scale(sc, g2d_plane->id, plane_state);

	update_plane_y2r(sc, g2d_plane->id, plane_state, state);
}

static int check_plane_rotation(struct drm_plane_state *plane_state)
{
	const struct device *dev = plane_state->plane->dev->dev;
	u32 rotate_mask = plane_state->rotation & DRM_MODE_ROTATE_MASK;
	u32 flip_mask = plane_state->rotation & DRM_MODE_REFLECT_MASK;

	/* Return early if neither flip nor rotate are configured */
	if (plane_state->rotation == DRM_MODE_ROTATE_0)
		return 0;

	/* Reduce complex Flip + Rotate */
	if (flip_mask == (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
		switch (rotate_mask) {
		case DRM_MODE_ROTATE_0:
			rotate_mask = DRM_MODE_ROTATE_180;
			break;
		case DRM_MODE_ROTATE_90:
			rotate_mask = DRM_MODE_ROTATE_270;
			break;
		case DRM_MODE_ROTATE_180:
			rotate_mask = DRM_MODE_ROTATE_0;
			break;
		case DRM_MODE_ROTATE_270:
			rotate_mask = DRM_MODE_ROTATE_90;
			break;
		default:
			return -EINVAL;
		}
		flip_mask = 0;
	} else if (rotate_mask == DRM_MODE_ROTATE_180) {
		if (flip_mask) {
			flip_mask ^= DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
			rotate_mask = DRM_MODE_ROTATE_0;
		}
	}

	/* b/318783657 - Rotation + x or y flip not supported */
	if (rotate_mask & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) {
		if (flip_mask != 0) {
			dev_err(dev, "Flip+Rotate not supported!\n");
			return -EINVAL;
		}
	}

	/* Atomic check allows the freestanding state to be modified. */
	plane_state->rotation = rotate_mask | flip_mask;

	return 0;
}

static int check_plane_y2r(struct g2d_plane_state *g2d_plane_state)
{
	struct drm_plane_state *plane_state = &g2d_plane_state->base;
	struct device *dev = plane_state->plane->dev->dev;
	bool is_yuv = plane_state->fb->format->is_yuv;

	/* TODO(b/384820419) Note that these will always pass until custom coefficients are
	 * supported
	 */
	if (g2d_plane_state->y2r_coef && !is_yuv) {
		dev_err(dev, "%s: Y2R coefficients provided for an RGB frame!", __func__);
		return -EINVAL;
	}

	if (g2d_plane_state->r2y_coef && is_yuv) {
		dev_err(dev, "%s: R2Y coefficients provided for an YUV frame!", __func__);
		return -EINVAL;
	}
	return 0;
}

static int check_plane(struct drm_device *dev, struct g2d_plane *plane,
		       struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = plane_state->crtc;
	struct drm_crtc_state *crtc_state;
	struct g2d_plane_state *g2d_plane_state = to_g2d_plane_state(plane_state);
	int ret = 0;

	crtc_state = drm_atomic_get_new_crtc_state(plane_state->state, crtc);
	if (IS_ERR(crtc_state)) {
		dev_err(dev->dev, "%s invalid crtc_state", __func__);
		return -EINVAL;
	}

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state, G2D_MIN_SCALE,
						  G2D_MAX_SCALE, true /* can_position */,
						  true /* can_update_disabled */);

	if (ret) {
		dev_err(dev->dev, "%s: check plane helper failed", __func__);
		return ret;
	}

	ret = check_plane_rotation(plane_state);
	if (ret)
		return ret;

	ret = check_plane_y2r(g2d_plane_state);

	return ret;
}

static const struct g2d_writeback_funcs sc_writeback_funcs = {
	.config = g2d_wb_hw_configure,
};

static const struct g2d_crtc_funcs sc_crtc_funcs = {
	.commit = g2d_sc_commit,
	.enable = g2d_sc_crtc_enable,
	.disable = g2d_sc_crtc_disable,
};

static const struct g2d_plane_funcs sc_plane_funcs = {
	.update = update_plane,
	.check = check_plane,
};

void sc_crtc_init(struct g2d_crtc *g2d_crtc)
{
	g2d_crtc->funcs = &sc_crtc_funcs;
}

void sc_wb_init(struct g2d_writeback_connector *g2d_wb_connector)
{
	g2d_wb_connector->funcs = &sc_writeback_funcs;
}

void sc_plane_init(struct g2d_plane *g2d_plane)
{
	g2d_plane->funcs = &sc_plane_funcs;
}

void sc_init(struct g2d_sc *sc, struct device *dev)
{
	sc_hw_init(&sc->hw, dev);
}

int sc_ioremap_memory(struct platform_device *pdev, struct g2d_sc *sc)
{
	struct resource *resource = NULL;

	sc->hw.reg_base = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(sc->hw.reg_base))
		return PTR_ERR(sc->hw.reg_base);

	sc->hw.reg_size = resource != NULL ? resource_size(resource) : 0;
	sc->hw.reg_dump_offset = 0;
	sc->hw.reg_dump_size = sc->hw.reg_size;
	dev_dbg(&pdev->dev, "%s, reg_base is : %pK, reg_size : 0x%x", __func__, sc->hw.reg_base,
		sc->hw.reg_size);

	return 0;
}

static irqreturn_t sc_isr(int irq, void *data)
{
	struct g2d_sc *sc = data;
	struct device *dev = sc->hw.dev;
	struct sc_hw_interrupt_status status = { 0 };
	int i, pipe_mask;

	sc_hw_get_interrupt(&sc->hw, &status);

	for (i = 0; i < NUM_PIPELINES; i++) {
		pipe_mask = BIT(i);
		if (status.pipe_frame_done & pipe_mask)
			g2d_handle_writeback_frm_done(sc->writeback[i]);
	}

	dev_dbg(dev,
		"%s: frm_start=%#x frm_done=%x pvric_decode_err=%x pvric_encode_err=%x apb_hang=%#x axi_rd_bus_hang=%d axi_wr_bus_hang=%d axi_bus_err=%d",
		__func__, status.pipe_frame_start, status.pipe_frame_done, status.pvric_decode_err,
		status.pvric_encode_err, status.apb_hang, status.axi_rd_bus_hang,
		status.axi_wr_bus_hang, status.axi_bus_err);

	return IRQ_HANDLED;
}

int sc_irq_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int i, irq, ret;
	struct drm_device *drm;
	struct g2d_device *g2d_device;
	struct g2d_sc *sc;

	dev = &pdev->dev;
	drm = dev_get_drvdata(dev);
	g2d_device = container_of(drm, struct g2d_device, drm);
	sc = g2d_device->sc;

	sc->irq_num = platform_irq_count(pdev);
	if (sc->irq_num <= 0) {
		ret = -EINVAL;
		return ret;
	}

	sc->irqs = devm_kmalloc_array(dev, sc->irq_num, sizeof(*sc->irqs), GFP_KERNEL);
	if (!sc->irqs) {
		dev_err(dev, "ERROR: IRQ struct allocation failed.!");
		return -ENOMEM;
	}

	for (i = 0; i < sc->irq_num; i++) {
		irq = platform_get_irq(pdev, i);
		ret = devm_request_irq(dev, irq, sc_isr, IRQF_NO_AUTOEN, dev_name(dev), sc);
		if (ret < 0) {
			dev_err(dev, "Failed to install irq:%u.\n", irq);
			return ret;
		}
		sc->irqs[i] = irq;
	}

	return ret;
}

static void g2d_sc_enable_irqs(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;
	int i;

	for (i = 0; i < sc->irq_num; i++) {
		dev_dbg(dev, "Enable IRQ %d", i);
		enable_irq(sc->irqs[i]);
	}
}

static void g2d_sc_disable_irqs(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;
	int i;

	for (i = 0; i < sc->irq_num; i++) {
		dev_dbg(dev, "Disable IRQ %d", i);
		disable_irq(sc->irqs[i]);
	}
}

int g2d_pm_runtime_suspend(struct device *dev)
{
	g2d_sc_disable_irqs(dev);

	return 0;
}

int g2d_pm_runtime_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;

	g2d_sc_enable_irqs(dev);
	sc_hw_restore_state(&sc->hw);

	return 0;
}

void g2d_sc_print_id_regs(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);
	struct g2d_sc *sc = g2d_device->sc;

	sc_hw_print_id_regs(&sc->hw, dev);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int reg_dump_show(struct seq_file *s, void *data)
{
	struct g2d_sc *sc = s->private;
	struct sc_hw *hw = &sc->hw;
	int ret;

	ret = pm_runtime_get_if_in_use(hw->dev);
	if (ret <= 0) {
		if (ret < 0)
			dev_warn(hw->dev, "%s: Skipping dump. PM runtime get err: %d", __func__,
				 ret);
		else
			dev_dbg(hw->dev, "%s: G2D off, skipping dump.", __func__);

		return ret;
	}

	ret = sc_hw_reg_dump(s, hw);
	pm_runtime_put_sync(hw->dev);

	return ret;
}

DEFINE_SHOW_ATTRIBUTE(reg_dump);
int sc_debugfs_init(struct device *dev)
{
	struct drm_device *drm;
	struct g2d_device *g2d_device;
	struct g2d_sc *sc;

	drm = dev_get_drvdata(dev);
	g2d_device = container_of(drm, struct g2d_device, drm);
	sc = g2d_device->sc;

	sc->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(sc->debugfs)) {
		dev_err(dev, "could not create debugfs root folder\n");
		return PTR_ERR(sc->debugfs);
	}

	debugfs_create_file("reg_dump", 0444, sc->debugfs, sc, &reg_dump_fops);

	return 0;
}

void sc_debugfs_deinit(struct device *dev)
{
	struct drm_device *drm;
	struct g2d_device *g2d_device;
	struct g2d_sc *sc;

	drm = dev_get_drvdata(dev);
	g2d_device = container_of(drm, struct g2d_device, drm);
	sc = g2d_device->sc;

	debugfs_remove_recursive(sc->debugfs);
}
#endif /* CONFIG_DEBUG_FS */
