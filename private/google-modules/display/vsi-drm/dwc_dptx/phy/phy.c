// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "phy/phy.h"

void phy_tc_write(struct dptx *dptx, u8 slave, u16 addr, u16 data)
{
	u8 aux = 0;

	if (slave <= JTAG_IP) {
		phy_jtag_write(dptx, slave, addr, data);
	} else {
		/* Using bridge inside JTAG_FPGA to access TC */
		phy_jtag_write(dptx, JTAG_FPGA, 0x21, addr);	//FPGA_JTAG_ADDR
		phy_jtag_write(dptx, JTAG_FPGA, 0x22, data);	//FPGA_JTAG_DATAWR
		aux = set8(aux, JTAG_TC_WR_EN_MASK, 1);
		phy_jtag_write(dptx, JTAG_FPGA, 0x23, aux);	//FPGA_JTAG_WRITE_EN
		aux = set8(aux, JTAG_TC_WR_EN_MASK, 0);
		phy_jtag_write(dptx, JTAG_FPGA, 0x23, aux);	//Clear WRITE_EN
	}
}

u16 phy_tc_read(struct dptx *dptx, u8 slave, u16 addr)
{
	u8 aux = 0;

	if (slave <= JTAG_IP) {
		return phy_jtag_read(dptx, slave, addr);
	} else {
		/* Using bridge inside JTAG_FPGA to access TC */
		phy_jtag_write(dptx, JTAG_FPGA, 0x21, addr);	//FPGA_JTAG_ADDR
		aux = set8(aux, JTAG_TC_RD_EN_MASK, 1);
		phy_jtag_write(dptx, JTAG_FPGA, 0x23, aux);	//FPGA_JTAG_READ_EN
		aux = set8(aux, JTAG_TC_RD_EN_MASK, 0);
		phy_jtag_write(dptx, JTAG_FPGA, 0x23, aux);	//Clear READ_EN
		return phy_jtag_read(dptx, JTAG_FPGA, 0x20);	//Get FPGA_DATARD
	}
}

void phy_messagebus_uncmit_write(struct dptx *dptx, u8 lane, u16 addr, u8 data)
{
	u16 lm_wr_u;

	lm_wr_u = 0;

	phy_jtag_write(dptx, JTAG_FPGA, LM_ADDR(lane), addr);
	phy_jtag_write(dptx, JTAG_FPGA, LM_WR_DATA(lane), (u16)(data));

	lm_wr_u = set16(lm_wr_u, LM_WR_U_MASK(lane), 1);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_wr_u);
	lm_wr_u = set16(lm_wr_u, LM_WR_U_MASK(lane), 0);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_wr_u);
}

void phy_messagebus_cmit_write(struct dptx *dptx, u8 lane, u16 addr, u8 data)
{
	u16 lm_wr_c;

	lm_wr_c = 0;

	phy_jtag_write(dptx, JTAG_FPGA, LM_ADDR(lane), addr);
	phy_jtag_write(dptx, JTAG_FPGA, LM_WR_DATA(lane), (u16)(data));

	lm_wr_c = set16(lm_wr_c, LM_WR_C_MASK(lane), 1);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_wr_c);
	lm_wr_c = set16(lm_wr_c, LM_WR_C_MASK(lane), 0);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_wr_c);
}

u8 phy_messagebus_read(struct dptx *dptx, u8 lane, u16 addr)
{
	u16 lm_rd;
	u16 read;

	lm_rd = 0;

	if (lane > 1) {
		dptx_err(dptx, "%s: Lane defined not allowed", __func__);
		return -EINVAL;
	}

	phy_jtag_write(dptx, JTAG_FPGA, LM_ADDR(lane), addr);
	lm_rd = set16(lm_rd, LM_RD_MASK(lane), 1);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_rd);
	lm_rd = set16(lm_rd, LM_RD_MASK(lane), 0);
	phy_jtag_write(dptx, JTAG_FPGA, 0x0034, lm_rd);
	read = phy_jtag_read(dptx, JTAG_FPGA, 0x0004);

	return (u8)(read >> (8 * lane));
}
