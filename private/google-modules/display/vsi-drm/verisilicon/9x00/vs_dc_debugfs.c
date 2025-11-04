// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "vs_dc_debugfs.h"

#include <drm/drm_print.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include "vs_dc.h"

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int vs_dc_reg_dump(struct seq_file *s, struct dc_hw *hw, enum dc_hw_reg_bank_type reg_type)
{
	int ret, ret_dump;
	struct drm_printer p = drm_seq_file_printer(s);

	if (!s || !hw)
		return -EINVAL;

	if (hw->reg_dump_offset > hw->reg_size) {
		seq_printf(s, "Offset %u exceeds register range %u\n", hw->reg_dump_offset,
			   hw->reg_size);
		return -EINVAL;
	}

	if (hw->reg_dump_offset + hw->reg_dump_size > hw->reg_size) {
		seq_printf(s, "Exceeding register range: offset %u, dump size %u\n",
			   hw->reg_dump_offset, hw->reg_dump_size);
		hw->reg_dump_size = hw->reg_size - hw->reg_dump_offset;
	}

	ret = pm_runtime_get_if_in_use(hw->dev);
	if (ret < 0) {
		dev_err(hw->dev, "Failed to power ON, ret %d\n", ret);
		return ret;
	}

	if (!ret) {
		dev_dbg(hw->dev, "Device is powered OFF\n");
		return ret;
	}

	ret_dump = dc_hw_reg_dump(hw, &p, reg_type);

	ret = pm_runtime_put_sync(hw->dev);
	if (ret < 0) {
		dev_err(hw->dev, "Failed to power OFF, ret %d\n", ret);
		return ret;
	}

	return ret_dump;
}

struct reg_dump_data {
	struct dc_hw *hw;
	enum dc_hw_reg_bank_type reg_type;
	size_t dump_size;
};

static struct reg_dump_data reg_dump_data_normal = {
	.reg_type = DC_HW_REG_BANK_ACTIVE,
};
static struct reg_dump_data reg_dump_data_shadow = {
	.reg_type = DC_HW_REG_BANK_SHADOW,
};

static int reg_dump_show(struct seq_file *s, void *data)
{
	struct reg_dump_data *reg_dump_data = s->private;

	return vs_dc_reg_dump(s, reg_dump_data->hw, reg_dump_data->reg_type);
}

static int reg_dump_open(struct inode *inode, struct file *file)
{
	struct reg_dump_data *data = inode->i_private;

	/*
	 * each line (containing up to 16 bytes) will print up to 46 chars,
	 * plus an additional line for the header
	 */
	data->dump_size = ((data->hw->reg_dump_size - 1) / 16 + 2) * 46;

	return single_open_size(file, reg_dump_show, data, data->dump_size);
}

static const struct file_operations reg_dump_fops = {
	.owner = THIS_MODULE,
	.open = reg_dump_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int dc_init_debugfs(struct vs_dc *dc)
{
	struct device *dev = dc->hw.dev;
	struct dentry *dentry_dpu_dump;

	dc->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(dc->debugfs)) {
		dev_err(dev, "could not create debugfs root folder\n");
		return PTR_ERR(dc->debugfs);
	}

	if (dc->path) {
		debugfs_create_u32("min-rd-avg-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.rd_avg_bw_mbps);
		debugfs_create_u32("min-rd-peak-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.rd_peak_bw_mbps);
		debugfs_create_u32("min-rd-rt-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.rd_rt_bw_mbps);
		debugfs_create_u32("min-wr-avg-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.wr_avg_bw_mbps);
		debugfs_create_u32("min-wr-peak-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.wr_peak_bw_mbps);
		debugfs_create_u32("min-wr-rt-bw", 0644, dc->debugfs,
				   &dc->min_qos_config.wr_rt_bw_mbps);
	}

	debugfs_create_u32("min-core-clk", 0644, dc->debugfs, &dc->min_qos_config.core_clk);

	dentry_dpu_dump = debugfs_create_dir("dpu_dump", dc->debugfs);
	if (IS_ERR(dentry_dpu_dump)) {
		dev_warn(dev, "could not create debugfs dpu_dump folder\n");
	} else {
		reg_dump_data_normal.hw = &dc->hw;
		reg_dump_data_shadow.hw = &dc->hw;
		debugfs_create_file("reg_dump", 0444, dentry_dpu_dump, &reg_dump_data_normal,
				    &reg_dump_fops);
		debugfs_create_file("reg_dump_sh", 0444, dentry_dpu_dump, &reg_dump_data_shadow,
				    &reg_dump_fops);
		debugfs_create_u32("reg_dump_offset", 0664, dentry_dpu_dump,
				   &dc->hw.reg_dump_offset);
		debugfs_create_u32("reg_dump_size", 0664, dentry_dpu_dump, &dc->hw.reg_dump_size);
	}

	debugfs_create_bool("disable_hw_reset", 0644, dc->debugfs, &dc->disable_hw_reset);
	debugfs_create_u32("hw_reg_dump_options", 0644, dc->debugfs, &dc->hw_reg_dump_options);

	return 0;
}

void dc_deinit_debugfs(struct vs_dc *dc)
{
	debugfs_remove_recursive(dc->debugfs);
}
#else
int dc_init_debugfs(struct device *dev, struct vs_dc *dc)
{
	return 0;
}
void dc_deinit_debugfs(struct vs_dc *dc)
{
	return;
}
#endif
