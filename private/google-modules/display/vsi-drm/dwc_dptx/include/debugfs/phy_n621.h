/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

int phy_powerup_show(struct seq_file *s, void *unused);
int phy_powerup_open(struct inode *inode, struct file *file);
ssize_t phy_powerup_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos);

extern const struct file_operations phy_powerup_fops;
