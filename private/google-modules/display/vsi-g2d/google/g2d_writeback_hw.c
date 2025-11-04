// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */
#include <linux/types.h>

#include "vs_g2d_reg_sc.h"
#include "g2d_sc_hw.h"
#include "g2d_writeback_hw.h"

void wb_set_fb(struct sc_hw *hw, u8 hw_id, struct sc_hw_fb *fb)
{
	u32 config = 0;

	if (!fb->enable)
		return;

	dev_info(hw->dev, "%s: programming writeback regs for hw_id %d", __func__, hw_id);

	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_CONFIG, FORMAT, fb->format);
	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_CONFIG, SWIZZLE, fb->swizzle);
	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_CONFIG, TILE_MODE, fb->tile_mode);
	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_CONFIG, UV_SWIZZLE, fb->uv_swizzle);
	sc_write(hw, SCREG_LAYER0_WB_CONFIG_Address, config);

	config = 0;
	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_SIZE, WIDTH, fb->width);
	config = VS_SET_FIELD(config, SCREG_LAYER0_WB_SIZE, HEIGHT, fb->height);
	sc_write(hw, SCREG_LAYER0_WB_SIZE_Address, config);
	sc_write(hw, SCREG_LAYER0_WDMA_ADDRESS_Address, (u32)fb->address);
	sc_write(hw, SCREG_LAYER0_WDMA_HADDRESS_Address, (fb->address >> 32) & 0xFF);
	sc_write(hw, SCREG_LAYER0_WDMA_UPLANE_ADDRESS_Address, (u32)fb->u_address);
	sc_write(hw, SCREG_LAYER0_WDMA_UPLANE_HADDRESS_Address, (fb->u_address >> 32) & 0xFF);
	sc_write(hw, SCREG_LAYER0_WDMA_VPLANE_ADDRESS_Address, (u32)fb->v_address);
	sc_write(hw, SCREG_LAYER0_WDMA_VPLANE_HADDRESS_Address, (fb->v_address >> 32) & 0xFF);
	sc_write(hw, SCREG_LAYER0_WDMA_STRIDE_Address, fb->stride);
	sc_write(hw, SCREG_LAYER0_WDMA_UPLANE_STRIDE_Address, fb->u_stride);
	sc_write(hw, SCREG_LAYER0_WDMA_VPLANE_STRIDE_Address, fb->v_stride);
}
