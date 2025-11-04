// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "phy/phy_n621.h"
#include "rst_mng.h"
#include "regmaps/ctrl_fields.h"

#define MAX_WAIT_ITER 20
#define MAX_PHY_BUSY_WAIT_ITER 20

static int phy_n621_reset_phyif_values(struct dptx *dptx)
{
	phyif_write(dptx, PHY_RESET, 0);
	phyif_write(dptx, PHY_ELECIDLE, 0);
	phyif_write(dptx, PHY_TX_DETECT, 0);
	phyif_write(dptx, PHY_MAXPCLKREQ, 0);
	phyif_write(dptx, PHY_DPTX_M2P_MESSAGE_BUS, 0);
	phyif_write(dptx, PHY_DPTX_LANE_RESET_N, 0x3);
	phyif_write(dptx, PHY_DPTX_P2M_MESSAGE_BUS, 0);

	return 0;
}

static struct phy_n621_pll_config phy_n621_pll[] = {
	{ RBR, 0,  0x84, 0xEE, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x20, 0xE5, 0x02, 0x14, 0x46, 0x20},
	{ HBR1, 0,  0xF4, 0xC0, 0, 0, 0, 0, 0, 0, 0x80, 0x01, 0x20, 0xE5, 0x02, 0x0C, 0x46, 0},
	{ HBR2, 0,  0xF4, 0xC0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0x20, 0xE5, 0x01, 0x0C, 0x46, 0},
	{ HBR3, 0,  0x34, 0x30, 0x01, 0, 0, 0, 0, 0, 0xC0, 0, 0xE0, 0xE4, 0, 0x10, 0x46, 0},
	{ EDP0, 0,  0x04, 0x48, 0x01, 0, 0, 0, 0, 0, 0, 0x02, 0xE0, 0xE4, 0x02, 0x10, 0xA0, 0},
	{ EDP1, 0,  0x34, 0x74, 0x01, 0, 0, 0, 0, 0, 0x80, 0x02, 0x60, 0xE5, 0x02, 0x0E, 0x8A, 0},
	{ EDP2, 0,  0x84, 0xEE, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x20, 0xE5, 0x01, 0x14, 0x46, 0x20},
	{ EDP3, 0,  0x04, 0x48, 0x01, 0, 0, 0, 0, 0, 0, 0x01, 0xE0, 0xE4, 0x01, 0x10, 0xA0, 0},
	{ RBR, 0x01,  0xA5, 0xEE, 0x50, 0xB8, 0x09, 0x05, 0xD1, 0, 0, 0x02, 0x20, 0xE5, 0x02, 0x14, 0x46, 0x20},
	{ HBR1, 0x01,  0xF5, 0xC0, 0xA0, 0x19, 0x08, 0x2F, 0xAE, 0, 0x80, 0x01, 0x20, 0xE5, 0x02, 0x0C, 0x46, 0},
	{ HBR2, 0x01,  0xF5, 0xC0, 0xA0, 0x19, 0x08, 0x2F, 0xAE, 0, 0x80, 0, 0x20, 0xE5, 0x01, 0x0C, 0x46, 0},
	{ HBR3, 0x01,  0x35, 0x30, 0x61, 0x26, 0x0C, 0x46, 0x05, 0x01, 0xC0, 0, 0xE0, 0xE4, 0, 0x10, 0x46, 0},
	{ EDP0, 0x01,  0x25, 0x48, 0xC1, 0xF5, 0x0C, 0xB1, 0x16, 0x01, 0, 0x02, 0xE0, 0xE4, 0x02, 0x10, 0xA0, 0},
	{ EDP1, 0x01,  0x35, 0x74, 0x81, 0x94, 0x0E, 0x87, 0x39, 0x01, 0x80, 0x02, 0x60, 0xE5, 0x02, 0x0E, 0x8A, 0},
	{ EDP2, 0x01,  0xA5, 0xEE, 0x50, 0xB8, 0x09, 0x05, 0xD1, 0, 0, 0x01, 0x20, 0xE5, 0x01, 0x14, 0x46, 0x20},
	{ EDP3, 0x01,  0x25, 0x48, 0xC1, 0xF5, 0x0C, 0xB1, 0x16, 0x01, 0, 0x01, 0xE0, 0xE4, 0x01, 0x10, 0xA0, 0},
};

static struct phy_n621_pll_config *phy_n621_get_ppl_config(struct dptx *dptx, u8 rate, u8 ssc_en)
{
	int i = 0;

	// There are 2 configs for each rate, depending on SSC_EN state
	for (i = 0; phy_n621_pll[i].rate < (MAX_RATE*2); i++) {
		if (phy_n621_pll[i].rate == rate && phy_n621_pll[i].ssc_en == ssc_en)
			return &(phy_n621_pll[i]);
	}

	return NULL;
}

void phy_n621_configure_pll(struct dptx *dptx, u8 rate, u8 ssc_en)
{
	struct phy_n621_pll_config *config = NULL;

	config = phy_n621_get_ppl_config(dptx, rate, ssc_en);
	if (config == NULL) {
		dptx_err(dptx, "%s - Configuration not found", __func__);
		return;
	}

	//C00
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL0, config->c00);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL0, config->c00);

	//C02
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL2, config->c02);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL2, config->c02);

	//C03
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL3, config->c03);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL3, config->c03);

	//C04
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL4, config->c04);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL4, config->c04);

	//C05
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL5, config->c05);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL5, config->c05);

	//C06
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL6, config->c06);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL6, config->c06);

	//C07
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL7, config->c07);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL7, config->c07);

	//C08
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL8, config->c08);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL8, config->c08);

	//C0C
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL12, config->c0c);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL12, config->c0c);

	//C0F
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL15, config->c0f);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL15, config->c0f);

	//C11
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL17, config->c11);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL17, config->c11);

	//C12
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL18, config->c12);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL18, config->c12);

	//C57
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX7, config->c57);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX7, config->c57);

	//C59
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX9, config->c59);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX9, config->c59);

	//C5C
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX12, config->c5c);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX12, config->c5c);

	//C5F
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX15, config->c5f);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX15, config->c5f);

	//C01
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL1, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL1, 0);
	//C09
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL9, 0x1);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL9, 0x1);
	//C0A
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL10, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL10, 0);
	//C0B
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL11, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL11, 0);
	//C0D
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL13, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL13, 0);
	//C0E
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL14, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL14, 0);
	//C10
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL16, 0x84);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL16, 0x84);
	//C13
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_PLL19, 0x2F);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_PLL19, 0x2F);
	//C20
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_CMN0, 0);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_CMN0, 0);
	//C21
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_CMN1, 0x70);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_CMN1, 0x70);
	//C22
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_CMN2, 0x44);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_CMN2, 0x44);
	//C23
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_CMN3, 0x16);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_CMN3, 0x16);
	//C24
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_CMN4, 0x1D);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_CMN4, 0x1D);
	//C30
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_TX0, 0x1F);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_TX0, 0x1F);
	//C31
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_TX1, 0x20);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_TX1, 0x20);
	//C3B
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_TX11, 0x08);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_TX11, 0x08);
	//C50
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX0, 0x03);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX0, 0x03);
	//C5D
	phy_messagebus_cmit_write(dptx, LANE_0, PHY_C10_VDR_RX13, 0x05);
	phy_messagebus_cmit_write(dptx, LANE_1, PHY_C10_VDR_RX13, 0x05);
}

int phy_n621_power_up(struct dptx *dptx)
{
	u32 tx_ready, ack_lane0, ack_lane1;
	u32 phystatus_lane0, phystatus_lane1, phy_busy;
	u8 count;
	struct ctrl_regfields *ctrl_fields;

	ctrl_fields = dptx->ctrl_fields;
	tx_ready = 0;
	ack_lane0 = ack_lane1 = 0;
	phystatus_lane0 = phystatus_lane1 = 0;
	count = 0;

	dptx_dbg(dptx, "PHY N621: Power Up - START\n");

	// Reset PHYIF reg values
	phy_n621_reset_phyif_values(dptx);

	// a. Set the initial input values
	dptx_dbg(dptx, "Power Up - a. Set the initial input values\n");
	phyif_write_mask(dptx, PHY_RESET, PHY_RESET_MASK, 0x1);
	phyif_write_mask(dptx, PHY_DPTX_LANE_RESET_N, PHY_DPTX_LANE_RESET_ALL_MASK, 0x0);
	dptx_write_regfield(dptx, ctrl_fields->field_phy_powerdown, 0b11);
	dptx_write_regfield(dptx, ctrl_fields->field_phy_soft_reset, 0x1);
	dptx_write_regfield(dptx, ctrl_fields->field_xmit_enable, 0x0);

	// b. De-assert phy_reset:
	dptx_dbg(dptx, "Power Up - b. De-assert phy_reset\n");
	dptx_write_regfield(dptx, ctrl_fields->field_phy_soft_reset, 0x0);
	phyif_write_mask(dptx, PHY_RESET, PHY_RESET_MASK, 0x0);

	// c. Wait for tx_ready == 1’b1
	dptx_dbg(dptx, "Power Up - c. Wait for tx_ready == 1’b1\n");
	tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	while (tx_ready != 1) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - TX_READY", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	};

	dptx_dbg(dptx, "Power Up - d. Generate a jtag reset\n");
	// d. Generate a jtag reset
	phy_jtag_master_trst(dptx, 0x1);
	phy_jtag_master_trst(dptx, 0x0);

	dptx_dbg(dptx, "Power Up - e. Enable the jtag to messagebus bridge\n");
	// e. Enable the jtag to messagebus bridge
	phy_jtag_write(dptx, JTAG_FPGA, 0x35, 0x1);
	phy_jtag_write(dptx, JTAG_IP, 0x2038, 0xE);

	dptx_dbg(dptx, "Power Up - f. Set pipe_laneM_maxpclk_req = 2\n");
	// f. Set pipe_laneM_maxpclk_req = 2
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE0_MASK, 0x2);
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE1_MASK, 0x2);

	// g. Wait for pipe_laneM_maxpclk_ack == 2
	dptx_dbg(dptx, "Power Up - g. Wait for pipe_laneM_maxpclk_ack == 2\n");
	ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
	ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	count = 0;
	while (ack_lane0 != 2 && ack_lane1 != 2) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - MAX CLK ACK", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
		ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	}

	// h. De-assert lane resets
	dptx_dbg(dptx, "Power Up - h. De-assert lane resets\n");
	phyif_write_mask(dptx, PHY_DPTX_LANE_RESET_N, PHY_DPTX_LANE_RESET_ALL_MASK, 0x3);

	// i. Wait for pipe_laneM_phystatus == 1’b0
	dptx_dbg(dptx, "Power Up - i. Wait for pipe_laneM_phystatus == 1’b0\n");
	phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	count = 0;
	while (phy_busy) {
		count++;
		if (count > MAX_PHY_BUSY_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - PHY BUSY", __func__);
			return -EAGAIN;
		}
		msleep(20);
		phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	}

	// j. Wait for tx_ready == 1’b1
	dptx_dbg(dptx, "Power Up - k. Wait for tx_ready == 1’b1\n");
	count = 0;
	tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	while (tx_ready != 1) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - TX_READY", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	}

	// k. Go to P2 pstate
	dptx_dbg(dptx, "Power Up - l. Go to P2 pstate (phy_powerdown)\n");
	dptx_write_regfield(dptx, ctrl_fields->field_phy_powerdown, 0b10);

	// l. Wait for a 0->1->0 pulse on pipe_laneM_phystatus
	dptx_dbg(dptx, "Power Up - m. Wait for a 0->1->0 pulse on pipe_laneM_phystatus\n");
	phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	count = 0;
	while (phy_busy) {
		count++;
		if (count > MAX_PHY_BUSY_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - PHY BUSY", __func__);
			return -EAGAIN;
		}
		msleep(20);
		phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	}

	// m. Configure PLL
	dptx_dbg(dptx, "Power Up - n. Configure PLL\n");
	phy_n621_configure_pll(dptx, dptx->link.rate, dptx->ssc_en);

	// n. LM messagebus write: addr: 0xd00, data: 0x40
	phy_messagebus_cmit_write(dptx, LANE_0, 0xD00, 0x40);
	phy_messagebus_cmit_write(dptx, LANE_1, 0xD00, 0x40);

	// o. L1 messagebus write: addr: 0xc70, data: 0x05
	phy_messagebus_cmit_write(dptx, LANE_1, 0xC70, 0x05);

	// p. L0 messagebus write: addr: 0xc70, data: 0x07
	phy_messagebus_cmit_write(dptx, LANE_0, 0xC70, 0x07);

	// q. Wait for tx_ready == 1’b1
	dptx_dbg(dptx, "Power Up - q. Wait for tx_ready == 1’b1\n");
	count = 0;
	tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	while (tx_ready != 1) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - TX_READY", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	}

	// r. Set pipe_laneM_maxpclk_req = 3
	dptx_dbg(dptx, "Power Up - r. Set pipe_laneM_maxpclk_req = 3\n");
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE0_MASK, 0x3);
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE1_MASK, 0x3);

	// s. Wait for pipe_laneM_maxpclk_ack == 3
	dptx_dbg(dptx, "Power Up - s. Wait for pipe_laneM_maxpclk_ack == 3\n");
	ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
	ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	count = 0;
	while (ack_lane0 != 3 && ack_lane1 != 3) {
		count++;
		if (count > MAX_PHY_BUSY_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - MAX CLK ACK", __func__);
			return -EAGAIN;
		}
		msleep(20);
		ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
		ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	}

	dptx_dbg(dptx, "PHY N621: Power Up - END\n");

	return 0;
}

int phy_n621_lane_power_up(struct dptx *dptx)
{
	u32 phy_busy, tx_ready;
	u8 count;
	struct ctrl_regfields *ctrl_fields;

	ctrl_fields = dptx->ctrl_fields;

	dptx_dbg(dptx, "PHY N621: Lane Power Up\n");

	dptx_write_regfield(dptx, ctrl_fields->field_phy_powerdown, 0b00);
	phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	count = 0;

	while (phy_busy) {
		count++;
		if (count > MAX_PHY_BUSY_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - PHY BUSY", __func__);
			return -EAGAIN;
		}
		msleep(20);
		phy_busy = dptx_read_regfield(dptx, ctrl_fields->field_phy_busy);
	}

	count = 0;
	tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	while (tx_ready != 1) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - TX_READY", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	}

	tx_ready = phyif_read_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK);
	phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK, 0);

	return 0;
}

void phy_n621_data_transmission(struct dptx *dptx, bool state)
{
	u8 lanes;
	u32 elecidle;

	lanes = dptx->link.lanes;

	if (state) {
		/*switch(lanes) {
		case 4:
			phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANE1_MASK, 0);
			fallthrough;
		case 2:
		case 1:
			phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANE0_MASK, 0);
			break;
		}*/
		phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK, 0x0);
	} else
		phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK, 0x3);	// Deactivate all lanes

	elecidle = phyif_read_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK);
	dptx_dbg(dptx, "%s: ELECIDLE - 0x%X\n", __func__, elecidle);

}

static int validate_txffe_combo(u8 margin, u8 deemph)
{
	int retval;

	retval = 0;

	switch (margin) {
	case 0:
		if (deemph > 3)
			retval = -EINVAL;
		break;
	case 1:
		if (deemph > 2)
			retval = -EINVAL;
		break;
	case 2:
		if (deemph > 1)
			retval = -EINVAL;
		break;
	case 3:
		if (deemph != 0)
			retval = -EINVAL;
		break;
	}

	return retval;
}

int phy_n621_txffe_config(struct dptx *dptx)
{
	u8 i;
	u8 margin, deemph;

	for (i = 0; i < dptx->link.lanes; i++) {

		margin = dptx->link.vswing_level[i];
		deemph = dptx->link.preemp_level[i];
		if (validate_txffe_combo(margin, deemph) != 0)
			return -EINVAL;

		switch (i) {
		case (0):
			phy_messagebus_cmit_write(dptx, LANE_0, 0x402, deemph);
			phy_messagebus_cmit_write(dptx, LANE_0, 0x408, margin);
			break;
		case (1):
			phy_messagebus_cmit_write(dptx, LANE_0, 0x602, deemph);
			phy_messagebus_cmit_write(dptx, LANE_0, 0x608, margin);
			break;
		case (2):
			phy_messagebus_cmit_write(dptx, LANE_1, 0x602, deemph);
			phy_messagebus_cmit_write(dptx, LANE_1, 0x608, margin);
			break;
		case (3):
			phy_messagebus_cmit_write(dptx, LANE_1, 0x402, deemph);
			phy_messagebus_cmit_write(dptx, LANE_1, 0x408, margin);
			break;
		}
	}

	//Read TxFFE configuration made
	margin = phy_messagebus_read(dptx, LANE_0, 0x408);
	deemph = phy_messagebus_read(dptx, LANE_0, 0x402);
	dptx_dbg(dptx, "%s - LANE 0: margin - %u deemph - %u", __func__, margin, deemph);
	margin = phy_messagebus_read(dptx, LANE_0, 0x608);
	deemph = phy_messagebus_read(dptx, LANE_0, 0x602);
	dptx_dbg(dptx, "%s - LANE 1: margin - %u deemph - %u", __func__, margin, deemph);
	margin = phy_messagebus_read(dptx, LANE_1, 0x608);
	deemph = phy_messagebus_read(dptx, LANE_1, 0x602);
	dptx_dbg(dptx, "%s - LANE 2: margin - %u deemph - %u", __func__, margin, deemph);
	margin = phy_messagebus_read(dptx, LANE_1, 0x408);
	deemph = phy_messagebus_read(dptx, LANE_1, 0x402);
	dptx_dbg(dptx, "%s - LANE 3: margin - %u deemph - %u", __func__, margin, deemph);

	return 0;
}

int phy_n621_set_rate(struct dptx *dptx, u8 rate)
{
	u8 count;
	u32 tx_ready, ack_lane0, ack_lane1;

	dptx_dbg(dptx, "%s - Rate: %d", __func__, rate);

	//phyif_write_mask(dptx, PHY_ELECIDLE, PHY_DPTX_ELECIDLE_LANES_MASK, 0x3);

	// f. Set pipe_laneM_maxpclk_req = 2
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE0_MASK, 0x2);
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE1_MASK, 0x2);

	// g. Wait for pipe_laneM_maxpclk_ack == 2
	ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
	ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	count = 0;
	while (ack_lane0 != 2 && ack_lane1 != 2) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - MAX CLK ACK", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
		ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	}

	// m. Configure PLL
	phy_n621_configure_pll(dptx, rate, dptx->ssc_en);

	// q. Wait for tx_ready == 1’b1
	count = 0;
	tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	while (tx_ready != 1) {
		count++;
		if (count > MAX_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - TX_READY", __func__);
			return -EAGAIN;
		}
		fsleep(10);
		tx_ready = phyif_read_mask(dptx, PHY_TX_READY, PHY_TX_READY_MASK);
	}

	// r. Set pipe_laneM_maxpclk_req = 3
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE0_MASK, 0x3);
	phyif_write_mask(dptx, PHY_MAXPCLKREQ, PHY_MAXPCLK_LANE1_MASK, 0x3);

	// s. Wait for pipe_laneM_maxpclk_ack == 3
	ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
	ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	count = 0;
	while (ack_lane0 != 3 && ack_lane1 != 3) {
		count++;
		if (count > MAX_PHY_BUSY_WAIT_ITER) {
			dptx_err(dptx, "%s: TIMEOUT - MAX CLK ACK", __func__);
			return -EAGAIN;
		}
		msleep(20);
		ack_lane0 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE0_MASK);
		ack_lane1 = phyif_read_mask(dptx, PHY_MAXPCLKACK, PHY_MAXPCLK_LANE1_MASK);
	}

	return 0;
}


/***************
 *   DEBUGFS   *
 ***************/

static int phy_powerup_show(struct seq_file *s, void *unused)
{
	return 0;
}

static int phy_powerup_open(struct inode *inode, struct file *file)
{
	return single_open(file, phy_powerup_show, inode->i_private);
}

static ssize_t phy_powerup_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	int retval = 0;
	char buf[5];

	struct seq_file *s = file->private_data;
	struct dptx *dptx = s->private;

	mutex_lock(&dptx->mutex);
	memset(buf, 0, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count))) {
		retval = -EFAULT;
		goto done;
	}

	phy_n621_power_up(dptx);
	retval = count;
done:
	mutex_unlock(&dptx->mutex);
	return retval;
}

const struct file_operations phy_powerup_fops = {
	.open	= phy_powerup_open,
	.write	= phy_powerup_write,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
