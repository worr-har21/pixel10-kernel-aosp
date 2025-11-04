// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include "phy/phy_jtag.h"

u8 slave_crsel[JTAG_MAX] = {0x0, 0x31, 0x31};

u32 phy_jtag_get_selected(struct dptx *dptx)
{
	return phyif_read_mask(dptx, PHY_JTAG_MASTER_DEVICE_ID,
			       PHY_JTAG_MASTER_DEVICE_ID_MASK);
}

void phy_jtag_master_dev_id(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_DEVICE_ID,
			 PHY_JTAG_MASTER_DEVICE_ID_MASK, val);
}

void phy_jtag_master_ir_addr(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_ADDR,
			 PHY_JTAG_MASTER_ADDR_MASK, val);
}

void phy_jtag_master_trst(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_START,
			 PHY_JTAG_MASTER_START_TRST_MASK, val);
}

void phy_jtag_master_addr_start(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_START,
			 PHY_JTAG_MASTER_START_IR_MASK, val);
}

void phy_jtag_master_data_start(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_START,
			 PHY_JTAG_MASTER_START_DR_MASK, val);
}

void phy_jtag_master_cmd(struct dptx *dptx, u32 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_CMD,
			 PHY_JTAG_MASTER_CMD_MASK, val);
}

void phy_jtag_master_data_write(struct dptx *dptx, u16 val)
{
	phyif_write_mask(dptx, PHY_JTAG_MASTER_DATAWR,
			 PHY_JTAG_MASTER_DATAWR_MASK, val);
}

u16 phy_jtag_master_data_read(struct dptx *dptx)
{
	return phyif_read_mask(dptx, PHY_JTAG_MASTER_DATARD,
			       PHY_JTAG_MASTER_DATARD_MASK);
}

u16 phy_jtag_master_get_done(struct dptx *dptx)
{
	return phyif_read_mask(dptx, PHY_JTAG_MASTER_STATUS,
			       PHY_JTAG_MASTER_STATUS_DONE_MASK);
}

bool phy_jtag_master_check_done(struct dptx *dptx)
{
	u16 done = 0;
	int tries = 5;

	while (!done && tries) {
		done = phy_jtag_master_get_done(dptx);
		tries--;
		fsleep(10);
	}

	if (done)
		return TRUE;
	else
		return FALSE;
}

void phy_jtag_master_send_ir(struct dptx *dptx, u8 jtag_addr)
{
	phy_jtag_master_ir_addr(dptx, jtag_addr);
	phy_jtag_master_addr_start(dptx, TRUE);
	phy_jtag_master_addr_start(dptx, FALSE);
}

void phy_jtag_master_send_dr(struct dptx *dptx, u8 cmd, u16 data)
{
	phy_jtag_master_cmd(dptx, cmd);
	phy_jtag_master_data_write(dptx, data);

	phy_jtag_master_data_start(dptx, TRUE);
	phy_jtag_master_data_start(dptx, FALSE);
}

void phy_jtag_slave_select(struct dptx *dptx, u8 desired, u8 jtag_addr)
{
	u32 selected = phy_jtag_get_selected(dptx);

	if (selected != desired) {
		phy_jtag_master_dev_id(dptx, desired);
		phy_jtag_master_send_ir(dptx, jtag_addr);
		if (!phy_jtag_master_check_done(dptx))
			dptx_dbg(dptx, "%s: JTAG Master didn't flag DONE after IR", __func__);
	}
}

void phy_jtag_init(struct dptx *dptx)
{
	phy_jtag_master_trst(dptx, TRUE);
	phy_jtag_master_trst(dptx, FALSE);
	phy_jtag_master_dev_id(dptx, 0);
}

u16 phy_jtag_read(struct dptx *dptx, u8 slave, u16 addr)
{
	if ((slave == JTAG_NONE) || (slave >= JTAG_MAX)) {
		dptx_err(dptx, "%s: JTAG Master doesn't support Slave index %u",
		       __func__, slave);
		return 0;
	}

	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master isn't ready", __func__);

	/* Check current device ID and send IR in case we want to
	 * target a different JTAG slave
	 */
	phy_jtag_slave_select(dptx, slave, slave_crsel[slave]);

	/* set read address */
	phy_jtag_master_send_dr(dptx, JTAG_TAP_READ_CMD, addr);
	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master didn't flag DONE after DR(1)", __func__);

	/* send dummy data to receive requested data */
	phy_jtag_master_send_dr(dptx, JTAG_TAP_DATA_CMD, 0xFFFF);
	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master didn't flag DONE after DR(2)", __func__);

	return phy_jtag_master_data_read(dptx);
}

void phy_jtag_write(struct dptx *dptx, u8 slave, u16 addr, u16 value)
{
	if ((slave == JTAG_NONE) || (slave >= JTAG_MAX)) {
		dptx_err(dptx, "%s: JTAG Master doesn't support Slave index %u",
		       __func__, slave);
		return;
	}

	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master isn't ready", __func__);

	/* Check current device ID and send IR in case we want to
	 * target a different JTAG slave
	 */
	phy_jtag_slave_select(dptx, slave, slave_crsel[slave]);

	/* send write address */
	phy_jtag_master_send_dr(dptx, JTAG_TAP_ADDR_CMD, addr);
	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master didn't flag DONE after DR(1)", __func__);

	/* send write data */
	phy_jtag_master_send_dr(dptx, JTAG_TAP_WRITE_CMD, value);
	if (!phy_jtag_master_check_done(dptx))
		dptx_dbg(dptx, "%s: JTAG Master didn't flag DONE after DR(2)", __func__);
}

/***************
 *   DEBUGFS   *
 ***************/

int phy_dbfs_jtag_show(struct seq_file *s, void *unused)
{
	return 0;
}

int phy_dbfs_jtag_open(struct inode *inode, struct file *file)
{
	return single_open(file, phy_dbfs_jtag_show, inode->i_private);
}

ssize_t phy_dbfs_jtag_write(struct file *file,
			      const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	int i, retval = 0;
	char buf[14];
	char addr_char[7];
	u16 addr;
	u16 value;

	struct seq_file *s = file->private_data;
	struct dptx *dptx = s->private;

	mutex_lock(&dptx->mutex);
	memset(buf, 0, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count))) {
		retval = -EFAULT;
		goto done;
	}

	seq_puts(s, "Buffer: ");
	dptx_dbg(dptx, " %s", buf);

	for (i = 0; i < sizeof(buf); i++) {
		if (buf[i] == ' ') {
			if (kstrtou16(&buf[i + 1], 0, &value) < 0) {
				retval = -EINVAL;
				goto done;
			}
			break;
		}
	}

	dptx_dbg(dptx, " Value it's ok!");

	memcpy(addr_char, buf, i);
	if (kstrtou16(addr_char, 0, &addr) < 0) {
		retval = -EINVAL;
		goto done;
	}
	dptx_dbg(dptx, "Addr it's ok!");

	phy_jtag_write(dptx, JTAG_FPGA, addr, value);

	retval = count;
done:
	mutex_unlock(&dptx->mutex);
	return retval;
}
