/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 VeriSilicon Holdings Co., Ltd.
 */
#ifndef __VS_DC_INFO_H__
#define __VS_DC_INFO_H__

#include <drm/drm_blend.h>
#include <drm/drm_atomic_helper.h>
#include <drm/vs_drm.h>
#include <linux/kernel.h>
#include <linux/types.h>

/* TODO: Different chip have differnert DC_XXX_NUM.
 *       Need find way to separte.
 *       For now, use the greatest common divisor of all.
 */
#if IS_ENABLED(CONFIG_VERISILICON_PLANE_RCD)
#define DC_PLANE_NUM 14
#else
#define DC_PLANE_NUM 12
#endif
#define DC_DISPLAY_NUM 5
#define DC_OUTPUT_NUM 6
#define DC_WB_NUM 3

/* Chip IDs for 9x00 */
#define VS_CHIP_ID_9200 0x9200
#define VS_CHIP_ID_9400 0x9400

/* Compile time assert */
#define vs_dc_ct_assert(cond, msg) __vs_dc_assert_impl(cond, __LINE__, msg)
#define __vs_dc_assert_paste(msg, line) msg##line
#define __vs_dc_assert_impl(cond, line, msg) \
	typedef char __vs_dc_assert_paste(assert_failed_##msg##_, line)[2 * !!(cond) - 1]

#define VS_OUTPUT_MODE_CMD BIT(0)
#define VS_OUTPUT_MODE_CMD_AUTO BIT(1)
#define VS_OUTPUT_MODE_CMD_DE_SYNC BIT(2)

#define VS_PLANE_NO_SCALING DRM_PLANE_NO_SCALING

/* DMA Alignment Requirements */
#define VS_ALIGN_YUV420 2
#define VS_ALIGN_YUV420_PVRIC_FB_HEIGHT 8
#define VS_ALIGN_ARGB_PVRIC_FB_HEIGHT 4
#define VS_ALIGN_PVRIC_STRIDE 64
#define VS_ALIGN_PVRIC_BASE 256
#define VS_ALIGN_STRIDE 32

/* Memory Constraints */
#define VS_MAX_TCU_SIZE 512

struct regmap_access_table;

enum dc_chip_rev {
	DC_REV_0, /* For HW_9400 */
	DC_REV_1, /* For HW_9500 */
};

enum dc_hw_plane_id {
	HW_PLANE_0,
	HW_PLANE_1,
	HW_PLANE_2,
	HW_PLANE_3,
	HW_PLANE_4,
	HW_PLANE_5,
	HW_PLANE_6,
	HW_PLANE_7,
	HW_PLANE_8,
	HW_PLANE_9,
	HW_PLANE_10,
	HW_PLANE_11,
	HW_PLANE_12,
	HW_PLANE_13,
	HW_PLANE_14,
	HW_PLANE_15,
	HW_PLANE_NUM,
};

/* Secure ID per plane*/
enum dc_hw_plane_sid {
	HW_PLANE_0_SID = 1,
	HW_PLANE_1_SID,
	HW_PLANE_2_SID,
	HW_PLANE_3_SID,
	HW_PLANE_4_SID,
	HW_PLANE_5_SID,
	HW_PLANE_6_SID,
	HW_PLANE_7_SID,
	HW_PLANE_8_SID,
	HW_PLANE_9_SID,
	HW_PLANE_10_SID,
	HW_PLANE_11_SID,
	HW_PLANE_12_SID,
	HW_PLANE_13_SID,
	HW_PLANE_14_SID,
	HW_PLANE_15_SID,
	HW_PLANE_NUM_SIDS,
	HW_PLANE_NOT_SUPPORTED_SID,
};

enum dc_hw_display_id {
	HW_DISPLAY_0,
	HW_DISPLAY_1,
	HW_DISPLAY_2,
	HW_DISPLAY_3,
	HW_DISPLAY_4, /* Only valid for DC_REV_0,
		       * corresponding to the blend wb path.
		       */
	HW_DISPLAY_5, /* Only valid for DC_REV_1,
		       * corresponding to the layer corssbar path.
		       */
	HW_DISPLAY_NUM,
};

enum dc_hw_wb_id {
	HW_WB_0,
	HW_WB_1,
	HW_BLEND_WB,
	HW_WB_NUM,
};

struct vs_dc_urgent_cmd_config {
	u32 h_margin_pct;
	u32 v_margin_pct;
	u32 delay_counter_usec;
	u32 urgent_value;
	bool enable;
};

struct vs_dc_urgent_vid_config {
	u32 qos_thresh_0;
	u32 qos_thresh_1;
	u32 qos_thresh_2;
	u32 urgent_thresh_0;
	u32 urgent_thresh_1;
	u32 urgent_thresh_2;
	u32 urgent_low_thresh;
	u32 urgent_high_thresh;
	u32 healthy_thresh;
	bool enable;
};

struct vs_plane_info {
	const char *name;
	u8 id;
	u8 sid; /* Secure ID per layer */
	u8 fe_id;
	enum drm_plane_type type;
	u8 crtc_id; /* default crtc index, only for primary plane */
	unsigned int num_formats;
	const u32 *formats;
	u8 num_modifiers;
	const u64 *modifiers;
	unsigned int min_width;
	unsigned int min_height;
	unsigned int max_width;
	unsigned int max_yuv_width;
	unsigned int max_height;
	unsigned int rotation;
	unsigned int blend_mode;
	unsigned int color_encoding;
	unsigned int color_range;

	/* 0 means no de-gamma LUT */
	unsigned int degamma_size;

	int min_scale; /* 16.16 fixed point */
	int max_scale; /* 16.16 fixed point */

	/* default zorder value, and 255 means unsupported zorder capability */
	u8 zpos;

	u8 max_uv_phase; /* for uv up-sampling */

	u8 axi_id;
	u8 outstanding_number;

	u32 color_mgmt : 1;
	u32 program_csc : 1;
	u32 roi : 1;
	u32 roi_two : 1;
	u32 roi_skip : 1;
	u32 layer_ext : 1;
	u32 layer_ext_ex : 1;
	u32 cgm_lut : 1;
	u32 alpha_ext : 1;
	u32 demultiply : 1;
	u32 hdr : 1;
	u32 tone_map : 1;
	u32 blend_config : 1; /* for BLEND_MODE, BLEND_ALPHA properties */
	u32 crc : 1;
	u32 test_pattern : 1;
	u32 compressed : 1;
	u32 watermark : 1;
	u32 sbs : 1;
	u32 line_padding : 1;
	u32 rcd_plane : 1;
};

struct vs_display_info {
	const char *name;
	u8 id;
	unsigned int color_formats;
	unsigned int max_width;
	unsigned int max_height;

	int min_scale; /* 16.16 fixed point */
	int max_scale; /* 16.16 fixed point */

	u32 bld_size : 1;
	u32 background : 1;
	u32 bld_cgm : 1; /* for blend EOTF, gamut map modules */
	u32 bld_oetf : 1;
	u32 bld_dth : 1;
	u32 gamma_dth : 1;
	u32 panel_dth : 1;
	u32 llv_dth : 1;
	u32 ltm : 1; /* local tone mapping module */
	u32 gtm : 1;
	u32 sharpness : 1;
	u32 brightness : 1;
	u32 degamma : 1;
	u32 gamma : 1;
	u32 ccm_non_linear : 1;
	u32 ccm_linear : 1;
	u32 cgm_lut : 1;
	u32 lut_roi : 1;
	u32 blur : 1;
	u32 sec_roi : 1;
	u32 data_mode : 1;
	u32 histogram : 1;
	u32 rgb_hist : 1;
	u32 crc : 1;
	u32 test_pattern : 1;
	/* write-back point validity */
	u32 disp_in_wb : 1;
	u32 disp_cc_wb : 1;
	u32 disp_out_wb : 1;
	u32 ofifo_in_wb : 1;
	u32 ofifo_out_wb : 1;
	u32 decompress : 1;
	u32 dsc : 1;
	u32 vdc : 1;
	u32 spliter : 1;
	u32 free_sync : 1;
	u32 panel_crop : 1;
	u32 dp_sync : 1;
	u32 ofifo_splice : 1;
};

struct vs_wb_info {
	const char *name;
	u8 id;
	unsigned int num_formats;
	const u32 *formats;
	const u64 *modifiers;
	unsigned int max_width;
	unsigned int max_height;
	unsigned int rotation;

	int min_scale; /* 16.16 fixed point */
	int max_scale; /* 16.16 fixed point */

	u8 src_mask; /* the mask of valid display path*/

	u8 program_point : 1;
	u8 dither : 1;
	u8 csc : 1; /* for R2Y */
	u8 crc : 1;
	u8 compressed : 1;
	u8 spliter : 1;
	u8 wb_stall : 1;
};

struct vs_dc_info {
	const char *name;
	u32 chip_id;
	u32 revision;
	u32 pid;
	u32 cid;

	/* planes */
	u8 plane_num;
	u8 plane_fe0_num;
	u8 plane_fe1_num;
	const struct vs_plane_info *planes_fe0;
	const struct vs_plane_info *planes_fe1;
	u8 layer_num;
	u8 layer_fe0_num;
	u8 layer_fe1_num;

	/* display */
	u8 display_num;
	const struct vs_display_info *displays;

	/* write back */
	u8 wb_num;
	const struct vs_wb_info *write_back;

	unsigned int max_bpc;
	u16 pitch_alignment;
	u16 addr_alignment;

	/* SRAM POOL SIZE (Kbytes) */
	u16 fe0_dma_sram_size;
	u16 fe1_dma_sram_size;
	u16 fe0_scl_sram_size;
	u16 fe1_scl_sram_size;

	u8 max_blend_layer;
	u8 max_ext_layer;
	u8 max_seg_num;

	/* Regdump Skip Registers */
	const struct regmap_access_table *dump_reg_access_table;

	/* Urgent configuration */
	const struct vs_dc_urgent_cmd_config *urgent_cmd_config;
	const struct vs_dc_urgent_vid_config *urgent_vid_config;

	/* LUT requirement */
	u16 max_eotf_size;
	u16 max_tonemap_size;
	u16 max_oetf_size;
	u16 max_degamma_size;
	u16 max_gamma_size;
	u16 cgm_lut_size; /* for 3D lut */
	u16 cgm_ex_lut_size; /* for roi 3D lut */

	u8 pre_eotf_bits; /* for FE EOTF module */
	u8 hdr_bits;
	u8 oetf_bits; /* for FE OETF module */
	u8 bld_cgm_bits; /* for blend EOTF, gamut map and OETF modules */
	u8 pre_degamma_bits; /* degamma in bits */
	u8 degamma_bits; /* degamma out bits */
	u8 cgm_lut_bits;
	u8 gamma_bits;
	u8 blur_coef_bits;
	u8 intr_dest; /*bit0 for NS, bit1 for TZ, bit2 for GSA, bit3 for AOC*/

	u32 std_color_lut : 1;
	u32 multi_roi : 1;
	u32 pipe_sync : 1;
	u32 mmu_prefetch : 1;
	u32 panel_sync : 1;
	/* cap_dec ===>
	 *  (1 << DRM_FORMAT_MOD_VS_TYPE_COMPRESSED) means support dec400
	 *  (1 << DRM_FORMAT_MOD_VS_TYPE_PVRIC) means support pvric
	 *  (1 << DRM_FORMAT_MOD_VS_TYPE_DECNANO) means support DECNANO
	 *  (1 << DRM_FORMAT_MOD_VS_TYPE_ETC2) means support ETC2
	 */
	u32 cap_dec;
	u8 roi_y_gap;

	u8 vrr : 1;
	u8 crc_roi : 1;

	u16 dma_sram_alignment; /* DMA sram pool buffer line alignment (Byte) */
	u8 dma_sram_extra_buffer : 1;
	u8 dma_sram_unit_size;

	u8 linear_yuv_rotation : 1;

	u8 ppc;
	u8 vblank_margin_pct;
	u8 h_bubble_pct;
	u8 rotation_prefetch_line;
	u8 rotation_pipeline_delay_us;
	u8 rotation_pipeline_latency_us;
	u16 rotation_nominal_cycle_num;
	u16 rotation_nominal_cycle_denom;
	u16 axi_bus_bit_width;
	u8 axi_bus_util_pct;
	u32 max_nv12_uncomp_rot_width;
	u32 max_nv12_uncomp_rot_height;
	u16 fe_axqos_threshold_mbps;
	u32 fe_axqos_high;
	u32 fe_axqos_low;
};

const struct vs_dc_info *vs_dc_get_chip_info(u32 chip_id, u32 revision, u32 pid, u32 cid);
int vs_dc_get_hw_cap(const struct vs_dc_info *info, enum drm_vs_hw_cap_type type, u64 *cap);

const struct vs_plane_info *vs_dc_get_fe0_info(const struct vs_dc_info *info);
const struct vs_plane_info *vs_dc_get_fe1_info(const struct vs_dc_info *info);
const struct vs_display_info *vs_dc_get_be_info(const struct vs_dc_info *info);
const struct vs_wb_info *vs_dc_get_wb_info(const struct vs_dc_info *info);
struct vs_plane_info *get_plane_info(u8 plane_id, const struct vs_dc_info *info);

#endif
