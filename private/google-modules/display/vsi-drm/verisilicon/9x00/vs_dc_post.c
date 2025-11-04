// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/media-bus-format.h>
#include <linux/of_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <uapi/linux/sched/types.h>

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
#include <linux/regmap.h>
#endif

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_vblank.h>

#include <gs_drm/gs_drm_connector.h>
#include <trace/dpu_trace.h>

#include "vs_crtc.h"
#include "vs_dc.h"
#include "vs_dc_post.h"
#include "vs_dc_hw.h"
#include "vs_drv.h"
#include "vs_writeback.h"
#include "vs_dc_info.h"
#include "vs_gem.h"
#include "display_compress/vs_dc_dsc.h"
#include "postprocess/vs_dc_histogram.h"
#include "vs_trace.h"

#include <drm/vs_drm.h>
#include <drm/vs_drm_fourcc.h>

#define MAX_DC_WAIT_EARLIEST_PROCESS_TIME_USEC 100000
#define MAX_FRAMES_PENDING_COUNT 2

static inline void update_wb_format(u32 format, struct dc_hw_fb *fb)
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

static inline void update_bld_wb_format(u32 format, struct dc_hw_fb *fb)
{
	u8 f = BLD_WB_FORMAT_ARGB8888;

	switch (format) {
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRA1010102:
		f = BLD_WB_FORMAT_A2RGB101010;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		f = BLD_WB_FORMAT_NV12;
		break;
	case DRM_FORMAT_P010:
		f = BLD_WB_FORMAT_P010;
		break;
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		f = BLD_WB_FORMAT_YV12;
		break;
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
		f = BLD_WB_FORMAT_NV16;
		break;
	case DRM_FORMAT_P210:
		f = BLD_WB_FORMAT_P210;
		break;
	case DRM_FORMAT_YUV420_10BIT:
		f = BLD_WB_FORMAT_YUV420_PACKED_10BIT;
		break;
	default:
		break;
	}

	fb->format = f;
}

static inline void update_swizzle(u32 format, struct dc_hw_fb *fb)
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
	case DRM_FORMAT_YUV420:
		fb->uv_swizzle = 1;
		break;
	default:
		break;
	}
}

static inline void update_tile_mode(const struct drm_framebuffer *fb, struct dc_hw_fb *dc_fb)
{
	u8 norm_mode, tile = TILE_MODE_LINEAR;

	norm_mode = get_fb_modifier_norm_mode(fb);

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
	case DRM_FORMAT_MOD_VS_TILE_8X8_UNIT2X2:
		tile = TILE_MODE_8X8_UNIT2X2;
		break;
	case DRM_FORMAT_MOD_VS_TILE_8X4_UNIT2X2:
		tile = TILE_MODE_8X4_UNIT2X2;
		break;
	default:
		break;
	}

	dc_fb->tile_mode = tile;
}

static inline void update_wb_tile_mode(const struct drm_framebuffer *fb, struct dc_hw_fb *dc_fb)
{
	u8 norm_mode, tile = WB_TILE_MODE_LINEAR;

	norm_mode = get_fb_modifier_norm_mode(fb);
	if (norm_mode == DRM_FORMAT_MOD_VS_TILE_16X16)
		tile = WB_TILE_MODE_16X16;

	dc_fb->tile_mode = tile;
}

inline u8 to_vs_display_id(struct vs_dc *dc, struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	const struct vs_display_info *display_info;
	const struct vs_dc_info *dc_info = dc->hw.info;
	u8 id = 0;

	display_info = &dc_info->displays[vs_crtc->id];
	id = display_info->id;

	return id;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static struct drm_vs_color last_crtc_crc[DC_DISPLAY_NUM];

void vs_crtc_set_last_crc(u32 crtc_id, struct drm_vs_color value)
{
	last_crtc_crc[crtc_id].a = value.a;
	last_crtc_crc[crtc_id].r = value.r;
	last_crtc_crc[crtc_id].g = value.g;
	last_crtc_crc[crtc_id].b = value.b;
}

static int vs_dc_put_display_crc_result(struct seq_file *s)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);

	seq_printf(s, "crtc[%u]: %s\n", crtc->base.id, crtc->name);

	seq_printf(s, "\tenable: %d\n", crtc_state->crc.enable);
	seq_printf(s, "\tpos setting instructions:\n\t\tpos\t\tid\n\t\tBLD\t\t0\
	\n\t\tPOST_PROC\t1\n\t\tOFIFO_IN\t2\n\t\tOFIFO_OUT\t3\n\t\tWB\t\t4\n");
	seq_printf(s, "\tpos-id = %d\n", crtc_state->crc.pos);
	if (crtc_state->crc.pos == VS_DISP_CRC_OFIFO_OUT) {
		seq_printf(s, "\talpha-seed= [0x%x, 0x%x]\n", crtc_state->crc.seed[0].a,
			   crtc_state->crc.seed[1].a);
		seq_printf(s, "\tred-seed= [0x%x, 0x%x]\n", crtc_state->crc.seed[0].r,
			   crtc_state->crc.seed[1].r);
		seq_printf(s, "\tgreen-seed= [0x%x, 0x%x]\n", crtc_state->crc.seed[0].g,
			   crtc_state->crc.seed[1].g);
		seq_printf(s, "\tblue-seed= [0x%x, 0x%x]\n", crtc_state->crc.seed[0].b,
			   crtc_state->crc.seed[1].b);

		seq_printf(s, "\talpha-crc= [0x%x, 0x%x]\n", crtc_state->crc.result[0].a,
			   crtc_state->crc.result[1].a);
		seq_printf(s, "\tred-crc= [0x%x, 0x%x]\n", crtc_state->crc.result[0].r,
			   crtc_state->crc.result[1].r);
		seq_printf(s, "\tgreen-crc= [0x%x, 0x%x]\n", crtc_state->crc.result[0].g,
			   crtc_state->crc.result[1].g);
		seq_printf(s, "\tblue-crc= [0x%x, 0x%x]\n", crtc_state->crc.result[0].b,
			   crtc_state->crc.result[1].b);
	} else {
		seq_printf(s, "\talpha-seed= [0x%x]\n", crtc_state->crc.seed[0].a);
		seq_printf(s, "\tred-seed= [0x%x]\n", crtc_state->crc.seed[0].r);
		seq_printf(s, "\tgreen-seed= [0x%x]\n", crtc_state->crc.seed[0].g);
		seq_printf(s, "\tblue-seed= [0x%x]\n", crtc_state->crc.seed[0].b);

		seq_printf(s, "\talpha-crc= [0x%x]\n", crtc_state->crc.result[0].a);
		seq_printf(s, "\tred-crc= [0x%x]\n", crtc_state->crc.result[0].r);
		seq_printf(s, "\tgreen-crc= [0x%x]\n", crtc_state->crc.result[0].g);
		seq_printf(s, "\tblue-crc= [0x%x]\n", crtc_state->crc.result[0].b);
	}

	seq_printf(s, "\tlast-alpha-crc= [0x%x]\n", last_crtc_crc[drm_crtc_index(crtc)].a);
	seq_printf(s, "\tlast-red-crc= [0x%x]\n", last_crtc_crc[drm_crtc_index(crtc)].r);
	seq_printf(s, "\tlast-green-crc= [0x%x]\n", last_crtc_crc[drm_crtc_index(crtc)].g);
	seq_printf(s, "\tlast-blue-crc= [0x%x]\n", last_crtc_crc[drm_crtc_index(crtc)].b);

	return 0;
}

static ssize_t vs_dc_set_display_crc_state(struct drm_crtc *crtc, const char __user *ubuf,
					   size_t len)
{
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	unsigned long value;
	char buf[256], *cur = buf;

	buf[len] = '\0';

	if (!vs_crtc->funcs->set_crc)
		return -EINVAL;

	if (len > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;

	cur = strstr(buf, "enable:");
	if (cur) {
		cur += 7;
		value = simple_strtoul(cur, NULL, 10);
		crtc_state->crc.enable = value;
	} else {
		return -EINVAL;
	}

	cur = strstr(buf, "pos:");
	if (cur) {
		cur += 4;
		value = simple_strtoul(cur, NULL, 10);
		crtc_state->crc.pos = value;
	}

	if (crtc_state->crc.pos == VS_DISP_CRC_OFIFO_OUT) {
		cur = strstr(buf, "a-seed0:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].a = value;
		}
		cur = strstr(buf, "a-seed1:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[1].a = value;
		}

		cur = strstr(buf, "r-seed0:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].r = value;
		}
		cur = strstr(buf, "r-seed1:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[1].r = value;
		}

		cur = strstr(buf, "g-seed0:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].g = value;
		}
		cur = strstr(buf, "g-seed1:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[1].g = value;
		}

		cur = strstr(buf, "b-seed0:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].b = value;
		}
		cur = strstr(buf, "b-seed1:");
		if (cur) {
			cur += 8;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[1].b = value;
		}
	} else {
		cur = strstr(buf, "a-seed:");
		if (cur) {
			cur += 7;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].a = value;
		}

		cur = strstr(buf, "r-seed:");
		if (cur) {
			cur += 7;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].r = value;
		}

		cur = strstr(buf, "g-seed:");
		if (cur) {
			cur += 7;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].g = value;
		}

		cur = strstr(buf, "b-seed:");
		if (cur) {
			cur += 7;
			value = simple_strtoul(cur, NULL, 16);
			crtc_state->crc.seed[0].b = value;
		}
	}

	return len;
}

static void vs_dc_set_display_crc(struct device *dev, struct drm_crtc *crtc,
				  const char __user *ubuf, size_t len)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct drm_crtc_state *state = vs_crtc->base.state;
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(state);
	struct vs_display_info *display_info;
	u8 hw_id;

	struct dc_hw_disp_crc crc;

	if (vs_dc_set_display_crc_state(crtc, ubuf, len) != len) {
		pr_err("%s: parse crc parameter error.\n", __func__);
		return;
	}

	display_info = (struct vs_display_info *)&dc->hw.info->displays[vs_crtc->id];
	if (!display_info) {
		pr_err("%s: Invalid vs_crtc index.\n", __func__);
		return;
	}

	if (!display_info->crc) {
		pr_info("%s: vs_crtc[%u] does not support crc.\n", __func__, vs_crtc->id);
		return;
	}

	if (crtc_state->crc.pos > VS_DISP_CRC_OFIFO_OUT) {
		pr_err("%s: Invalid crc pos.\n", __func__);
		return;
	}

	hw_id = display_info->id;

	crc.enable = crtc_state->crc.enable;
	crc.pos = crtc_state->crc.pos;
	if (!crc.enable) {
		dc_hw_set_display_crc(&dc->hw, hw_id, &crc);
		return;
	}

	memcpy(&crc.seed, &crtc_state->crc.seed, sizeof(crtc_state->crc.seed));
	dc_hw_set_display_crc(&dc->hw, hw_id, &crc);
}

static int vs_dc_show_display_pattern_config(struct seq_file *s)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);

	seq_printf(s, "crtc[%u]: %s\n", crtc->base.id, crtc->name);

	seq_printf(s, "\tenable: %d\n", crtc_state->pattern.enable);
	seq_printf(s, "\tpos setting instructions:\n\t\tpos\t\tid\n\t\tBLD\t\t0\
			\n\t\tPOST_PROC\t1\n\t\tOFIFO\t\t2\n");
	seq_printf(s, "\tpos-id = %d\n", crtc_state->pattern.pos);
	seq_printf(s, "\tmode setting instructions:\n\t\tmode\t\tid\n\t\tPURE_CLR\
\t0\n\t\tCLR_BAR_H\t1\n\t\tCLR_BAR_V\t2\n\t\tRMAP_H\t\t3\n\t\tRMAP_V\t\t4\n\t\
\tBLK_WHT_H\t5\n\t\tBLK_WHT_V\t6\n\t\tBLK_WHT_S\t7\n\t\tBORDER\t\t8\n\t\t\
CURSOR\t\t9\n");
	seq_printf(s, "\tmode-id = %d\n", crtc_state->pattern.mode);
	seq_printf(s, "\tcurser = {%d,%d}\n", crtc_state->pattern.rect.x,
		   crtc_state->pattern.rect.y);
	seq_printf(s, "\twidth = %d\n", crtc_state->pattern.rect.w);
	seq_printf(s, "\theight = %d\n", crtc_state->pattern.rect.h);
	seq_printf(s, "\tcolor = 0x%llx\n", crtc_state->pattern.color);

	return 0;
}

static ssize_t vs_dc_parse_display_pattern_config(struct vs_crtc_state *crtc_state,
						  const char __user *ubuf, size_t len)
{
	unsigned long value;
	unsigned long long color_val;

	char buf[96], *cur = buf;

	if (len > sizeof(buf) - 1)
		return -EINVAL;

	if (copy_from_user(buf, ubuf, len))
		return -EINVAL;

	buf[len] = '\0';

	cur = strstr(buf, "enable:");
	if (cur) {
		cur += 7;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.enable = value;
	} else {
		return -EINVAL;
	}

	cur = strstr(buf, "pos:");
	if (cur) {
		cur += 4;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.pos = value;
	}

	cur = strstr(buf, "mode:");
	if (cur) {
		cur += 5;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.mode = value;
	}

	cur = strstr(buf, "size.x:");
	if (cur) {
		cur += 7;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.rect.x = value;
	}

	cur = strstr(buf, "size.y:");
	if (cur) {
		cur += 7;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.rect.y = value;
	}

	cur = strstr(buf, "size.w:");
	if (cur) {
		cur += 7;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.rect.w = value;
	}

	cur = strstr(buf, "size.h:");
	if (cur) {
		cur += 7;

		value = simple_strtoul(cur, NULL, 10);

		crtc_state->pattern.rect.h = value;
	}

	cur = strstr(buf, "color:0x");
	if (cur) {
		cur += 8;

		if (kstrtoull(cur, 16, &color_val))
			return -EINVAL;

		crtc_state->pattern.color = color_val;
	}

	return len;
}

static void vs_dc_set_display_pattern(struct drm_crtc *crtc, const char __user *ubuf, size_t len)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_dc *dc = dev_get_drvdata(vs_crtc->dev);
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);
	struct vs_display_info *display_info;
	struct dc_hw_pattern pattern = crtc_state->pattern;
	u16 width = 0, height = 0;
	u8 hw_id;

	/* parse display pattern config */
	if (vs_dc_parse_display_pattern_config(crtc_state, ubuf, len) != len) {
		pr_err("%s: parse display pattern config error.\n", __func__);
		return;
	}

	display_info = (struct vs_display_info *)&dc->hw.info->displays[vs_crtc->id];
	if (!display_info) {
		pr_err("%s: Invalid vs_crtc index.\n", __func__);
		return;
	}

	if (!display_info->test_pattern) {
		pr_info("%s: vs_crtc[%u] does not support test pattren.\n", __func__, vs_crtc->id);
		return;
	}

	hw_id = display_info->id;

	if (pattern.pos > VS_DISP_TP_OFIFO) {
		pr_err("%s: Invalid test pattern pos.\n", __func__);
		return;
	}

	if (!pattern.enable)
		return dc_hw_set_display_pattern(&dc->hw, hw_id, &pattern);

	if (pattern.mode > VS_CURSOR_PATRN) {
		pr_err("%s: Invalid test pattern mode.\n", __func__);
		return;
	}

	width = dc->hw.display[vs_crtc->id].mode.h_active;
	height = dc->hw.display[vs_crtc->id].mode.v_active;

	if (pattern.mode == VS_CURSOR_PATRN &&
	    (pattern.rect.x > width || pattern.rect.y > height)) {
		pr_err("%s: Coordinate of cursor out of range.\n", __func__);
		return;
	} else if ((pattern.mode == VS_COLOR_BAR_H || pattern.mode == VS_RMAP_H) &&
		   pattern.rect.h > height) {
		pr_err("%s: Horizontal color pattern h out of range.\n", __func__);
		return;
	} else if ((pattern.mode == VS_COLOR_BAR_V || pattern.mode == VS_RMAP_V) &&
		   pattern.rect.w > width) {
		pr_err("%s: Vertical color pattern w out of range.\n", __func__);
		return;
	} else if ((pattern.mode == VS_BLACK_WHITE_H || pattern.mode == VS_BLACK_WHITE_SQR) &&
		   (pattern.rect.h < 1 || pattern.rect.h > 256)) {
		pr_err("%s: Horizontal black white pattern out of range.\n", __func__);
		return;
	} else if (pattern.mode == VS_BLACK_WHITE_V &&
		   (pattern.rect.w < 1 || pattern.rect.w > 256)) {
		pr_err("%s: Vertical black white pattern out of range.\n", __func__);
		return;
	}

	if (pattern.mode == VS_RMAP_H)
		pattern.ramp_step = 65535 / width * 2048;
	else if (pattern.mode == VS_RMAP_V)
		pattern.ramp_step = 65535 / height * 2048;

	dc_hw_set_display_pattern(&dc->hw, hw_id, &pattern);
}
#endif /* CONFIG_DEBUG_FS */

static void vs_dc_display_set_mode(struct device *dev, struct drm_crtc *crtc,
				   struct drm_atomic_state *atomic_state)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct dc_hw_display_mode display = { 0 };
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	const struct gs_drm_connector_state *gs_conn_state;
	struct dc_hw_dsc_usage dsc_usage = { 0 };
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	const struct vs_display_info *display_info;
	const struct vs_dc_info *dc_info = dc->hw.info;

	display.bus_format = crtc_state->output_fmt;
	display.output_mode = crtc_state->output_mode;
	display.te_usec = crtc_state->te_usec;
	display.fps = drm_mode_vrefresh(mode);
	display.h_active = mode->hdisplay;
	display.h_total = mode->htotal;
	display.h_sync_start = mode->hsync_start;
	display.h_sync_end = mode->hsync_end;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		display.h_sync_polarity = false;
	else
		display.h_sync_polarity = true;  /* DRM_MODE_FLAG_PHSYNC is set or default */

	display.v_active = mode->vdisplay;
	display.v_total = mode->vtotal;
	display.v_sync_start = mode->vsync_start;
	display.v_sync_end = mode->vsync_end;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		display.v_sync_polarity = false;
	else
		display.v_sync_polarity = true;  /* DRM_MODE_FLAG_PVSYNC is set or default */

	if (crtc_state->encoder_type == DRM_MODE_ENCODER_VIRTUAL) {
		if (crtc_state->out_dp)
			display.out = OUT_DP;
		else
			display.out = OUT_DPI;
	} else if (crtc_state->encoder_type == DRM_MODE_ENCODER_DSI) {
		display.out = OUT_DPI;
	} else {
		display.out = OUT_DP;
	}

	display.vrr_enable = crtc_state->base.vrr_enabled;
	display.enable = crtc_state->base.active;

	display_info = &dc_info->displays[vs_crtc->id];
	if (display_info->dsc) {
		gs_conn_state = crtc_get_new_gs_connector_state(atomic_state, crtc->state);
		if (gs_conn_state) {
			const struct gs_display_mode *gs_mode = &gs_conn_state->gs_mode;

			if (gs_mode->dsc.enabled && gs_mode->dsc.cfg) {
				const struct drm_dsc_config *cfg = gs_mode->dsc.cfg;
				unsigned int dsc_count = gs_mode->dsc.dsc_count ?: 1;

				dsc_usage.enable = true;
				dsc_usage.slice_height = cfg->slice_height;
				if (cfg->slice_count)
					dsc_usage.slices_per_line = cfg->slice_count;
				else
					dsc_usage.slices_per_line =
						cfg->pic_width / cfg->slice_width;
				dsc_usage.ss_num = dsc_usage.slices_per_line / dsc_count;

				dsc_usage.split_panel_enable = true;
				dsc_usage.multiplex_mode_enable = true;
				dsc_usage.multiplex_out_sel = false;
				dsc_usage.de_raster_enable = true;
				dsc_usage.multiplex_eoc_enable = false;
				dsc_usage.video_mode =
					(gs_mode->mode_flags & MIPI_DSI_MODE_VIDEO) != 0;
			} else {
				dsc_usage.enable = false;
			}

			dc_hw_config_dsc(&dc->hw, vs_crtc->id, &dsc_usage, gs_mode->dsc.cfg);
		} else {
			dev_dbg(dev, "%s: no gs_drm_connector for dsc config\n", __func__);
			dc_hw_config_dsc(&dc->hw, vs_crtc->id, &dsc_usage, NULL);
		}
		display.dsc_enable = dsc_usage.enable;
	}

	dc_hw_setup_display_mode(&dc->hw, vs_crtc->id, &display);
}

static void dc_fabrt_boost_kwork(struct kthread_work *work)
{
	struct vs_crtc *vs_crtc = container_of(work, struct vs_crtc, fboost_work);
	int ret;

	DPU_ATRACE_BEGIN("DPU Boost_FABRT");
	vs_qos_set_fabrt_boost(&vs_crtc->base);

	ret = wait_event_interruptible_timeout(
		vs_crtc->fboost_wait_q, READ_ONCE(vs_crtc->fboost_state) == VS_FABRT_BOOST_RESTORE,
		msecs_to_jiffies(20));
	DPU_ATRACE_BEGIN("RESTORE FABRT ret = %d", ret);

	vs_qos_clear_fabrt_boost(&vs_crtc->base);
	WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_INIT);
	DPU_ATRACE_END("RESTORE FABRT");
	DPU_ATRACE_END("DPU Boost_FABRT");
}

static void vs_dc_enable(struct device *dev, struct drm_crtc *crtc,
			 struct drm_atomic_state *state)
{
	/* TBD !
	 * developer should update the function implementation
	 * according to actual requirements during developing !
	 */
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	const struct vs_display_info display_info = dc->hw.info->displays[vs_crtc->id];
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	const struct drm_vs_r2y_config *r2y_config = NULL;
	u8 display_id = to_vs_display_id(dc, crtc);
	bool need_boost_fabrt;

	DPU_ATRACE_BEGIN(__func__);
	/*
	 * get the output id information form encoder ID,
	 * if the ENCODER NONE, the output_id = hw_id
	 */
	if (vs_crtc_state->encoder_type == DRM_MODE_ENCODER_NONE)
		dc->hw.display[vs_crtc->id].output_id = dc->hw.display[vs_crtc->id].info->id;
	else
		dc->hw.display[vs_crtc->id].output_id = vs_crtc_state->output_id;

	/* For convernienting to debug,
	 * get the output bus format from r2y config,
	 * the default output bus format is MEDIA_BUS_FMT_RGB888_1X24
	 */
	if (display_info.color_formats &
	    (DRM_COLOR_FORMAT_YCBCR444 | DRM_COLOR_FORMAT_YCBCR422 | DRM_COLOR_FORMAT_YCBCR420))
		r2y_config = vs_dc_drm_crtc_property_get(vs_crtc_state, "R2Y", NULL);
	if (r2y_config)
		vs_crtc_state->output_fmt = r2y_config->output_bus_format;

	vs_crtc_state->seamless_mode_change = false;


	dc_hw_config_display_status(&dc->hw, vs_crtc->id, true);
	dc_hw_enable_frame_irqs(&dc->hw, vs_crtc->id, true);
	dc_hw_enable_shadow_register(&dc->hw, display_id, false);

	vs_dc_display_set_mode(dev, crtc, state);

	vs_qos_set_qos_config(dev, crtc);

	need_boost_fabrt = (READ_ONCE(vs_crtc->fboost_state) == VS_FABRT_BOOST_PENDING);
	if (need_boost_fabrt) {
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_BOOSTING);
		kthread_queue_work(vs_crtc->fboost_worker, &vs_crtc->fboost_work);
	}

	dc_hw_display_commit(&dc->hw, display_id);
	/* in case of video mode, turn on timing engine during enable */
	if (!(vs_crtc_state->output_mode & VS_OUTPUT_MODE_CMD)) {
		dc_hw_enable_shadow_register(&dc->hw, display_id, true);
		dc_hw_start_trigger(&dc->hw, display_id, crtc);
	}

	if (need_boost_fabrt) {
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_RESTORE);
		wake_up_interruptible(&vs_crtc->fboost_wait_q);
	}

	DPU_ATRACE_END(__func__);
}

static bool vs_dc_display_is_idle(struct device *dev, struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct dc_hw_display *display = &dc->hw.display[vs_crtc->id];
	u32 position;
	u32 hpos, vpos, vres;

	if (!vs_crtc->funcs->get_crtc_scanout_position)
		return false;

	vs_crtc->funcs->get_crtc_scanout_position(dev, crtc, &position);

	hpos = position & 0xffff;
	vpos = (position >> 16) & 0xffff;
	vres = display->mode.v_active;
	dev_dbg(dev, "%s: scanout position: %d,%d (vres:%d)\n", __func__,
			hpos, vpos, vres);

	return !position || vpos == vres;
}

static void vs_dc_disable(struct device *dev, struct drm_crtc *crtc)
{
	/* TBD !
	 * developer should update the function implementation
	 * according to actual requirements during developing !
	 */
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct dc_hw_display_mode display;
	struct dc_hw_display *hw_disp = &dc->hw.display[vs_crtc->id];
	u8 display_id = to_vs_display_id(dc, crtc);
	bool is_vid_mode = (hw_disp->mode.output_mode & VS_OUTPUT_MODE_CMD) == 0;
	int ret;

	dc_hw_disable_trigger(&dc->hw, vs_crtc->id);

	if (is_vid_mode) {
		int pending = atomic_xchg(&vs_crtc->frames_pending, 1);
		bool is_idle = vs_dc_display_is_idle(dev, crtc);

		dev_dbg(dev, "%s: idle=%d, frames pending=%d\n", __func__, is_idle, pending);

		if (is_idle)
			atomic_set(&vs_crtc->frames_pending, 0);
	}
	DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending), vs_crtc->trace_pid,
			       "frames_pending[%u]", crtc->index);
	if (is_vid_mode) {
		ret = wait_event_timeout(vs_crtc->framedone_waitq,
					 atomic_read(&vs_crtc->frames_pending) <= 0 &&
						 !vs_crtc->frame_transfer_pending,
					 msecs_to_jiffies(100));
	} else {
		ret = wait_event_timeout(vs_crtc->framedone_waitq,
					 atomic_read(&vs_crtc->frames_pending) <= 0,
					 msecs_to_jiffies(100));
	}
	if (!ret) {
		trace_disp_frame_done_timeout(display_id, vs_crtc);
		dev_err(dev, "%s: wait for frame done timed out, frames pending:%d, te count:%d",
			crtc->name, atomic_read(&vs_crtc->frames_pending),
			atomic_read(&vs_crtc->te_count));
		atomic_set(&vs_crtc->frames_pending, 0);
		DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending), vs_crtc->trace_pid,
				       "frames_pending[%u]", crtc->index);
		vs_crtc->frame_transfer_pending = false;
		atomic_inc(&vs_crtc->frame_done_timeout);
	}

	if (is_vid_mode && !vs_dc_display_is_idle(dev, crtc))
		dev_WARN(dev, "still running after wait (%d)\n", ret);

	display.enable = false;

	dc_hw_setup_display_mode(&dc->hw, vs_crtc->id, &display);
	dc_hw_enable_frame_irqs(&dc->hw, vs_crtc->id, false);

	dc_hw_config_display_status(&dc->hw, vs_crtc->id, true);
	dc_hw_disable_plane_features(&dc->hw, vs_crtc->id);
}

static void update_display_bld_size(struct vs_dc *dc, u8 id, struct vs_crtc_state *crtc_state,
				    struct dc_hw_size *bld_size, struct vs_display_info *info)
{
	struct device *dev = dc->hw.dev;
	u16 width = 0, height = 0;

	if (info->bld_size) {
		if ((info->min_scale == 1 << 16) && (info->max_scale == 1 << 16)) {
			dc_hw_update_display_bld_size(&dc->hw, id, false, NULL);
		} else {
			width = (crtc_state->bld_size >> 16) & 0xFFFF;
			height = crtc_state->bld_size & 0xFFFF;

			if (!width || !height) {
				dev_err(dev, "vs_crtc[%u] has invalid image size in %s\n", id,
					__func__);
				return;
			}

			bld_size->width = width;
			bld_size->height = height;
			dc_hw_update_display_bld_size(&dc->hw, id, true, bld_size);
		}
	}
}

static void update_display_gamma(struct vs_dc *dc, u8 id, struct vs_crtc_state *crtc_state)
{
	if (crtc_state->prior_gamma.changed) {
		if (crtc_state->prior_gamma.blob) {
			memcpy(&dc->hw.display[id].gamma.lut[0], crtc_state->prior_gamma.blob->data,
			       sizeof(dc->hw.display[id].gamma.lut[0]));
			dc->hw.display[id].gamma.enable[0] = true;
		} else {
			dc->hw.display[id].gamma.enable[0] = false;
		}
	}

	if (crtc_state->roi0_gamma.changed) {
		if (crtc_state->roi0_gamma.blob) {
			memcpy(&dc->hw.display[id].gamma.lut[1], crtc_state->roi0_gamma.blob->data,
			       sizeof(dc->hw.display[id].gamma.lut[1]));
			dc->hw.display[id].gamma.enable[1] = true;
		} else {
			dc->hw.display[id].gamma.enable[1] = false;
		}
	}

	if (crtc_state->roi1_gamma.changed) {
		if (crtc_state->roi1_gamma.blob) {
			memcpy(&dc->hw.display[id].gamma.lut[2], crtc_state->roi1_gamma.blob->data,
			       sizeof(dc->hw.display[id].gamma.lut[2]));
			dc->hw.display[id].gamma.enable[2] = true;
		} else {
			dc->hw.display[id].gamma.enable[2] = false;
		}
	}

	if (crtc_state->prior_gamma.changed || crtc_state->roi0_gamma.changed ||
	    crtc_state->roi1_gamma.changed)
		dc->hw.display[id].gamma.dirty = true;

	trace_update_hw_display_feature("GAMMA", id, "en:[%d %d %d] dirty:%d",
					dc->hw.display[id].gamma.enable[0],
					dc->hw.display[id].gamma.enable[1],
					dc->hw.display[id].gamma.enable[2],
					dc->hw.display[id].gamma.dirty);
}

static void update_display_blur_mask(struct vs_dc *dc, u8 id, struct vs_crtc_state *crtc_state)
{
	if (crtc_state->blur_mask) {
		struct dc_hw_fb fb = { 0 };
		struct drm_framebuffer *drm_fb = crtc_state->blur_mask;
		dma_addr_t dma_addr[MAX_NUM_PLANES] = { 0 };
		dma_addr[0] = vs_fb_get_dma_addr(drm_fb, 0);

		fb.address = (u64)dma_addr[0];
		fb.stride = drm_fb->pitches[0];
		fb.width = drm_fb->width;
		fb.height = drm_fb->height;

		fb.format = drm_fb->format->format;
		update_tile_mode(drm_fb, &fb);

		dc_hw_update_display_blur_mask(&dc->hw, id, &fb);
	}
}

static void update_display_brightness_mask(struct vs_dc *dc, u8 id,
					   struct vs_crtc_state *crtc_state)
{
	if (crtc_state->brightness_mask) {
		struct dc_hw_fb fb = { 0 };
		struct drm_framebuffer *drm_fb = crtc_state->brightness_mask;
		dma_addr_t dma_addr[MAX_NUM_PLANES] = { 0 };
		dma_addr[0] = vs_fb_get_dma_addr(drm_fb, 0);

		fb.address = (u64)dma_addr[0];
		fb.stride = drm_fb->pitches[0];
		fb.width = drm_fb->width;
		fb.height = drm_fb->height;

		fb.format = drm_fb->format->format;
		update_tile_mode(drm_fb, &fb);

		dc_hw_update_display_brightness_mask(&dc->hw, id, &fb);
	}
}

static void update_display_hist_chans(struct vs_dc *dc, u8 id, struct vs_crtc_state *crtc_state)
{
	struct dc_hw *hw = &dc->hw;

	vs_dc_hist_chans_update(hw, id, crtc_state);
}

static void vs_dc_conf_display(struct device *dev, struct drm_crtc *crtc)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(crtc->state);
	struct vs_display_info *display_info;
	struct dc_hw_display *display = &dc->hw.display[vs_crtc->id];
	struct dc_hw_size bld_size = { 0 };
	int i;
	bool ret;

	display_info = (struct vs_display_info *)&dc->hw.info->displays[vs_crtc->id];
	if (!display_info) {
		dev_err(dev, "%s: Invalid vs_crtc index.\n", __func__);
		return;
	}

	update_display_bld_size(dc, vs_crtc->id, crtc_state, &bld_size, display_info);

	update_display_gamma(dc, vs_crtc->id, crtc_state);

	update_display_blur_mask(dc, vs_crtc->id, crtc_state);

	update_display_brightness_mask(dc, vs_crtc->id, crtc_state);

	update_display_hist_chans(dc, vs_crtc->id, crtc_state);

	/* dc property */
	for (i = 0; i < vs_crtc->properties.num; i++) {
		ret = vs_dc_update_drm_property(dc, vs_crtc->id, &crtc_state->drm_states[i],
						display->states.items[i].proto,
						&display->states.items[i], crtc_state);

		if (ret && !display->states.items[i].proto->update)
			trace_update_hw_display_feature_en_dirty(
				display->states.items[i].proto->name, vs_crtc->id,
				display->states.items[i].enable, display->states.items[i].dirty);
	}

	dc_hw_config_display_status(&dc->hw, vs_crtc->id, true);
}

static int check_display_blur_mask(struct vs_dc *dc, struct vs_crtc_state *crtc_state,
				   const struct vs_display_info *display_info)
{
	struct device *dev = dc->hw.dev;
	const struct drm_framebuffer *fb_mask = NULL;

	if (crtc_state->blur_mask) {
		if (!display_info->blur) {
			dev_err(dev, "%s The crtc is not support blur.\n", __func__);
			return -EINVAL;
		}

		fb_mask = crtc_state->blur_mask;

		if (fb_mask->format->format != DRM_FORMAT_C8) {
			dev_err(dev, "%s Invalid blur mask format.\n", __func__);
			return -EINVAL;
		}
	}

	return 0;
}

static int check_display_brightness_mask(struct vs_dc *dc, struct vs_crtc_state *crtc_state,
					 const struct vs_display_info *display_info)
{
	struct device *dev = dc->hw.dev;
	const struct drm_framebuffer *fb_mask = NULL;

	if (crtc_state->brightness_mask) {
		if (!display_info->brightness) {
			dev_err(dev, "%s The crtc is not support brightness.\n", __func__);
			return -EINVAL;
		}

		fb_mask = crtc_state->brightness_mask;

		if (fb_mask->format->format != DRM_FORMAT_C8) {
			dev_err(dev, "%s Invalid brightness mask format.\n", __func__);
			return -EINVAL;
		}
	}

	return 0;
}

static int check_display_gamma_lut(struct vs_dc *dc, struct drm_vs_gamma_lut *lut,
				   const struct vs_dc_info *info)
{
	struct device *dev = dc->hw.dev;
	u32 j;
	u32 gamma_in_bit, gamma_out_bit, gamma_seg_max, gamma_entry_max;

	gamma_in_bit = info->degamma_bits;
	gamma_out_bit = info->gamma_bits;
	gamma_seg_max = info->max_seg_num;
	gamma_entry_max = info->max_gamma_size;

	/* check the effectiveness of segment count */
	if (lut->seg_cnt > gamma_seg_max) {
		dev_err(dev, "%s: Invalid GAMMA segment count.\n", __func__);
		return -EINVAL;
	}

	/* check the effectiveness of x-step */
	for (j = 0; j < lut->seg_cnt; j++) {
		if (lut->seg_step[j] >> gamma_in_bit) {
			dev_err(dev, "%s: The x-step of GAMMA over valid bit.\n", __func__);
			return -EINVAL;
		}

		if (lut->seg_step[j] & (lut->seg_step[j] - 1)) {
			dev_err(dev, "%s: The x-step of GAMMA not be power of 2.\n", __func__);
			return -EINVAL;
		}
	}

	/* check the effectiveness of entry count*/
	if (lut->entry_cnt > gamma_entry_max) {
		dev_err(dev, "%s: Invalid GAMMA entry count.\n", __func__);
		return -EINVAL;
	}

	/* check the effectiveness of table value for R/G/B channels */
	for (j = 0; j < lut->entry_cnt; j++) {
		if ((lut->data[j].r >> gamma_out_bit) || (lut->data[j].g >> gamma_out_bit) ||
		    (lut->data[j].b >> gamma_out_bit)) {
			dev_err(dev, "%s: Invalid GAMMA bits range.\n", __func__);
			return -EINVAL;
		}

		if (j >= 1 &&
		    (lut->data[j].r < lut->data[j - 1].r || lut->data[j].g < lut->data[j - 1].g ||
		     lut->data[j].b < lut->data[j - 1].b)) {
			dev_dbg(dev, "%s: Decreasing GAMMA curve slope at %d.\n", __func__, j);
		}
	}

	return 0;
}

static int check_display_gamma(struct vs_dc *dc, struct vs_crtc_state *crtc_state,
			       const struct vs_display_info *display_info,
			       const struct vs_dc_info *info)
{
	struct device *dev = dc->hw.dev;
	int ret;
	u32 i;

	for (i = 0; i < 3; i++) {
		struct drm_vs_gamma_lut *lut = NULL;

		if ((i == 0) && crtc_state->prior_gamma.blob && crtc_state->prior_gamma.changed) {
			if (!display_info->gamma) {
				dev_err(dev, "%s: This CRTC not support primary gamma.\n",
					__func__);
				return -EINVAL;
			}
			lut = crtc_state->prior_gamma.blob->data;
		} else if ((i == 1) && crtc_state->roi0_gamma.blob &&
			   crtc_state->roi0_gamma.changed) {
			if (!display_info->gamma || !display_info->lut_roi) {
				dev_err(dev, "%s: This CRTC not support roi0 gamma.\n", __func__);
				return -EINVAL;
			}
			lut = crtc_state->roi0_gamma.blob->data;
		} else if ((i == 2) && crtc_state->roi1_gamma.blob &&
			   crtc_state->roi1_gamma.changed) {
			if (!display_info->gamma || !display_info->lut_roi) {
				dev_err(dev, "%s: This CRTC not support roi1 gamma.\n", __func__);
				return -EINVAL;
			}
			lut = crtc_state->roi1_gamma.blob->data;
		}

		if (lut) {
			ret = check_display_gamma_lut(dc, lut, info);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int get_blend_id(const unsigned long blend_mask, unsigned int zpos)
{
	int bit, i = 0;

	static_assert(HW_PLANE_NUM < (sizeof(blend_mask) * 8));

	for_each_set_bit(bit, &blend_mask, HW_PLANE_NUM) {
		if (i == zpos)
			return bit;
		i++;
	}
	return -EINVAL;
}

static int check_display_blend(struct vs_dc *dc, struct drm_crtc *crtc,
			       struct drm_crtc_state *crtc_state)
{
	struct device *dev = dc->hw.dev;
	struct drm_atomic_state *state = crtc_state->state;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	struct vs_plane_state *vs_plane_state;
	unsigned long blend_mask = 0;
	int i;

	drm_for_each_plane_mask(plane, state->dev, crtc_state->plane_mask) {
		unsigned long bm;

		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);

		vs_plane_state = to_vs_plane_state(plane_state);

		bm = BIT(vs_plane_state->blend_id);
		if (bm & blend_mask) {
			dev_warn(dev, "%s: [%s] %s has duplicate blend id:%d blend_mask:%#lx\n",
				 __func__, crtc->name, plane->name, vs_plane_state->blend_id,
				 blend_mask);

				return -EINVAL;
		}
		blend_mask |= bm;
	}

	for_each_new_plane_in_state(state, plane, plane_state, i) {
		int blend_id;

		if (plane_state->crtc != crtc)
			continue;

		blend_id = get_blend_id(blend_mask, plane_state->normalized_zpos);
		if (blend_id < 0) {
			dev_err(dev, "%s: [%s] invalid blend id for %s zpos:%d mask:%#lx\n",
				__func__, crtc->name, plane->name, plane_state->normalized_zpos,
				blend_mask);
			return blend_id;
		}
		vs_plane_state = to_vs_plane_state(plane_state);
		vs_plane_state->blend_id = blend_id;
	}

	return 0;
}

static int check_blend_size(struct device *dev, struct drm_crtc *crtc,
			    struct drm_crtc_state *crtc_state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);

	/* if bld_size is not set explicitly during mode change, default to current mode */
	if (crtc_state->mode_changed && vs_crtc->bld_size_prop &&
	    !vs_crtc_state->bld_size_changed) {
		struct drm_display_mode *mode = &crtc_state->adjusted_mode;

		vs_crtc_state->bld_size = (mode->hdisplay << 16) | (mode->vdisplay & 0xFFFF);
		dev_dbg(dev, "%s: default bld_size to new mode (0x%08x)\n", __func__,
			vs_crtc_state->bld_size);
	}

	return 0;
}

static int check_seamless_mode_change(struct device *dev, struct drm_crtc *crtc,
				      struct drm_crtc_state *crtc_state)
{
	struct drm_atomic_state *state = crtc_state->state;
	struct drm_crtc_state *old_crtc_state = drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_display_mode *new_mode = &crtc_state->mode;
	struct drm_display_mode *old_mode = &old_crtc_state->mode;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);
	bool seamless_mode_change = false;
	bool is_cmd_mode = vs_crtc_state->output_mode & VS_OUTPUT_MODE_CMD;

	if (!crtc_state->mode_changed)
		return 0;

	if (!is_cmd_mode || crtc_state->active_changed) {
		dev_dbg(dev, "%s: is_cmd_mode:%d active_changed:%d\n",
			crtc->name, is_cmd_mode, crtc_state->active_changed);
		goto end;
	}

	if ((old_mode->vdisplay == new_mode->vdisplay) &&
	    (old_mode->hdisplay == new_mode->hdisplay))
		seamless_mode_change = true;

end:
	vs_crtc_state->seamless_mode_change = seamless_mode_change;
	dev_dbg(dev, "%s: %s mode change detected (%s) -> (%s) when CRTC is %s\n", crtc->name,
		seamless_mode_change ? "seamless" : "full", old_mode->name, new_mode->name,
		crtc_state->active ? "active" : "inactive");

	return 0;
}

/*
 * @brief Validate histogram channel configuration
 */
static int check_display_hist_chans(const struct vs_dc *dc,
				    const struct vs_crtc *vs_crtc,
				    const struct vs_crtc_state *crtc_state)
{
	const struct dc_hw *hw = &dc->hw;

	if (!vs_dc_hist_chans_check(hw, vs_crtc->id, crtc_state))
		return -EINVAL;

	return 0;
}

static int vs_dc_check_wb_r2y(struct device *dev, struct vs_crtc *vs_crtc,
			      struct vs_writeback_connector_state *vs_wb_state,
			      struct drm_framebuffer *fb)
{
	struct drm_atomic_state *atomic_state = vs_wb_state->base.state;
	struct drm_crtc_state *crtc_state;
	struct vs_crtc_state *vs_crtc_state;
	const struct drm_vs_r2y_config *r2y;

	if (!fb->format->is_yuv)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(atomic_state, &vs_crtc->base);
	vs_crtc_state = to_vs_crtc_state(crtc_state);

	r2y = vs_dc_drm_crtc_property_get(vs_crtc_state, "R2Y", NULL);

	/*
	 * To maintain compatibility with post-blend R2Y, the blob provides VSI specific enums for
	 * mode and gamut, instead of upstream DRM notions of color encoding & range.
	 * If no blob is set and the wb format is YUV, the expectation is that the input format is
	 * also YUV. Since Y2R is always set for YUV input buffers, R2Y must be configured with
	 * matching parameters.
	 */
	if (r2y) {
		if (r2y->mode > VS_CSC_CM_F2F) {
			dev_err(dev, "Invalid r2y mode %#x for crtc %d\n", r2y->mode, vs_crtc->id);
			return -EINVAL;
		}

		if (r2y->gamut > VS_CSC_CG_SRGB) {
			dev_err(dev, "Invalid r2y gamut %#x for crtc %d\n", r2y->gamut,
				vs_crtc->id);
			return -EINVAL;
		}
	} else {
		struct drm_plane *plane;
		const struct drm_plane_state *plane_state;
		bool found_yuv_plane = false;

		drm_atomic_crtc_state_for_each_plane_state(plane, plane_state, crtc_state) {
			if (!plane_state->fb->format->is_yuv)
				continue;

			vs_wb_state->color_encoding = plane_state->color_encoding;
			vs_wb_state->color_range = plane_state->color_range;
			found_yuv_plane = true;
			break;
		}

		if (!found_yuv_plane) {
			dev_err(dev,
				"BLDWB R2Y blob required but not set on crtc %d! Output format is YUV but input is not.\n",
				vs_crtc->id);
			return -EINVAL;
		}
	}

	return 0;
}

static int vs_dc_check_display(struct device *dev, struct drm_crtc *crtc,
			       struct drm_crtc_state *crtc_state)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);
	struct vs_display_info *display_info;
	int ret = 0;

	display_info = (struct vs_display_info *)&dc->hw.info->displays[vs_crtc->id];
	if (!display_info)
		return -EINVAL;

	ret = check_display_blend(dc, crtc, crtc_state);
	if (ret)
		return ret;

	ret = check_display_gamma(dc, vs_crtc_state, display_info, dc->hw.info);
	if (ret)
		return ret;

	ret = check_blend_size(dev, crtc, crtc_state);
	if (ret)
		return ret;

	ret = check_seamless_mode_change(dev, crtc, crtc_state);
	if (ret)
		return ret;

	ret = check_display_blur_mask(dc, vs_crtc_state, display_info);
	if (ret)
		return ret;

	ret = check_display_brightness_mask(dc, vs_crtc_state, display_info);
	if (ret)
		return ret;

	ret = check_display_hist_chans(dc, vs_crtc, vs_crtc_state);
	if (ret)
		return ret;

	if (!vs_dc_check_drm_property(dc, display_info->id, vs_crtc_state->drm_states,
				      vs_crtc->properties.num, vs_crtc_state))
		return -EINVAL;

	return ret;
}

static void vs_dc_enable_vblank(struct vs_crtc *vs_crtc, bool enable)
{
	struct device *dev = vs_crtc->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);

	dc_hw_enable_vblank_irqs(&dc->hw, vs_crtc->id, enable);
}

static u32 vs_dc_get_vblank_count(struct vs_crtc *crtc)
{
	struct device *dev = crtc->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);

	return dc_hw_get_vblank_count(&dc->hw, crtc->id);
}

static int vs_dc_get_crtc_scanout_position(struct device *dev, struct drm_crtc *crtc, u32 *position)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	u8 display_id = to_vs_display_id(dc, crtc);

	if (display_id >= HW_DISPLAY_4) {
		dev_warn(dev, "[%s] crtc %u : not support scanoutpos query.\n", __func__,
			 display_id);
		return -EINVAL;
	}

	dc_hw_get_crtc_scanout_position(&dc->hw, display_id, position);
	return 0;
}

static void dc_fbc_commit(struct vs_dc *dc, u8 display_id)
{
	u8 i, layer_num = dc->hw.info->layer_num;
	struct dc_hw_plane *plane;

	if (!dc->hw.info->cap_dec)
		return;

	for (i = 0; i < layer_num; i++) {
		plane = &dc->hw.plane[i];
		if (plane->fb.display_id != display_id)
			continue;

		if (!dc->planes[i].pvric.reqt.dirty)
			continue;

		dc_pvric_commit(&dc->planes[i].pvric, &dc->hw, PVRIC_PLANE, i);
	}
}

static void dc_wait_earliest_process_time(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	const struct drm_crtc_state *old_crtc_state =
				drm_atomic_get_old_crtc_state(state, crtc);
	const struct drm_crtc_state *new_crtc_state;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	int te_freq, vsync_period_ns;
	ktime_t earliest_process_time, expected_process_duration_ns, now;

	te_freq = gs_drm_mode_te_freq(&old_crtc_state->mode);
	if (!te_freq) {
		new_crtc_state = crtc->state;
		te_freq = gs_drm_mode_te_freq(&new_crtc_state->mode);
	}

	vsync_period_ns = mult_frac(1000, 1000 * 1000, te_freq);
	expected_process_duration_ns = mult_frac(vsync_period_ns, 3, 4);
	if (ktime_compare(vs_crtc_state->expected_present_time,
			expected_process_duration_ns) <= 0)
		return;

	earliest_process_time = ktime_sub_ns(vs_crtc_state->expected_present_time,
						expected_process_duration_ns);
	now = ktime_get();

	if (ktime_after(earliest_process_time, now)) {
		/*
		 * Maximum delay is 100ms for 10 Hz.
		 * Do not rely on |vsync_period_ns| as it varies with VRR configurations.
		 */
		const int32_t max_delay_us = MAX_DC_WAIT_EARLIEST_PROCESS_TIME_USEC;
		int32_t delay_until_process;

		delay_until_process = (int32_t)ktime_us_delta(earliest_process_time, now);
		if (delay_until_process > max_delay_us) {
			delay_until_process = max_delay_us;
			pr_warn("expected present time seems incorrect(now %llu, earliest %llu)\n",
				now, earliest_process_time);
		}
		DPU_ATRACE_BEGIN("wait for earliest present (vsync:%d, delay %dus)",
				  te_freq, delay_until_process);
		usleep_range(delay_until_process, delay_until_process + 10);
		DPU_ATRACE_END("wait for earliest process time");
	}
}

static void vs_dc_commit(struct device *dev, struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	u8 display_id = to_vs_display_id(dc, crtc);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	unsigned long flags;
	bool need_boost_fabrt;

	trace_disp_commit_begin(display_id, crtc->state);
	/* TBD !
	 * developer should update the function implementation
	 * according to actual requirements during developing !
	 */

	dc_hw_enable_fe_interrupts(&dc->hw);

	if (vs_crtc_state->seamless_mode_change) {
		dev_dbg(vs_crtc->dc_dev, "%s: [%s] seamless mode changed\n", __func__, crtc->name);
		vs_dc_display_set_mode(dev, crtc, state);
	}

	vs_qos_set_qos_config(dev, crtc);

	need_boost_fabrt = (READ_ONCE(vs_crtc->fboost_state) == VS_FABRT_BOOST_PENDING);
	if (need_boost_fabrt) {
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_BOOSTING);
		kthread_queue_work(vs_crtc->fboost_worker, &vs_crtc->fboost_work);
	}

	dc_hw_enable_shadow_register(&dc->hw, display_id, false);
	dc_fbc_commit(dc, display_id);
	dc_hw_plane_commit(&dc->hw, display_id);
	dc_hw_display_commit(&dc->hw, display_id);

	if (need_boost_fabrt) {
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_RESTORE);
		wake_up_interruptible(&vs_crtc->fboost_wait_q);
	}

	dc_wait_earliest_process_time(crtc, state);

	spin_lock_irqsave(&dc->int_lock, flags);
	if (crtc->state->event && !crtc->state->no_vblank) {
		WARN_ON(vs_crtc->event);
		vs_crtc->event = crtc->state->event;
		crtc->state->event = NULL;
	}
	dc_hw_enable_shadow_register(&dc->hw, display_id, true);

	if (!atomic_add_unless(&vs_crtc->frames_pending, 1, MAX_FRAMES_PENDING_COUNT))
		dev_dbg(dev, "[%s] frames pending overflow\n", crtc->name);

	DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending), vs_crtc->trace_pid,
			       "frames_pending[%u]", crtc->index);
	dc_hw_start_trigger(&dc->hw, display_id, crtc);
	spin_unlock_irqrestore(&dc->int_lock, flags);
	trace_disp_commit_done(display_id, vs_crtc);
}

static void update_wb_fb(struct vs_dc *dc, u32 display_id,
			 struct vs_writeback_connector *wb_connector, struct drm_framebuffer *fb)
{
	struct dc_hw_fb wb_fb = { 0 };
	struct vs_wb_info wb_info = dc->hw.info->write_back[wb_connector->id];

	wb_fb.display_id = display_id;
	wb_fb.address = (u64)wb_connector->dma_addr[0];
	wb_fb.u_address = (u64)wb_connector->dma_addr[1];
	wb_fb.v_address = (u64)wb_connector->dma_addr[2];
	wb_fb.stride = wb_connector->pitch[0];
	wb_fb.u_stride = wb_connector->pitch[1];
	wb_fb.v_stride = wb_connector->pitch[2];
	wb_fb.width = fb->width;
	wb_fb.height = fb->height;
	wb_fb.tile_mode = 0; /* TODO: need to refine */
	wb_fb.enable = true;
	if (wb_info.id == HW_BLEND_WB)
		update_bld_wb_format(fb->format->format, &wb_fb);
	else
		update_wb_format(fb->format->format, &wb_fb);
	update_swizzle(fb->format->format, &wb_fb);
	update_wb_tile_mode(fb, &wb_fb);
	/* TBD */

	dc_hw_update_wb_fb(&dc->hw, wb_connector->id, &wb_fb);
}

static void update_wb_r2y(struct vs_dc *dc, struct vs_crtc *vs_crtc,
			  struct vs_writeback_connector *wb_connector,
			  struct vs_writeback_connector_state *vs_wb_state,
			  struct drm_framebuffer *fb)
{
	struct dc_hw *hw = &dc->hw;
	struct drm_crtc_state *crtc_state = vs_crtc->base.state;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);
	struct dc_hw_r2y r2y_conf = {0};
	struct vs_wb_info wb_info = dc->hw.info->write_back[wb_connector->id];
	const struct drm_vs_r2y_config *r2y_blob;

	/*
	 * Blender writeback has its own R2Y engine. Other R2Y blocks are configured through the
	 * display path, not the writeback path
	 */
	if (wb_info.id != HW_BLEND_WB)
		return;

	r2y_conf.enable = fb->format->is_yuv;

	r2y_blob = vs_dc_drm_crtc_property_get(vs_crtc_state, "R2Y", NULL);
	if (r2y_blob) {
		r2y_conf.gamut = r2y_blob->gamut;
		r2y_conf.mode = r2y_blob->mode;

		memcpy(&r2y_conf.coef, &r2y_blob->coef, sizeof(r2y_blob->coef));
	} else {
		r2y_conf.gamut = to_vs_yuv_gamut(vs_wb_state->color_encoding);
		if (vs_wb_state->color_range == DRM_COLOR_YCBCR_FULL_RANGE)
			r2y_conf.mode = CSC_MODE_F2F;
		else
			r2y_conf.mode = CSC_MODE_F2L;
	}

	dc_hw_update_r2y(hw, wb_connector->id, &r2y_conf);
}


static void update_wb_point(struct vs_dc *dc, u8 display_id, u8 wb_id, u32 wb_point)
{
	struct dc_hw_display_wb wb = { 0 };

	wb.enable = true;
	wb.wb_id = wb_id;

	if (wb_id != HW_BLEND_WB)
		wb.wb_point = wb_point;

	dc_hw_update_display_wb(&dc->hw, display_id, &wb);
}

static void update_wb_stall(struct vs_dc *dc, u8 wb_id)
{
	const struct vs_wb_info *wb_info = &dc->hw.info->write_back[wb_id];

	if (wb_info->wb_stall)
		dc_hw_set_wb_stall(&dc->hw, true);
	else
		dev_dbg(dc->hw.dev, "%s: wb_stall not supported on wb[%d].\n", __func__, wb_id);
}

static void vs_dc_conf_writeback(struct vs_writeback_connector *wb_connector,
				 struct drm_framebuffer *fb)
{
	struct device *dev = wb_connector->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct drm_connector_state *state = wb_connector->base.base.state;
	struct vs_writeback_connector_state *vs_wb_state = to_vs_writeback_connector_state(state);
	struct dc_hw_wb *wb = &dc->hw.wb[wb_connector->id];
	struct vs_crtc *vs_crtc;
	int i;
	bool ret;

	if (!state->crtc) {
		dev_err(dev, "wb connector is not connected to a valid CRTC!\n");
		return;
	}

	vs_crtc = to_vs_crtc(state->crtc);
	update_wb_fb(dc, vs_crtc->id, wb_connector, fb);

	update_wb_r2y(dc, vs_crtc, wb_connector, vs_wb_state, fb);

	update_wb_point(dc, vs_crtc->id, wb_connector->id, vs_wb_state->wb_point);

	update_wb_stall(dc, wb_connector->id);

	for (i = 0; i < wb_connector->properties.num; i++) {
		ret = vs_dc_update_drm_property(dc, wb_connector->id, &vs_wb_state->drm_states[i],
						wb->states.items[i].proto, &wb->states.items[i],
						vs_wb_state);

		if (ret && !wb->states.items[i].proto->update)
			trace_update_hw_wb_feature_en_dirty(wb->states.items[i].proto->name,
							    wb_connector->id,
							    wb->states.items[i].enable,
							    wb->states.items[i].dirty);
	}

	dc_hw_setup_wb(&dc->hw, wb_connector->id);

	dc_hw_config_wb_status(&dc->hw, wb_connector->id, true);

	vs_dpu_link_node_config(&dc->hw, VS_DPU_LINK_WB, wb_connector->id,
				dc->hw.display[vs_crtc->id].sbs_split_dirty ?
					((vs_crtc->id >> 1) ? 2 : 0) :
					vs_crtc->id,
				true);
}

static void vs_dc_disable_writeback(struct vs_writeback_connector *wb_connector)
{
	struct device *dev = wb_connector->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct dc_hw_fb wb_fb = { 0 };
	struct dc_hw_display_wb wb = { 0 };

	wb.enable = false;
	wb.wb_id = wb_connector->id;

	if (wb_connector->crtc)
		dc_hw_update_display_wb(&dc->hw, wb_connector->crtc->id, &wb);

	wb_fb.enable = false;
	dc_hw_update_wb_fb(&dc->hw, wb_connector->id, &wb_fb);

	dc_hw_setup_wb(&dc->hw, wb_connector->id);
	dc_hw_config_wb_status(&dc->hw, wb_connector->id, true);
}

static int vs_dc_check_wb_point(struct vs_dc *dc, int wb_id,
				const struct vs_display_info *display_info, const u32 wb_point)
{
	struct device *dev = dc->hw.dev;

	if (wb_id == HW_BLEND_WB)
		return 0;

	switch (wb_point) {
	case VS_WB_DISP_IN:
		if (!display_info->disp_in_wb) {
			dev_err(dev, "display[%u] has unsupported writeback point!\n",
				display_info->id);
			return -EINVAL;
		}
		break;
	case VS_WB_DISP_CC:
		if (!display_info->disp_cc_wb) {
			dev_err(dev, "display[%u] has unsupported writeback point!\n",
				display_info->id);
			return -EINVAL;
		}
		break;
	case VS_WB_DISP_OUT:
		if (!display_info->disp_out_wb) {
			dev_err(dev, "display[%u] has invalid writeback point!\n",
				display_info->id);
			return -EINVAL;
		}
		break;
	case VS_WB_OFIFO_IN:
		if (!display_info->ofifo_in_wb) {
			dev_err(dev, "display[%u] has invalid writeback point!\n",
				display_info->id);
			return -EINVAL;
		}
		break;
	case VS_WB_OFIFO_OUT:
		if (!display_info->ofifo_out_wb) {
			dev_err(dev, "display[%u] has unsupported writeback point!\n",
				display_info->id);
			return -EINVAL;
		}
		break;
	default:
		dev_err(dev, "%s has invalid writeback point!\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int vs_dc_check_wb_mod(const struct vs_wb_info *wb_info, u64 modifier)
{
	const u64 *mods = NULL;

	uint64_t vs_modifier = vs_fb_parse_fourcc_modifier(modifier);

	for (mods = wb_info->modifiers; *mods != DRM_FORMAT_MOD_INVALID; mods++) {
		if (*mods == vs_modifier)
			return 0;
	}

	return -EINVAL;
}

static int vs_dc_check_writeback(struct vs_writeback_connector *wb_connector,
				 struct drm_framebuffer *fb, struct drm_display_mode *mode,
				 struct drm_connector_state *state)
{
	struct device *dev = wb_connector->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	const struct vs_wb_info *wb_info;
	struct vs_writeback_connector_state *vs_wb_state = to_vs_writeback_connector_state(state);
	struct vs_crtc *vs_crtc = to_vs_crtc(state->crtc);
	int ret = 0;
	const struct dc_hw_wb *hw_wb = NULL;
	const struct drm_vs_wb_spliter *spliter = NULL;

	wb_info = &dc->hw.info->write_back[wb_connector->id];
	if (wb_info == NULL)
		return -EINVAL;

	hw_wb = vs_dc_hw_get_wb(&dc->hw, wb_info->id);

	if ((fb->width > wb_info->max_width) || (fb->height > wb_info->max_height)) {
		dev_err(dev, "Invalid framebuffer size %ux%u for writeback\n", fb->width,
			fb->height);
		return -EINVAL;
	}

	ret = vs_dc_check_wb_point(dc, wb_info->id, dc->hw.display[vs_crtc->id].info,
				   vs_wb_state->wb_point);
	if (ret)
		return -EINVAL;

	ret = vs_dc_check_wb_mod(wb_info, fb->modifier);
	if (ret) {
		dev_err(dev, "Invalid framebuffer modifier for writeback\n");
		return ret;
	}

	ret = vs_dc_check_wb_r2y(dev, vs_crtc, vs_wb_state, fb);
	if (ret)
		return ret;

	if (!vs_dc_check_drm_property(dc, wb_info->id, vs_wb_state->drm_states,
				      wb_connector->properties.num, vs_wb_state))
		return -EINVAL;

	if (wb_connector->id == HW_WB_0) {
		if (wb_info->spliter) {
			spliter = vs_dc_drm_connector_property_get(vs_wb_state, "SPLITER", NULL);

			if (spliter != NULL)
				dc->hw.display[hw_wb->fb.display_id].wb_split_dirty = true;
			else
				dc->hw.display[hw_wb->fb.display_id].wb_split_dirty = false;

		} else
			dc->hw.display[hw_wb->fb.display_id].wb_split_dirty = false;
	}

	return 0;
}

const struct vs_writeback_funcs dc_writeback_funcs = {
	.config = vs_dc_conf_writeback,
	.disable = vs_dc_disable_writeback,
	.check = vs_dc_check_writeback,
};

const struct vs_crtc_funcs dc_crtc_funcs = {
	.enable = vs_dc_enable,
	.disable = vs_dc_disable,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.show_pattern_config = vs_dc_show_display_pattern_config,
	.set_pattern = vs_dc_set_display_pattern,
	.set_crc = vs_dc_set_display_crc,
	.show_crc = vs_dc_put_display_crc_result,
#endif /* CONFIG_DEBUG_FS */
	.config = vs_dc_conf_display,
	.enable_vblank = vs_dc_enable_vblank,
	.get_vblank_count = vs_dc_get_vblank_count,
	.commit = vs_dc_commit,
	.check = vs_dc_check_display,
	.get_crtc_scanout_position = vs_dc_get_crtc_scanout_position,
};

static const struct of_device_id be_driver_dt_match[] = {
	{ .compatible = "verisilicon,dpu_be" },
	{},
};
MODULE_DEVICE_TABLE(of, be_driver_dt_match);

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int vs_dc_display_debugfs_init(struct device *dev, struct vs_dc *dc,
				      const struct vs_dc_info *dc_info, int display_id)
{
	struct dentry *dentry_display, *dentry_urgent_cmd, *dentry_urgent_vid;
	const struct vs_display_info *display_info = &dc_info->displays[display_id];
	struct dc_hw_display *display = &dc->hw.display[display_id];

	dentry_display = debugfs_create_dir(display_info->name, dc->debugfs);
	if (IS_ERR(dentry_display)) {
		dev_err(dev, "could not create diplay-%d debugfs root folder\n", display_id);
		return PTR_ERR(dentry_display);
	}

	dentry_urgent_cmd = debugfs_create_dir("urgent_cmd_config", dentry_display);
	if (IS_ERR(dentry_urgent_cmd)) {
		dev_err(dev, "could not create urgent_cmd_config in diplay-%d debugfs folder\n",
			display_id);
		return PTR_ERR(dentry_urgent_cmd);
	}

	debugfs_create_u32("h_margin_pct", 0664, dentry_urgent_cmd,
			   &display->urgent_cmd_config.h_margin_pct);
	debugfs_create_u32("v_margin_pct", 0664, dentry_urgent_cmd,
			   &display->urgent_cmd_config.v_margin_pct);
	debugfs_create_u32("delay_counter_usec", 0664, dentry_urgent_cmd,
			   &display->urgent_cmd_config.delay_counter_usec);
	debugfs_create_u32("urgent_value", 0664, dentry_urgent_cmd,
			   &display->urgent_cmd_config.urgent_value);
	debugfs_create_bool("enable", 0664, dentry_urgent_cmd, &display->urgent_cmd_config.enable);

	dentry_urgent_vid = debugfs_create_dir("urgent_vid_config", dentry_display);
	if (IS_ERR(dentry_urgent_vid)) {
		dev_err(dev, "could not create urgent_vid_config in diplay-%d debugfs folder\n",
			display_id);
		return PTR_ERR(dentry_urgent_vid);
	}

	debugfs_create_u32("qos_thresh_0", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.qos_thresh_0);
	debugfs_create_u32("qos_thresh_1", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.qos_thresh_1);
	debugfs_create_u32("qos_thresh_2", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.qos_thresh_2);
	debugfs_create_u32("urgent_thresh_0", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.urgent_thresh_0);
	debugfs_create_u32("urgent_thresh_1", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.urgent_thresh_1);
	debugfs_create_u32("urgent_thresh_2", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.urgent_thresh_2);
	debugfs_create_u32("urgent_low_thresh", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.urgent_low_thresh);
	debugfs_create_u32("urgent_high_thresh", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.urgent_high_thresh);
	debugfs_create_u32("healthy_thresh", 0664, dentry_urgent_vid,
			   &display->urgent_vid_config.healthy_thresh);
	debugfs_create_bool("enable", 0664, dentry_urgent_vid, &display->urgent_vid_config.enable);

	return 0;
}
#else
static int vs_dc_display_debugfs_init(struct device *dev, struct vs_dc *dc,
				      const struct vs_dc_info *dc_info, int display_id)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

static int be_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct vs_drm_private *priv = drm_dev->dev_private;
	struct vs_dc *dc = dev_get_drvdata(priv->dc_dev);
	struct device_node *port;
	struct vs_crtc *crtc;
	struct drm_crtc *drm_crtc;
	const struct vs_dc_info *dc_info;
	const struct vs_display_info *display_info;
	struct dc_hw_display *display;
	int i, ret;
	u32 max_width = 0, max_height = 0;
	u8 hw_id;

	if (!drm_dev || !dc) {
		dev_err(dev, "devices are not created.\n");
		return -ENODEV;
	}

	ret = vs_dc_power_get(priv->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power ON\n", __func__);

	dev_set_drvdata(dev, dc);

	ret = dc_be_hw_init(&dc->hw);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize be hardware.\n");
		return ret;
	}

	ret = vs_dc_power_put(priv->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "failed to power OFF\n");

	dc_info = dc->hw.info;
	for (i = 0; i < dc_info->display_num; i++) {
		display_info = &dc_info->displays[i];
		display = &dc->hw.display[i];
		hw_id = display_info->id;

		port = of_graph_get_port_by_id(priv->dc_dev->of_node, display_info->id);
		if (!port) {
			dev_warn(dev, "port node not found for display #%d\n", display_info->id);
			continue;
		}

		crtc = vs_crtc_create(display, drm_dev, dc, dc_info, i);
		if (!crtc) {
			dev_err(dev, "Failed to create CRTC.\n");
			ret = -ENOMEM;
			goto err_cleanup_crtcs;
		}

		crtc->base.port = port;
		crtc->id = i;

		crtc->dev = dev;
		crtc->dc_dev = priv->dc_dev;
		crtc->funcs = &dc_crtc_funcs;
		dc->crtc[i] = crtc;
		dc->crtc_mask |= drm_crtc_mask(&crtc->base);

		display_info = &dc_info->displays[i];
		max_width = (max_width < display_info->max_width) ? display_info->max_width :
								    max_width;
		max_height = (max_height < display_info->max_height) ? display_info->max_height :
								       max_height;

		vs_dc_display_debugfs_init(dev, dc, dc_info, hw_id);

		WRITE_ONCE(crtc->fboost_state, VS_FABRT_BOOST_INIT);

		/* create fabrt boosting thread once the boost_fabrt_freq is set */
		if (dc->boost_fabrt_freq != 0) {
			struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

			init_waitqueue_head(&crtc->fboost_wait_q);
			crtc->fboost_worker = kthread_create_worker(0, "crtc%d_boost_fabrt", i);
			sched_setscheduler_nocheck(crtc->fboost_worker->task, SCHED_FIFO, &param);
			kthread_init_work(&crtc->fboost_work, dc_fabrt_boost_kwork);
		}
	}

	if (!dc->crtc_mask) {
		dev_err(dev, "no port found\n");
		ret = -ENOENT;
		goto err_cleanup_crtcs;
	}

	drm_dev->mode_config.max_width = (max_width < drm_dev->mode_config.max_width) ?
						 drm_dev->mode_config.max_width :
						 max_width;
	drm_dev->mode_config.max_height = (max_height < drm_dev->mode_config.max_height) ?
						  drm_dev->mode_config.max_height :
						  max_height;

	vs_drm_update_alignment(drm_dev, dc_info->pitch_alignment, dc_info->addr_alignment);

	return 0;

err_cleanup_crtcs:
	drm_for_each_crtc(drm_crtc, drm_dev)
		vs_crtc_destroy(drm_crtc);

	return ret;
}

static void be_unbind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct drm_crtc *drm_crtc = data;
	struct vs_dc *dc = dev_get_drvdata(dev);

	dc_be_hw_deinit(&dc->hw);

	drm_for_each_crtc(drm_crtc, drm_dev)
		vs_crtc_destroy(drm_crtc);
}

static const struct component_ops be_component_ops = {
	.bind = be_bind,
	.unbind = be_unbind,
};

static int dc_be_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	return component_add(dev, &be_component_ops);
}

static int dc_be_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	component_del(dev, &be_component_ops);

	dev_set_drvdata(dev, NULL);

	return 0;
}

struct platform_driver dc_be_platform_driver = {
	.probe = dc_be_probe,
	.remove = dc_be_remove,

	.driver = {
		.name = "vs-dc-be",
		.of_match_table = of_match_ptr(be_driver_dt_match),
	},
};

static const struct of_device_id wb_driver_dt_match[] = {
	{ .compatible = "verisilicon,dpu_wb" },
	{},
};
MODULE_DEVICE_TABLE(of, wb_driver_dt_match);

static int wb_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct vs_drm_private *priv = drm_dev->dev_private;
	struct vs_dc *dc = dev_get_drvdata(priv->dc_dev);
	struct drm_plane *drm_plane, *tmp;
	const struct vs_dc_info *dc_info;
	const struct vs_display_info *display_info;
	struct dc_hw_wb *hw_wb;
	int i, ret;
	u32 max_width = 0, max_height = 0;
	struct vs_writeback_connector *writeback;
	const struct vs_wb_info *wb_info;
	u32 j, valid_crtcs = 0;

	if (!drm_dev || !dc) {
		dev_err(dev, "devices are not created.\n");
		return -ENODEV;
	}

	ret = vs_dc_power_get(priv->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power ON\n", __func__);

	dev_set_drvdata(dev, dc);

	ret = dc_wb_hw_init(&dc->hw);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize wb hardware.\n");
		return ret;
	}

	ret = vs_dc_power_put(priv->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "failed to power OFF\n");

	dc_info = dc->hw.info;
	for (i = 0; i < dc_info->wb_num; i++) {
		wb_info = &dc_info->write_back[i];
		hw_wb = &dc->hw.wb[i];

		valid_crtcs = 0;
		for (j = 0; j < dc_info->display_num; j++) {
			if (!dc->crtc[j])
				continue;

			display_info = &dc_info->displays[j];

			if (wb_info->src_mask & BIT(display_info->id))
				valid_crtcs |= drm_crtc_mask(&dc->crtc[j]->base);
		}

		writeback = vs_writeback_create(hw_wb, drm_dev, wb_info, valid_crtcs);

		if (!writeback) {
			dev_err(dev, "Failed to create writeback connector.\n");
			ret = -ENOMEM;
			goto err_cleanup_planes;
		}

		writeback->id = i;
		writeback->dev = dev;
		writeback->funcs = &dc_writeback_funcs;
		init_waitqueue_head(&writeback->framedone_waitq);
		writeback->crtc = NULL;
		dc->writeback[i] = writeback;

		/*
		 * Note: these values are used for multiple independent things:
		 * hw display mode filtering, plane buffer sizes, writeback buffer size ...
		 * Use the combined maximum values here to cover all use cases, and do more
		 * specific checking in the respective code paths.
		 */
		max_width = (max_width < wb_info->max_width) ? wb_info->max_width : max_width;
		max_height = (max_height < wb_info->max_height) ? wb_info->max_height : max_height;
	}

	drm_dev->mode_config.max_width = (max_width < drm_dev->mode_config.max_width) ?
						 drm_dev->mode_config.max_width :
						 max_width;
	drm_dev->mode_config.max_height = (max_height < drm_dev->mode_config.max_height) ?
						  drm_dev->mode_config.max_height :
						  max_height;

	vs_drm_update_alignment(drm_dev, dc_info->pitch_alignment, dc_info->addr_alignment);

	return 0;

err_cleanup_planes:
	list_for_each_entry_safe(drm_plane, tmp, &drm_dev->mode_config.plane_list, head)
		if (drm_plane->possible_crtcs & valid_crtcs)
			vs_plane_destroy(drm_plane);
	return ret;
}

static void wb_unbind(struct device *dev, struct device *master, void *data)
{
	struct drm_connector *drm_connector = data;

	drm_connector_cleanup(drm_connector);
	kfree(drm_connector);
}

static const struct component_ops wb_component_ops = {
	.bind = wb_bind,
	.unbind = wb_unbind,
};

static int dc_wb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	return component_add(dev, &wb_component_ops);
}

static int dc_wb_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	component_del(dev, &wb_component_ops);

	dev_set_drvdata(dev, NULL);

	return 0;
}

struct platform_driver dc_wb_platform_driver = {
	.probe = dc_wb_probe,
	.remove = dc_wb_remove,

	.driver = {
		.name = "vs-dc-wb",
		.of_match_table = of_match_ptr(wb_driver_dt_match),
	},
};

MODULE_DESCRIPTION("VeriSilicon BE_WB Driver");
MODULE_LICENSE("GPL v2");
