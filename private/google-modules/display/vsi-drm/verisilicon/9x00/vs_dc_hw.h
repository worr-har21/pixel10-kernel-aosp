/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_DC_HW_H__
#define __VS_DC_HW_H__

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <drm/vs_drm.h>
#include <drm/display/drm_dsc.h>

#include "vs_dc_info.h"
#include "vs_dc_property.h"

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
#include <linux/regmap.h>
#endif

#define __vsFIELDSTART(reg_field) \
		(0 ? reg_field)

#define __vsFIELDEND(reg_field) \
		(1 ? reg_field)

#define __vsFIELDSIZE(reg_field) (__vsFIELDEND(reg_field) - __vsFIELDSTART(reg_field) + 1)

#define __vsFIELDALIGN(data, reg_field) (((u32)(data)) << __vsFIELDSTART(reg_field))

#define __vsFIELDMASK(reg_field) \
	((u32)((__vsFIELDSIZE(reg_field) == 32) ? ~0 : (~(~0 << __vsFIELDSIZE(reg_field)))))

/**************************************************************************
 **
 **  VS_SET_FIELD
 **
 **  Set the value of a field within specified data.
 **
 **  ARGUMENTS:
 **
 **  data	Data value.
 **  reg	 Name of register.
 **  field   Name of field within register.
 **  value   Value for field.
 */
#define VS_SET_FIELD(data, reg, field, value)                                             \
	((((u32)(data)) & ~__vsFIELDALIGN(__vsFIELDMASK(reg##_##field), reg##_##field)) | \
	 __vsFIELDALIGN((u32)(value) & __vsFIELDMASK(reg##_##field), reg##_##field))

/*******************************************************************************
 **
 **  VS_SET_FIELD_VALUE
 **
 **      Set the value of a field within specified data with a
 **      predefined value.
 **
 **  ARGUMENTS:
 **
 **      data    Data value.
 **      reg     Name of register.
 **      field   Name of field within register.
 **      value   Name of the value within the field.
 */
#define VS_SET_FIELD_VALUE(data, reg, field, value)                                       \
	((((u32)(data)) & ~__vsFIELDALIGN(__vsFIELDMASK(reg##_##field), reg##_##field)) | \
	 __vsFIELDALIGN(reg##_##field##_##value & __vsFIELDMASK(reg##_##field), reg##_##field))

/**************************************************************************
 **
 **  VS_SET_FIELD_PREDEF
 **
 **  Set the value of a field within specified data with a
 **  predefined value.
 **
 **  ARGUMENTS:
 **
 **  data	Data value.
 **  reg	 Name of register.
 **  field   Name of field within register.
 **  value   Name of the value within the field.
 */
#define VS_SET_FIELD_PREDEF(data, reg, field, value)                                      \
	((((u32)(data)) & ~__vsFIELDALIGN(__vsFIELDMASK(reg##_##field), reg##_##field)) | \
	 __vsFIELDALIGN(reg##_##field##_##value & __vsFIELDMASK(reg##_##field), reg##_##field))

/*******************************************************************************
 **
 **  VS_GET_FIELD
 **
 **  Extract the value of a field from specified data.
 **
 **  ARGUMENTS:
 **
 **  data	Data value.
 **  reg	 Name of register.
 **  field   Name of field within register.
 */
#define VS_GET_FIELD(data, reg, field) \
	(((((u32)(data)) >> __vsFIELDSTART(reg##_##field)) & __vsFIELDMASK(reg##_##field)))

#define VS_SET_FE_FIELD(field0, id, field1)                     \
	(((u32)id < 8) ? VS_SET_FE0_FIELD(field0, id, field1) : \
			 VS_SET_FE1_FIELD(field0, id, field1))

#define VS_LAYER_FIELD(hw_id, field) VS_SET_FE_FIELD(DCREG_LAYER, hw_id, field)
#define VS_SH_LAYER_FIELD(hw_id, field) VS_SET_FE_FIELD(DCREG_SH_LAYER, hw_id, field)
#define VS_SH_PANEL_FIELD(hw_id, field) VS_SET_PANEL_FIELD(DCREG_SH_PANEL, hw_id, field)
#define VS_SH_PANEL01_FIELD(hw_id, field) VS_SET_PANEL01_FIELD(DCREG_SH_PANEL, hw_id, field)

#define VS_SET_PANEL_FIELD_PREDEF(data, hw_id, reg, field, value)                                 \
	(((u32)hw_id == 0) ? VS_SET_FIELD_PREDEF(data, DCREG_SH_PANEL##0##_##reg, field, value) : \
	 ((u32)hw_id == 1) ? VS_SET_FIELD_PREDEF(data, DCREG_SH_PANEL##1##_##reg, field, value) : \
	 ((u32)hw_id == 2) ? VS_SET_FIELD_PREDEF(data, DCREG_SH_PANEL##2##_##reg, field, value) : \
			     VS_SET_FIELD_PREDEF(data, DCREG_SH_PANEL##3##_##reg, field, value))

#define VS_SET_INTR_ADDR(field0, id, field1)                                                    \
	(((u8)id == 1) ? (DCREG_##field0##_TZ_INTR_##field1##_Address) :                        \
	 ((u8)id == 2) ? (DCREG_##field0##_GSA_INTR_##field1##_Address) :                       \
	 ((u8)id == 3 && strcmp(#field0, "BE") == 0) ? (DCREG_BE_AOC_INTR_##field1##_Address) : \
						       (DCREG_##field0##_INTR_##field1##_Address))

#define VS_SET_FE0_FIELD(field0, id, field1)       \
	(((u32)id == 0) ? (field0##0##_##field1) : \
	 ((u32)id == 1) ? (field0##1##_##field1) : \
	 ((u32)id == 2) ? (field0##2##_##field1) : \
	 ((u32)id == 3) ? (field0##3##_##field1) : \
	 ((u32)id == 4) ? (field0##4##_##field1) : \
			  (field0##5##_##field1))

#define VS_SET_FE1_FIELD(field0, id, field1)         \
	(((u32)id == 8)	 ? (field0##8##_##field1) :  \
	 ((u32)id == 9)	 ? (field0##9##_##field1) :  \
	 ((u32)id == 10) ? (field0##10##_##field1) : \
	 ((u32)id == 11) ? (field0##11##_##field1) : \
	 ((u32)id == 12) ? (field0##12##_##field1) : \
			   (field0##13##_##field1))

#define VS_SET_PANEL_FIELD(field0, id, field1)                      \
	(((u32)id < 2) ? VS_SET_PANEL01_FIELD(field0, id, field1) : \
			 VS_SET_PANEL23_FIELD(field0, id, field1))

#define VS_SET_PANEL01_FIELD(field0, id, field1) \
	(((u32)id == 0) ? (field0##0##_##field1) : (field0##1##_##field1))

#define VS_SET_PANEL23_FIELD(field0, id, field1) \
	(((u32)id == 2) ? (field0##2##_##field1) : (field0##3##_##field1))

#define VS_SET_WB_FIELD(field0, id, field1) \
	(((u32)id == 0) ? (field0##0##_##field1) : (field0##1##_##field1))

#define VS_SET_POSTWB_FIELD(field0, id, field1) \
	(((u32)id == 0) ? (field0##0##_##field1) : (field0##1##_##field1))

#define VS_SET_WB_PANEL_FIELD(field0, id0, field1, id1, field2)                       \
	(((u32)id0 == 0) ? (((u32)id1 == 0) ? (field0##0##_##field1##0##_##field2) :  \
			    ((u32)id1 == 1) ? (field0##0##_##field1##1##_##field2) :  \
			    ((u32)id1 == 2) ? (field0##0##_##field1##2##_##field2) :  \
					      (field0##0##_##field1##3##_##field2)) : \
			   (((u32)id1 == 0) ? (field0##1##_##field1##0##_##field2) :  \
			    ((u32)id1 == 1) ? (field0##1##_##field1##1##_##field2) :  \
			    ((u32)id1 == 2) ? (field0##1##_##field1##2##_##field2) :  \
					      (field0##1##_##field1##3##_##field2)))

#define VS_SET_WB_PANEL01_FIELD(field0, id0, field1, id1, field2)                     \
	(((u32)id0 == 0) ? (((u32)id1 == 0) ? (field0##0##_##field1##0##_##field2) :  \
					      (field0##0##_##field1##1##_##field2)) : \
			   (((u32)id1 == 0) ? (field0##1##_##field1##0##_##field2) :  \
					      (field0##1##_##field1##1##_##field2)))

#define VS_SET_OUTPUT_FIELD(field0, id, field1)    \
	(((u32)id == 0) ? (field0##0##_##field1) : \
	 ((u32)id == 1) ? (field0##1##_##field1) : \
	 ((u32)id == 2) ? (field0##2##_##field1) : \
			  (field0##3##_##field1))

#define VS_SET_OUTPUT012_FIELD(field0, id, field1) \
	(((u32)id == 0) ? (field0##0##_##field1) : \
	 ((u32)id == 1) ? (field0##1##_##field1) : \
			  (field0##2##_##field1))

#define VS_SET_LINKNODE_FIELD(field0, id, field1)  \
	(((u32)id == 0) ? (field0##0##_##field1) : \
	 ((u32)id == 1) ? (field0##1##_##field1) : \
	 ((u32)id == 2) ? (field0##2##_##field1) : \
	 ((u32)id == 3) ? (field0##3##_##field1) : \
			  (field0##4##_##field1))

#define UPDATE_PANEL_CONFIG(hw, hw_id, field, value)                                     \
	do {                                                                             \
		const u32 __reg = VS_SH_PANEL_FIELD(hw_id, CONFIG_Address);              \
		u32 __config = dc_read(hw, __reg);                                       \
		__config = VS_SET_FIELD(__config, DCREG_SH_PANEL0_CONFIG, field, value); \
		dc_write(hw, __reg, __config);                                           \
	} while (0)

#define UPDATE_PANEL_CONFIG_EX(hw, hw_id, field, value)                                     \
	do {                                                                                \
		const u32 __reg = VS_SH_PANEL01_FIELD(hw_id, CONFIG_EX_Address);            \
		u32 __config = dc_read(hw, __reg);                                          \
		__config = VS_SET_FIELD(__config, DCREG_SH_PANEL0_CONFIG_EX, field, value); \
		dc_write(hw, __reg, __config);                                              \
	} while (0)

#define VS_SET_PDMAMASTER_FIELD(field0, id, field1)  \
	(((u32)id == 0)	 ? (field0##0##_##field1) :  \
	 ((u32)id == 1)	 ? (field0##1##_##field1) :  \
	 ((u32)id == 2)	 ? (field0##2##_##field1) :  \
	 ((u32)id == 3)	 ? (field0##3##_##field1) :  \
	 ((u32)id == 4)	 ? (field0##4##_##field1) :  \
	 ((u32)id == 5)	 ? (field0##5##_##field1) :  \
	 ((u32)id == 6)	 ? (field0##6##_##field1) :  \
	 ((u32)id == 7)	 ? (field0##7##_##field1) :  \
	 ((u32)id == 8)	 ? (field0##8##_##field1) :  \
	 ((u32)id == 9)	 ? (field0##9##_##field1) :  \
	 ((u32)id == 10) ? (field0##10##_##field1) : \
	 ((u32)id == 11) ? (field0##11##_##field1) : \
	 ((u32)id == 12) ? (field0##12##_##field1) : \
	 ((u32)id == 13) ? (field0##13##_##field1) : \
	 ((u32)id == 14) ? (field0##14##_##field1) : \
	 ((u32)id == 15) ? (field0##15##_##field1) : \
	 ((u32)id == 16) ? (field0##16##_##field1) : \
	 ((u32)id == 17) ? (field0##17##_##field1) : \
	 ((u32)id == 18) ? (field0##18##_##field1) : \
	 ((u32)id == 19) ? (field0##19##_##field1) : \
			   (field0##19##_##field1))

#define VS_GET_START(reg) \
	( \
	(1 ? reg) \
	)

#define VS_GET_END() \
	( \
	(0 ? reg) \
	)

#define R2R_TABLE_SIZE 9
#define FILTER_COEF_NUM 238 /* For 32 phases and 9x5 taps */
#define MAX_CRC_CORE_NUM 2 /* For OFIFO_OUT CRC */
/*****************************************************************
 *
 * TBD !!
 * Developesr should modify the enumeration member
 * according to the actual register definition.
 *
 *****************************************************************/
enum dc_hw_color_format {
	FORMAT_A8R8G8B8 = 0x00,
	FORMAT_X8R8G8B8,
	FORMAT_A2R10G10B10,
	FORMAT_X2R10G10B10,
	FORMAT_R8G8B8,
	FORMAT_R5G6B5,
	FORMAT_A1R5G5B5,
	FORMAT_X1R5G5B5,
	FORMAT_A4R4G4B4,
	FORMAT_X4R4G4B4,
	FORMAT_A16R16G16B16 = 0x0A,
	FORMAT_YUY2,
	FORMAT_UYVY,
	FORMAT_YV12,
	FORMAT_NV12,
	FORMAT_NV16,
	FORMAT_P010,
	FORMAT_P210,
	FORMAT_YUV420_PACKED,
};

enum dc_hw_tile_mode {
	TILE_MODE_LINEAR = 0,
	TILE_MODE_32X2,
	TILE_MODE_16X4,
	TILE_MODE_32X4,
	TILE_MODE_32X8,
	TILE_MODE_16X8,
	TILE_MODE_8X8,
	TILE_MODE_16X16,
	TILE_MODE_8X8_UNIT2X2 = 9,
	TILE_MODE_8X4_UNIT2X2,
};

enum dc_hw_wb_tile_mode {
	WB_TILE_MODE_LINEAR = 0,
	WB_TILE_MODE_16X16,
};

enum dc_hw_wb_format {
	WB_FORMAT_RGB888 = 0,
	WB_FORMAT_ARGB8888,
	WB_FORMAT_XRGB8888,
	WB_FORMAT_A2RGB101010,
	WB_FORMAT_X2RGB101010,
	WB_FORMAT_NV12,
	WB_FORMAT_P010,
};

enum dc_hw_bld_wb_format {
	BLD_WB_FORMAT_ARGB8888 = 0,
	BLD_WB_FORMAT_A2RGB101010,
	BLD_WB_FORMAT_NV12,
	BLD_WB_FORMAT_P010,
	BLD_WB_FORMAT_YV12,
	BLD_WB_FORMAT_NV16,
	BLD_WB_FORMAT_P210,
	BLD_WB_FORMAT_YUV420_PACKED_10BIT,
};

enum dc_hw_csc_gamut {
	CSC_GAMUT_601 = 0,
	CSC_GAMUT_709 = 1,
	CSC_GAMUT_2020 = 2,
	CSC_GAMUT_P3 = 3,
	CSC_GAMUT_SRGB = 4,
};

enum dc_hw_csc_mode {
	CSC_MODE_USER_DEF = 0,
	CSC_MODE_L2L = 1,
	CSC_MODE_L2F = 2,
	CSC_MODE_F2L = 3,
	CSC_MODE_F2F = 4,
};
enum dc_hw_rotation {
	ROT_0,
	ROT_90,
	ROT_180,
	ROT_270,
	FLIP_X,
	FLIP_Y,
	FLIPX_90,
	FLIPY_90,
};

enum dc_hw_swizzle {
	SWIZZLE_ARGB = 0,
	SWIZZLE_RGBA,
	SWIZZLE_ABGR,
	SWIZZLE_BGRA,
};

enum dc_hw_out {
	OUT_DPI,
	OUT_DP,
};

enum dc_hw_dither_pos {
	HW_DTH_PANEL = 0,
	HW_DTH_GAMMA = 1,
	HW_DTH_POS_NUM,
};

enum dc_hw_rdm_dither_pos {
	HW_RDM_DTH_BLD = 0,
	HW_RDM_DTH_LLV = 1, /* TBD */
	HW_RDM_DTH_POS_NUM,
};

enum dc_hw_reset_pos {
	FE0_SW_RESET = 0,
	FE1_SW_RESET,
	BE_SW_RESET,
	SW_RESET_NUM,
};

enum vs_dpu_link_node_type {
	VS_DPU_LINK_LAYER = 0,
	VS_DPU_LINK_POST,
	VS_DPU_LINK_WB,
	VS_DPU_LINK_WB_SPILTER,
	VS_DPU_LINK_DSC,
	VS_DPU_LINK_DBUFFER,
	VS_DPU_LINK_TM,
	RESERVED_TYPE_NUM,
};

enum dc_hw_reg_bank_type {
	DC_HW_REG_BANK_ACTIVE = 0,
	DC_HW_REG_BANK_SHADOW = 1,
};

enum dc_hw_reg_dump_options {
	DC_HW_REG_DUMP_IN_NONE = 0,
	DC_HW_REG_DUMP_IN_CONSOLE = 1,
	DC_HW_REG_DUMP_IN_TRACE = 2,
};

enum dc_hw_cmd_mode {
	DC_HW_CMD_MODE_NONE = 0, /* Default is trigger mode*/
	DC_HW_CMD_MODE_AUTO = 1,
};

enum dc_hw_fe_bus_errors {
	DC_HW_FE_BUS_ERROR_AXI_HANG0,
	DC_HW_FE_BUS_ERROR_AXI_HANG1,
	DC_HW_FE_BUS_ERROR_AXI_BUS_ERROR0,
	DC_HW_FE_BUS_ERROR_AXI_BUS_ERROR1,
	DC_HW_FE_BUS_ERROR_APB_HANG,
	DC_HW_FE_BUS_ERROR_COUNT,
};

enum dc_hw_be_bus_errors {
	DC_HW_BE_BUS_ERROR_APB_HANG,
	DC_HW_BE_BUS_ERROR_AXI_BUS_ERROR,
	DC_HW_BE_BUS_ERROR_AXI_RD_BUS_HANG,
	DC_HW_BE_BUS_ERROR_AXI_WR_BUS_HANG,
	DC_HW_BE_BUS_ERROR_COUNT,
};

/*****************************************************************
 *
 * TBD !
 * Developesr should modify this item according to
 * actual requirements during developing !
 *
 *****************************************************************/
struct dc_hw_fb {
	u64 address;
	u64 u_address;
	u64 v_address;
	u32 stride;
	u32 u_stride;
	u32 v_stride;
	u16 width;
	u16 height;
	u8 format;
	u8 tile_mode;
	u8 rotation;
	u8 swizzle;
	u8 uv_swizzle;
	u8 zpos;
	u8 display_id;
	bool secure;
	bool enable;
	bool secure_dirty;
	bool dirty;
};

struct dc_hw_sram_pool {
	u32 sp_handle;
	u32 sp_size;
	u8 sp_unit_size;
	u32 scl_sp_handle;
	u32 scl_sp_size;
	bool dirty;
};

struct dc_hw_position {
	struct drm_vs_rect rect[2];
	bool dirty;
	bool enable;
};

struct dc_hw_size {
	u16 width;
	u16 height;
	bool dirty;
	bool enable;
};

/*****************************************************************
 *
 * TBD !
 * Developesr should modify the  following structure definition
 * according to the actual requirements during developing!
 *
 *****************************************************************/
struct dc_hw_std_bld {
	u16 alpha;
	u16 blend_mode;
	bool dirty;
	bool enable;
};

struct dc_hw_display_mode {
	enum dc_hw_out out;
	u32 bus_format;
	u16 h_active;
	u16 h_total;
	u16 h_sync_start;
	u16 h_sync_end;
	u16 v_active;
	u16 v_total;
	u16 v_sync_start;
	u16 v_sync_end;
	bool h_sync_polarity;
	bool v_sync_polarity;
	u32 output_mode;
	bool enable;
	bool is_yuv;
	bool vrr_enable;
	bool dsc_enable;
	u32 te_usec;
	int fps;
};

struct dc_hw_block {
	void *vaddr;
	bool enable;
	bool dirty;
};

struct dc_hw_lut {
	struct drm_vs_xstep_lut lut;
	bool enable;
	bool dirty;
};

struct dc_hw_scale {
	struct drm_vs_preprocess_scale_config coefficients;
	u32 src_w;
	u32 src_h;
	u32 dst_w;
	u32 dst_h;
	u32 factor_x;
	u32 factor_y;
	bool stretch_mode;
	bool coefficients_enable;
	bool enable;
	bool coefficients_dirty;
	bool dirty;
};

struct dc_hw_roi {
	// Written to hardware to configure DMA block
	enum drm_vs_dma_mode mode;
	/* in_rect[0] is available under the DMA mode:
	 *	   VS_DMA_ONE_ROI: the ROI region rectangle.
	 *	   VS_DMA_TWO_ROI: the first ROI region rectangle.
	 *	   VS_DMA_SKIP_ROI: the skip ROI region rectangle.
	 *	   VS_DMA_EXT_LAYER_EX: the ROI region rectangle of first image.
	 * in_rect[1] is available under the DMA mode:
	 *	   VS_DMA_TWO_ROI: the second ROI region rectangle.
	 *	   VS_DMA_EXT_LAYER_EX: the ROI region rectangle of second image.
	 */
	struct drm_vs_rect in_rect[VS_MAX_ROI_CNT];
	/* out_rect[0] is available under the DMA mode:
	 *	   VS_DMA_ONE_ROI: specify the ROI out region.
	 *	   VS_DMA_TWO_ROI: specify the first ROI out region.
	 *	   VS_DMA_SKIP_ROI: specify skip ROI out region.
	 *	   VS_DMA_EXT_LAYER: specify the first image out region.
	 *	   VS_DMA_EXT_LAYER_EX: specify the ROI out region of first image.
	 * out_rect[1] is available under the DMA mode:
	 *	   VS_DMA_TWO_ROI: specify the second ROI out region.
	 *	   VS_DMA_EXT_LAYER: specify the second image out region.
	 *	   VS_DMA_EXT_LAYER_EX: specify the ROI out region of second image.
	 */
	struct drm_vs_rect out_rect[VS_MAX_ROI_CNT];
	bool enable;
	bool dirty;
};

struct dc_hw_clear {
	struct drm_vs_color color;
	bool enable;
	bool dirty;
};

struct dc_hw_rcd_mask {
	u64 address;
	u32 stride;
	u8 display_id;
	enum drm_vs_mask_blend_type type;
	struct drm_vs_rcd_roi roi_data;
	bool enable;
	bool dirty;
};

struct dc_hw_y2r {
	u8 gamut;
	u8 mode;
	s32 coef[VS_MAX_Y2R_COEF_NUM];
	bool dirty;
	bool enable;
};

struct dc_hw_r2y {
	u8 gamut;
	u8 mode;
	s32 coef[VS_MAX_R2Y_COEF_NUM];
	bool dirty;
	bool enable;
};

struct dc_hw_data_extend {
	enum drm_vs_data_extend_mode mode;
	bool dirty;
	bool enable;
};

struct dc_hw_gamma {
	struct drm_vs_gamma_lut lut[3];
	bool enable[3];
	bool dirty;
};

struct dc_hw_display_wb {
	u8 wb_id;
	u32 wb_point;
	bool enable;
	bool dirty;
};

struct dc_hw_pattern {
	struct drm_vs_rect rect;
	u8 mode;
	u8 pos;
	u32 ramp_step;
	u64 color;
	u64 pixel_value; /*read back for curser*/
	bool enable;
};

struct dc_hw_crc {
	bool enable;
	u8 pos;
	struct drm_vs_color seed;
	struct drm_vs_color result;
};

struct dc_hw_disp_crc {
	bool enable;
	u8 pos;
	struct drm_vs_color seed[MAX_CRC_CORE_NUM];
	struct drm_vs_color result[MAX_CRC_CORE_NUM];
};

struct dc_hw_ltm_enable {
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_xgamma {
	struct drm_vs_ltm_xgamma xgamma;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_luma {
	struct drm_vs_ltm_luma ltm_luma;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_freq_decomp {
	struct drm_vs_ltm_freq_decomp freq_decomp;
	u32 norm;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_luma_adj {
	struct drm_vs_1d_lut lut_1d;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_grid {
	struct drm_vs_ltm_grid grid_size;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_af_filter {
	struct drm_vs_ltm_af_filter af_filter;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_af_slice {
	struct drm_vs_ltm_af_slice af_slice;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_af_trans {
	struct drm_vs_ltm_af_trans af_trans;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_tone_adj {
	struct drm_vs_ltm_tone_adj lut_1d;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_color {
	struct drm_vs_ltm_color ltm_color;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_dither {
	struct drm_vs_ltm_dither ltm_dither;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_luma_set {
	struct drm_vs_ltm_luma_ave ltm_luma_set;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_luma_get {
	struct drm_vs_ltm_luma_ave *ltm_luma_get;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_cd_set {
	struct drm_vs_ltm_cd_set ltm_cd_set;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_cd_get {
	struct drm_vs_ltm_cd_get *ltm_cd_get;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_hist_set {
	struct drm_vs_ltm_hist_set ltm_hist_set;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_hist_get {
	struct drm_vs_ltm_hist_get *ltm_hist_get;
	bool enable;
	bool dirty;
};

struct dc_hw_ltm_ds {
	struct drm_vs_ltm_ds ltm_ds;
	bool enable;
	bool dirty;
};

/*
 * histogram engine state
 */
enum dc_hw_hist_state {
	VS_HIST_STATE_DISABLED, /* histogram stage: disabled */
	VS_HIST_STATE_CONFIG, /* histogram state: configuration */
	VS_HIST_STATE_ACTIVE, /* histogram state: active|in-flight|currently processing */
	VS_HIST_STATE_READY, /* histogram stage: data is ready for user */
	VS_HIST_STATE_COUNT,
};

/*
 * histogram buffer stage
 *  ACTIVE: points to next frame histogram data
 *  READY: points to last frame histogram data
 *  USER: points to histogram data accessed by user
 */
enum dc_hw_hist_stage {
	VS_HIST_STAGE_ACTIVE,
	VS_HIST_STAGE_READY,
	VS_HIST_STAGE_USER,
	VS_HIST_STAGE_COUNT
};

/*
 * histogram channel
 */
struct dc_hw_hist_chan {
	/* runtime state */
	struct drm_vs_hist_chan drm_config; /* channel configuration copy */

	/* buffer allocations */
	struct vs_gem_node *gem_node[VS_HIST_STAGE_COUNT];

	/* book keeping */
	bool enable;
	bool dirty;  /* tracks configuration changes (via property change or PSR state change) */
	bool changed; /* tracks property value change */
};

/*
 * histogram rgb
 * Histogram RGB operates using fixed configuration. It can be only enabled or disabled.
 */
struct dc_hw_hist_rgb {
	/* buffer allocations */
	struct vs_gem_node *gem_node[VS_HIST_STAGE_COUNT];

	/* book keeping */
	bool enable;
	bool dirty;
};

struct dc_hw_hdr {
	bool enable;
	bool dirty;
};

struct dc_hw_interrupt_status {
	u16 layer_frm_done;
	u8 display_frm_start;
	u8 display_frm_done;
	u8 display_underrun;
	u8 display_te_rising;
	u8 display_te_falling;
	u8 wb_frm_done;
	u8 wb_datalost;
	u16 pvric_decode_err;
	u16 layer_reset_done;
	u8 reset_status[SW_RESET_NUM];
	DECLARE_BITMAP(fe0_bus_errors, DC_HW_FE_BUS_ERROR_COUNT);
	DECLARE_BITMAP(fe1_bus_errors, DC_HW_FE_BUS_ERROR_COUNT);
	DECLARE_BITMAP(be_bus_errors, DC_HW_BE_BUS_ERROR_COUNT);
};

struct dc_hw_display {
	const struct vs_display_info *info;
	struct dc_hw_display_mode mode;
	struct dc_hw_size bld_size;
	struct dc_hw_data_extend data_ext;
	struct dc_hw_gamma gamma;
	struct dc_hw_display_wb wb;
	struct dc_hw_fb blur_mask;
	struct dc_hw_fb brightness_mask;
	struct dc_hw_ltm_enable ltm_enable;
	struct dc_hw_ltm_xgamma ltm_degamma;
	struct dc_hw_ltm_xgamma ltm_gamma;
	struct dc_hw_ltm_luma ltm_luma;
	struct dc_hw_ltm_freq_decomp freq_decomp;
	struct dc_hw_ltm_luma_adj luma_adj;
	struct dc_hw_ltm_grid grid_size;
	struct dc_hw_ltm_af_filter af_filter;
	struct dc_hw_ltm_af_slice af_slice;
	struct dc_hw_ltm_af_trans af_trans;
	struct dc_hw_ltm_tone_adj tone_adj;
	struct dc_hw_ltm_color ltm_color;
	struct dc_hw_ltm_dither ltm_dither;
	struct dc_hw_ltm_luma_set ltm_luma_set;
	struct dc_hw_ltm_luma_get ltm_luma_get;
	struct dc_hw_ltm_cd_set ltm_cd_set;
	struct dc_hw_ltm_cd_get ltm_cd_get;
	struct dc_hw_ltm_hist_set ltm_hist_set;
	struct dc_hw_ltm_hist_get ltm_hist_get;
	struct dc_hw_ltm_ds ltm_ds;
	struct dc_hw_hist_chan hw_hist_chan[VS_HIST_CHAN_IDX_COUNT];
	struct dc_hw_hist_rgb hw_hist_rgb;
	struct dc_hw_disp_crc crc;
	struct vs_dc_urgent_cmd_config urgent_cmd_config;
	struct vs_dc_urgent_vid_config urgent_vid_config;
	struct vs_dc_property_state_group states;
	struct dc_hw_sram_pool sram_pool;
	struct drm_dsc_config dsc;
	u8 output_id;
	bool sbs_split_dirty;
	bool wb_split_dirty;
	bool config_status;
	u32 vblank_count;
	/* TBD */
};

struct dc_hw_plane {
	const struct vs_plane_info *info;
	struct dc_hw_fb fb;
	struct dc_hw_fb fb_ext;
	struct dc_hw_position pos;
	struct dc_hw_rcd_mask rcd_mask;
	struct dc_hw_std_bld std_bld;
	struct dc_hw_y2r y2r;
	struct dc_hw_block lut_3d;
	struct dc_hw_crc crc;
	struct dc_hw_scale scale;
	struct dc_hw_roi roi;
	struct dc_hw_clear clear;
	struct vs_dc_property_state_group states;
	struct dc_hw_sram_pool sram;
	bool config_status;
};

struct dc_hw_wb {
	const struct vs_wb_info *info;
	struct dc_hw_fb fb;
	struct dc_hw_r2y r2y;
	struct vs_dc_property_state_group states;
	bool config_status;
	/* TBD */
};

struct dc_hw_read {
	u32 reg;
	u32 value;
};

struct dc_hw_dsc_usage {
	bool enable;
	/* Operating mode of the encoder */
	__u8 slices_per_line;
	__u8 ss_num;
	__u16 slice_height;

	/* For usage model */
	bool split_panel_enable;
	bool multiplex_mode_enable;
	int multiplex_out_sel;
	bool de_raster_enable;
	bool multiplex_eoc_enable;
	bool video_mode;
};

struct dc_hw;

/* Used for differential configuration of modules
 * with different chip versions.
 */
struct dc_hw_sub_funcs {
	void (*display_gamma)(struct dc_hw *hw, u8 hw_id, struct dc_hw_gamma *degamma);
	void (*display_wb_pos)(struct dc_hw *hw, u8 hw_id, struct dc_hw_display_wb *wb);
	void (*wb_fb)(struct dc_hw *hw, u8 hw_id, struct dc_hw_fb *fb);
	void (*wb_reg_switch)(struct dc_hw *hw, u8 hw_id, bool enable);
	/* TBD */
};

struct dc_hw_funcs {
	/* TBD !
	 * developer should modify this item according to
	 * actual requirements during developing !
	 */
	void (*set_mode)(struct dc_hw *hw, u8 hw_id, u8 output_id, struct dc_hw_display_mode *mode);
	void (*set_wb)(struct dc_hw *hw, u8 hw_id, struct dc_hw_wb *wb);
	void (*plane)(struct dc_hw *hw, u8 display_id);
	void (*display)(struct dc_hw *hw, u8 display_id);
};

struct dc_hw {
	/* TBD !
	 * developer should modify this item according to
	 * actual requirements during developing !
	 */
	enum dc_chip_rev rev;
	void *reg_base;
	u32 reg_dump_offset;
	u32 reg_dump_size;
	u32 reg_size;
	struct dc_hw_display display[DC_DISPLAY_NUM];
	struct dc_hw_plane plane[DC_PLANE_NUM];
	struct dc_hw_wb wb[DC_WB_NUM];
	struct dc_hw_funcs *func;
	struct dc_hw_sub_funcs *sub_func;
	const struct vs_dc_info *info;
	u8 reset_status[SW_RESET_NUM];
	bool fe0_has_bus_errors;
	bool fe1_has_bus_errors;
	bool be_has_bus_errors;
	u32 link_node_resource[DC_DISPLAY_NUM];
	/*for multiple interrupt destinations*/
	u8 intr_dest;
	struct device *dev;

	bool all_sync_frm_done; /* dp sync */

	DECLARE_BITMAP(secured_layers_mask, HW_PLANE_NUM);

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
	struct regmap_config regmap_config;
	struct regmap *regmap;
#endif

	/* histogram support */
	spinlock_t histogram_slock;

	/* BE interrupt lock*/
	spinlock_t be_irq_slock;

	/* Output mux lock */
	spinlock_t output_mux_slock;
	u32 output_mux_value;
};

void dc_write(struct dc_hw *hw, u32 reg, u32 value);
u32 dc_read(struct dc_hw *hw, u32 reg);
void dc_write_relaxed(struct dc_hw *hw, u32 reg, u32 value);
u32 dc_read_relaxed(struct dc_hw *hw, u32 reg);
#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
void dc_write_immediate(struct dc_hw *hw, u32 reg, u32 value);
u32 dc_read_immediate(struct dc_hw *hw, u32 reg);
#define dc_write_relaxed dc_write
#define dc_read_relaxed dc_read
#else
#define dc_write_immediate dc_write
#define dc_read_immediate dc_read
void dc_write_relaxed(struct dc_hw *hw, u32 reg, u32 value);
u32 dc_read_relaxed(struct dc_hw *hw, u32 reg);
#endif

inline bool vs_dc_display_is_wb(u32 hw_id);
void dc_write_u32_blob(struct dc_hw *hw, u32 reg, const u32 *data, u32 size);
void dc_write_relaxed_u32_blob(struct dc_hw *hw, u32 reg, const u32 *data, u32 size);
int dc_fe0_hw_init(struct dc_hw *hw);
int dc_fe1_hw_init(struct dc_hw *hw);
int dc_be_hw_init(struct dc_hw *hw);
int dc_wb_hw_init(struct dc_hw *hw);
int dc_hw_init(struct dc_hw *hw);
void dc_fe0_hw_deinit(struct dc_hw *hw);
void dc_fe1_hw_deinit(struct dc_hw *hw);
void dc_be_hw_deinit(struct dc_hw *hw);
void dc_wb_hw_deinit(struct dc_hw *hw);
void dc_hw_deinit(struct dc_hw *hw);
void dc_hw_reinit(struct dc_hw *hw);
void dc_hw_update_plane(struct dc_hw *hw, u8 id, struct dc_hw_fb *fb);
void dc_hw_update_plane_rcd_mask(struct dc_hw *hw, u8 id, struct dc_hw_rcd_mask *rcd_mask);
void dc_hw_update_plane_position(struct dc_hw *hw, u8 id, struct dc_hw_position *pos);
void dc_hw_update_ext_fb(struct dc_hw *hw, u8 id, struct dc_hw_fb *fb);
void dc_hw_update_plane_y2r(struct dc_hw *hw, u8 id, struct dc_hw_y2r *y2r_conf);
void dc_hw_update_plane_scale(struct dc_hw *hw, u8 id, struct dc_hw_scale *scale);
void dc_hw_update_plane_roi(struct dc_hw *hw, u8 id, struct dc_hw_roi *roi);
void dc_hw_update_plane_3d_lut(struct dc_hw *hw, u8 id, struct dc_hw_block *lut_3d);
void dc_hw_update_plane_std_bld(struct dc_hw *hw, u8 id, struct dc_hw_std_bld *std_bld);
void dc_hw_update_plane_clear(struct dc_hw *hw, u8 id, struct dc_hw_clear *clear);
void dc_hw_update_plane_sram(struct dc_hw *hw, u8 id, struct dc_hw_sram_pool *sram);

void dc_hw_update_display_bld_size(struct dc_hw *hw, u8 id, bool user_conf,
				   struct dc_hw_size *bld_size);
void dc_hw_update_display_ltm_luma_get(struct dc_hw *hw, u8 id,
				       struct dc_hw_ltm_luma_get *ltm_luma_get);
void dc_hw_update_display_ltm_cd_get(struct dc_hw *hw, u8 id, struct dc_hw_ltm_cd_get *ltm_cd_get);
void dc_hw_update_display_ltm_hist_get(struct dc_hw *hw, u8 id,
				       struct dc_hw_ltm_hist_get *ltm_hist_get);
void dc_hw_update_display_blur_mask(struct dc_hw *hw, u8 id, struct dc_hw_fb *fb);
void dc_hw_update_display_brightness_mask(struct dc_hw *hw, u8 id, struct dc_hw_fb *fb);
void dc_hw_update_display_wb(struct dc_hw *hw, u8 id, struct dc_hw_display_wb *wb);
void dc_hw_setup_display_mode(struct dc_hw *hw, u8 id, struct dc_hw_display_mode *mode);
void dc_hw_update_wb_fb(struct dc_hw *hw, u8 id, struct dc_hw_fb *fb);
void dc_hw_update_r2y(struct dc_hw *hw, u8 id, struct dc_hw_r2y *r2y_conf);
void dc_hw_setup_wb(struct dc_hw *hw, u8 id);
void dc_hw_set_wb_stall(struct dc_hw *hw, bool enable);
void dc_hw_get_crtc_scanout_position(struct dc_hw *hw, u8 display_id, u32 *position);
u32 dc_hw_get_vblank_count(struct dc_hw *hw, u8 id);
void dc_hw_config_plane_status(struct dc_hw *hw, u8 id, bool config);
void dc_hw_config_load_filter(struct dc_hw *hw, u8 hw_id, const u32 *coef_v, const u32 *coef_h);
void dc_hw_config_display_status(struct dc_hw *hw, u8 id, bool config);
void dc_hw_config_wb_status(struct dc_hw *hw, u8 id, bool config);
void dc_hw_enable_frame_irqs(struct dc_hw *hw, u8 id, bool enable);
void dc_hw_enable_vblank_irqs(struct dc_hw *hw, u8 id, bool enable);
int dc_hw_reset_all_be_interrupts(struct dc_hw *hw);
int dc_hw_disable_all_be_interrupts(struct dc_hw *hw);
int dc_hw_clear_be_interrupt_statuses(struct dc_hw *hw);
int dc_hw_clear_be_interrupt_overflows(struct dc_hw *hw);
int dc_hw_get_interrupt(struct dc_hw *hw, struct dc_hw_interrupt_status *status);
int dc_hw_enable_fe_interrupts(struct dc_hw *hw);
u8 dc_hw_get_plane_id(u8 layer, struct dc_hw *hw);
#if IS_ENABLED(CONFIG_DEBUG_FS)
void dc_hw_set_plane_pattern(struct dc_hw *hw, u8 hw_id, struct dc_hw_pattern *pattern);
void dc_hw_set_plane_crc(struct dc_hw *hw, u8 id, struct dc_hw_crc *crc);
void dc_hw_get_plane_crc(struct dc_hw *hw, u8 id, struct dc_hw_crc *crc);
void dc_hw_get_plane_crc_config(struct dc_hw *hw, u8 id, struct dc_hw_crc *crc);
void dc_hw_set_display_pattern(struct dc_hw *hw, u8 hw_id, struct dc_hw_pattern *pattern);
void dc_hw_set_display_crc(struct dc_hw *hw, u8 hw_id, struct dc_hw_disp_crc *crc);
void dc_hw_get_display_crc(struct dc_hw *hw, u8 hw_id, struct dc_hw_disp_crc *crc);
void dc_hw_get_display_crc_config(struct dc_hw *hw, u8 id, struct dc_hw_disp_crc *crc);
int dc_hw_reg_dump(struct dc_hw *hw, struct drm_printer *p, enum dc_hw_reg_bank_type reg_type);
#endif /* CONFIG_DEBUG_FS */
void dc_hw_do_fe0_reset(struct dc_hw *hw);
void dc_hw_do_fe1_reset(struct dc_hw *hw);
void dc_hw_do_be_reset(struct dc_hw *hw);
void dc_hw_do_reset(struct dc_hw *hw);
bool dc_hw_fe_is_all_layers_idle(struct dc_hw *hw);
void dc_hw_enable_shadow_register(struct dc_hw *hw, u8 display_id, bool enable);
void dc_hw_start_trigger(struct dc_hw *hw, u8 display_id, struct drm_crtc *crtc);
void dc_hw_disable_trigger(struct dc_hw *hw, u8 id);
void dc_hw_sw_sof_trigger(struct dc_hw *hw, u8 output_id, bool trig_enable);
int dc_hw_get_ltm_hist(struct dc_hw *hw, u8 output_id, struct drm_vs_ltm_histogram_data *data,
		       dma_addr_t (*vs_crtc_get_ltm_hist_dma_addr)(struct device *, u8));
void online_trigger(struct dc_hw *hw, u8 src_panel, u8 output_id, bool trig_enable);
void link_node_online_trigger(struct dc_hw *hw, u8 src_panel, bool trig_enable, bool wb_enable,
			      u32 wb_point, u32 intf_id);
void link_node_offline_trigger(struct dc_hw *hw, u8 src_panel, bool trig_enable);
void dc_hw_set_output_start(struct dc_hw *hw, u8 output_id, bool trig_enable);
void dc_hw_enable_underrun_interrupt(struct dc_hw *hw, u8 id, bool enable);
void dc_hw_clear_underrun_interrupt(struct dc_hw *hw, u8 id);
void dc_hw_commit(struct dc_hw *hw, u8 display_id);
void dc_hw_plane_commit(struct dc_hw *hw, u8 display_id);
void dc_hw_disable_plane_features(struct dc_hw *hw, u8 display_id);
void dc_hw_display_commit(struct dc_hw *hw, u8 display_id);
void dc_hw_display_frame_done(struct dc_hw *hw, u8 display_id,
			      struct dc_hw_interrupt_status *irq_status);
void dc_hw_display_flip_done(struct dc_hw *hw, u8 display_id);
u32 vs_dc_get_display_offset(u32 hw_id);
u32 vs_dc_get_panel_dither_table_offset(u32 hw_id);
u32 vs_dc_get_wb_offset(u32 hw_id);
const struct dc_hw_plane *vs_dc_hw_get_plane(const struct dc_hw *hw, u32 hw_id);
const struct dc_hw_display *vs_dc_hw_get_display(const struct dc_hw *hw, u32 hw_id);
const int vs_dc_hw_get_display_id(const struct dc_hw *hw, u32 hw_id);
const struct dc_hw_wb *vs_dc_hw_get_wb(const struct dc_hw *hw, u32 hw_id);
const void *vs_dc_hw_get_plane_property(const struct dc_hw *hw, u32 hw_id, const char *prop_name,
					bool *out_enabled);
const void *vs_dc_hw_get_display_property(const struct dc_hw *hw, u32 hw_id, const char *prop_name,
					  bool *out_enabled);
const void *vs_dc_hw_get_wb_property(const struct dc_hw *hw, u32 hw_id, const char *prop_name,
				     bool *out_enabled);

void vs_dpu_query_module_range(u32 moduleid, u32 *reg_start, u32 *reg_end);
void vs_dpu_link_node_config(struct dc_hw *hw, enum vs_dpu_link_node_type type, u32 moduleid,
			     u32 link_node_id, bool enable);
u32 vs_dpu_link_node_config_get(struct dc_hw *hw, u32 link_node_id);

#if IS_ENABLED(CONFIG_PM_SLEEP)
void dc_hw_save_plane_status(struct dc_hw_plane *plane);
void dc_hw_save_display_status(struct dc_hw_display *display);
void dc_hw_save_wb_status(struct dc_hw_wb *wb);
void dc_hw_save_status(struct dc_hw *hw);
#endif

void dc_hw_set_fe_qos(struct dc_hw *hw, u32 value);

#endif /* __VS_DC_HW_H__ */
