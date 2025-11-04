/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "dptx.h"
#include "dptx_utils.h"
#include "phy/phy_jtag.h"

#define JTAG_TC_WR_EN_MASK		0x0001
#define JTAG_TC_RD_EN_MASK		0x0002

#define LANE_0                          0
#define LANE_1                          1
#define LM_ADDR(lane)                   (0x0030 + 0x0002 * lane)
#define LM_WR_DATA(lane)                (0x0031 + 0x0002 * lane)
#define LM_WR_U_MASK(lane)              (1 << 8 * lane)
#define LM_WR_C_MASK(lane)              (1 << (1 + 8 * lane))
#define LM_RD_MASK(lane)                (1 << (2 + 8 * lane))


void phy_tc_write(struct dptx *dptx, u8 slave, u16 addr, u16 data);
u16 phy_tc_read(struct dptx *dptx, u8 slave, u16 addr);
void phy_messagebus_uncmit_write(struct dptx *dptx, u8 lane, u16 addr, u8 data);
void phy_messagebus_cmit_write(struct dptx *dptx, u8 lane, u16 addr, u8 data);
u8 phy_messagebus_read(struct dptx *dptx, u8 lane, u16 addr);
