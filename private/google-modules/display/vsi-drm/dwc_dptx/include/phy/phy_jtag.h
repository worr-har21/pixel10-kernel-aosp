/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "dptx.h"
#include "dptx_utils.h"

/*****************************************************************************
 *                                                                           *
 *                        JTAG PHY Interface Registers                       *
 *                                                                           *
 *****************************************************************************/

//PHY JTAG Master Device ID register
#define PHY_JTAG_MASTER_DEVICE_ID		0x0000002C
#define PHY_JTAG_MASTER_DEVICE_ID_MASK		0x0000000F

//PHY JTAG Master Address register
#define PHY_JTAG_MASTER_ADDR			0x00000024
#define PHY_JTAG_MASTER_ADDR_MASK		0x000000FF

//PHY JTAG Master Start register
#define PHY_JTAG_MASTER_START			0x00000030
#define PHY_JTAG_MASTER_START_IR_MASK		0x00000001
#define PHY_JTAG_MASTER_START_DR_MASK		0x00000002
#define PHY_JTAG_MASTER_START_TRST_MASK		0x00000004

//PHY JTAG Master Command register
#define PHY_JTAG_MASTER_CMD			0x00000028
#define PHY_JTAG_MASTER_CMD_MASK		0x00000003

//PHY JTAG Master Date Write register
#define PHY_JTAG_MASTER_DATAWR			0x00000034
#define PHY_JTAG_MASTER_DATAWR_MASK		0x0000FFFF

//PHY JTAG Master Data Read register
#define PHY_JTAG_MASTER_DATARD			0x00000038
#define PHY_JTAG_MASTER_DATARD_MASK		0x0000FFFF

//PHY JTAG Master Status register
#define PHY_JTAG_MASTER_STATUS			0x0000003C
#define PHY_JTAG_MASTER_STATUS_DONE_MASK	0x00000001

enum phy_jtag_slaves_enum {
	JTAG_NONE = 0,
	JTAG_FPGA,
	JTAG_IP,
	JTAG_TC,
	JTAG_MAX
};

#define JTAG_TAP_ADDR_CMD	0
#define JTAG_TAP_WRITE_CMD	1
#define JTAG_TAP_DATA_CMD	2
#define JTAG_TAP_READ_CMD	3

u32 phy_jtag_get_selected(struct dptx *dptx);
void phy_jtag_master_dev_id(struct dptx *dptx, u32 val);
void phy_jtag_master_ir_addr(struct dptx *dptx, u32 val);
void phy_jtag_master_trst(struct dptx *dptx, u32 val);
void phy_jtag_master_addr_start(struct dptx *dptx, u32 val);
void phy_jtag_master_data_start(struct dptx *dptx, u32 val);
void phy_jtag_master_cmd(struct dptx *dptx, u32 val);
void phy_jtag_master_data_write(struct dptx *dptx, u16 val);
u16 phy_jtag_master_data_read(struct dptx *dptx);
u16 phy_jtag_master_get_done(struct dptx *dptx);
bool phy_jtag_master_check_done(struct dptx *dptx);
void phy_jtag_master_send_ir(struct dptx *dptx, u8 jtag_addr);
void phy_jtag_master_send_dr(struct dptx *dptx, u8 cmd, u16 data);
void phy_jtag_slave_select(struct dptx *dptx, u8 desired, u8 jtag_addr);
void phy_jtag_init(struct dptx *dptx);
u16 phy_jtag_read(struct dptx *dptx, u8 slave, u16 addr);
void phy_jtag_write(struct dptx *dptx, u8 slave, u16 addr, u16 value);

int phy_dbfs_jtag_show(struct seq_file *s, void *unused);
int phy_dbfs_jtag_open(struct inode *inode, struct file *file);
ssize_t phy_dbfs_jtag_write(struct file *file, const char __user *ubuf,
					size_t count, loff_t *ppos);

static const struct file_operations phy_jtag_write_fops = {
	.open	= phy_dbfs_jtag_open,
	.write	= phy_dbfs_jtag_write,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
