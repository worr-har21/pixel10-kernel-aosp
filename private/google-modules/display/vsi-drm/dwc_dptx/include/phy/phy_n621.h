/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "dptx.h"
#include "phy/phy.h"

/*****************************************************************************
 *                                                                           *
 *                   PHY Interface Configuration Registers                   *
 *                                                                           *
 *****************************************************************************/

#define PHY_TX_READY			0x0000
#define PHY_RESET			0x0004
#define PHY_ELECIDLE			0x0008
#define PHY_TX_DETECT			0x000C
#define PHY_MAXPCLKREQ			0x0010
#define PHY_MAXPCLKACK			0x0014
#define PHY_DPTX_M2P_MESSAGE_BUS	0x0018
#define PHY_DPTX_STATUS			0x001C
#define PHY_DPTX_LANE_RESET_N		0x0020
#define PHY_DPTX_P2M_MESSAGE_BUS	0x0040

#define PHY_TX_READY_MASK		0x0001
#define PHY_RESET_MASK			0x0001
#define PHY_DPTX_LANE_RESET_ALL_MASK	0x0003
#define PHY_MAXPCLK_LANE0_MASK		0x0003
#define PHY_MAXPCLK_LANE1_MASK		0x000C
#define PHY_DPTX_ELECIDLE_LANE0_MASK	0x0001
#define PHY_DPTX_ELECIDLE_LANE1_MASK	0x0002
#define PHY_DPTX_ELECIDLE_LANES_MASK	0x0003

// UPCSLANE_PIPE_LPC_PHY_C10_VDR_PPL
#define PHY_C10_VDR_PLL0	0xC00
#define PHY_C10_VDR_PLL1	0xC01
#define PHY_C10_VDR_PLL2	0xC02
#define PHY_C10_VDR_PLL3	0xC03
#define PHY_C10_VDR_PLL4	0xC04
#define PHY_C10_VDR_PLL5	0xC05
#define PHY_C10_VDR_PLL6	0xC06
#define PHY_C10_VDR_PLL7	0xC07
#define PHY_C10_VDR_PLL8	0xC08
#define PHY_C10_VDR_PLL9	0xC09
#define PHY_C10_VDR_PLL10	0xC0A
#define PHY_C10_VDR_PLL11	0xC0B
#define PHY_C10_VDR_PLL12	0xC0C
#define PHY_C10_VDR_PLL13	0xC0D
#define PHY_C10_VDR_PLL14	0xC0E
#define PHY_C10_VDR_PLL15	0xC0F
#define PHY_C10_VDR_PLL16	0xC10
#define PHY_C10_VDR_PLL17	0xC11
#define PHY_C10_VDR_PLL18	0xC12
#define PHY_C10_VDR_PLL19	0xC13

// UPCSLANE_PIPE_LPC_PHY_C10_VDR_CMN
#define PHY_C10_VDR_CMN0	0xC20
#define PHY_C10_VDR_CMN1	0xC21
#define PHY_C10_VDR_CMN2	0xC22
#define PHY_C10_VDR_CMN3	0xC23
#define PHY_C10_VDR_CMN4	0xC24

// UPCSLANE_PIPE_LPC_PHY_C10_VDR_TX
#define PHY_C10_VDR_TX0		0xC30
#define PHY_C10_VDR_TX1		0xC31
#define PHY_C10_VDR_TX11	0xC3B

// PCSLANE_PIPE_LPC_PHY_C10_VDR_RX
#define PHY_C10_VDR_RX0		0xC50
#define PHY_C10_VDR_RX7		0xC57
#define PHY_C10_VDR_RX9		0xC59
#define PHY_C10_VDR_RX12	0xC5C
#define PHY_C10_VDR_RX13	0xC5D
#define PHY_C10_VDR_RX15	0xC5F

enum rate_enum {
	RBR = 0,
	HBR1,
	HBR2,
	HBR3,
	EDP0,
	EDP1,
	EDP2,
	EDP3,
	MAX_RATE
};

struct phy_n621_pll_config {
	u8	rate;
	u8	ssc_en;
	u8	c00;
	u8	c02;
	u8	c03;
	u8	c04;
	u8	c05;
	u8	c06;
	u8	c07;
	u8	c08;
	u8	c0c;
	u8	c0f;
	u8	c11;
	u8	c12;
	u8	c57;
	u8	c59;
	u8	c5c;
	u8	c5f;
};

void phy_n621_configure_pll(struct dptx *dptx, u8 rate, u8 ssc_en);
int phy_n621_power_up(struct dptx *dptx);
int phy_n621_lane_power_up(struct dptx *dptx);
void phy_n621_data_transmission(struct dptx *dptx, bool state);
int phy_n621_txffe_config(struct dptx *dptx);
int phy_n621_set_rate(struct dptx *dptx, u8 rate);
