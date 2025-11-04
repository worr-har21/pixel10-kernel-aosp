// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel_internal.h"

#include <linux/debugfs.h>
#include <video/mipi_display.h>

#include "gs_panel/gs_panel.h"
#include "gs_drm/gs_drm_connector.h"

/* Private Structs */

struct gs_dsi_reg_data {
	struct mipi_dsi_device *dsi;
	u8 address;
	u8 type;
	u16 flags;
	size_t count;
};

/* Specific Functions */

static int gs_dsi_name_show(struct seq_file *m, void *data)
{
	struct mipi_dsi_device *dsi = m->private;

	seq_puts(m, dsi->name);
	seq_putc(m, '\n');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gs_dsi_name);

static int gs_allowed_hs_clks_show(struct seq_file *m, void *data)
{
	struct gs_panel *ctx = m->private;

	for (int i = 0; i < MAX_ALLOWED_MIPI_CLOCK_NUM; i++) {
		if (!ctx->allowed_hs_clks.clks[i])
			break;
		seq_printf(m, "%u ", ctx->allowed_hs_clks.clks[i]);
	}
	seq_puts(m, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gs_allowed_hs_clks);

static ssize_t gs_debugfs_reset_panel(struct file *file, const char __user *user_buf, size_t count,
				      loff_t *ppos)
{
	bool reset_panel;
	int ret;
	struct mipi_dsi_device *dsi = file->private_data;
	struct gs_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!gs_is_panel_active(ctx))
		return -EPERM;

	ret = kstrtobool_from_user(user_buf, count, &reset_panel);
	if (ret)
		return ret;

	if (reset_panel) {
		if (!gs_panel_gpio_set(ctx, DISP_RESET_GPIO, 0))
			pr_info("reset_panel: pull reset_gpio to low to reset panel\n");
	}

	return count;
}

static const struct file_operations gs_reset_panel_fops = {
	.open = simple_open,
	.write = gs_debugfs_reset_panel,
};

static ssize_t gs_te2_irq_en_write(struct file *file, const char __user *user_buf, size_t count,
				   loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct gs_panel *ctx = m->private;
	int ret;
	bool en;

	ret = kstrtobool_from_user(user_buf, count, &en);
	if (ret)
		return ret;

	if (en != ctx->te2.irq_en) {
		gs_panel_enable_te2_irq(ctx, en);
		ctx->te2.irq_en = en;
	}

	return count;
}

static int gs_te2_irq_en_show(struct seq_file *m, void *data)
{
	struct gs_panel *ctx = m->private;

	seq_printf(m, "%d\n", ctx->te2.irq_en);

	return 0;
}

static int gs_te2_irq_en_open(struct inode *inode, struct file *file)
{
	return single_open(file, gs_te2_irq_en_show, inode->i_private);
}

static const struct file_operations gs_te2_irq_en_fops = {
	.open = gs_te2_irq_en_open,
	.read = seq_read,
	.write = gs_te2_irq_en_write,
	.llseek = seq_lseek,
	.release = seq_release,
};

static ssize_t parse_byte_buf(u8 *out, size_t len, char *src)
{
	const char *skip = "\n ";
	size_t i = 0;
	int rc = 0;
	char *s;

	while (src && !rc && i < len) {
		s = strsep(&src, skip);
		if (*s != '\0') {
			rc = kstrtou8(s, 16, out + i);
			i++;
		}
	}

	return rc ?: i;
}

/* DSI Payload Functions */

static ssize_t gs_dsi_payload_write(struct file *file, const char __user *user_buf, size_t count,
				    loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct gs_dsi_reg_data *reg_data = m->private;
	char *buf;
	char *payload;
	size_t len;
	ssize_t bytes_to_send;
	ssize_t ret = 0;

	buf = memdup_user_nul(user_buf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	/* calculate length for worst case (1 digit per byte + whitespace) */
	len = (count + 1) / 2;
	payload = kmalloc(len, GFP_KERNEL);
	if (!payload) {
		kfree(buf);
		return -ENOMEM;
	}

	bytes_to_send = parse_byte_buf(payload, len, buf);
	if (bytes_to_send > 0) {
		ssize_t bytes_sent;
		if (reg_data->type)
			bytes_sent = gs_dsi_dcs_transfer(reg_data->dsi, reg_data->type, payload,
							 bytes_to_send, reg_data->flags);
		else
			bytes_sent = gs_dsi_dcs_write_buffer(reg_data->dsi, payload, bytes_to_send,
							     reg_data->flags);
		if (bytes_sent != bytes_to_send) {
			ret = -EIO;
			if (bytes_sent > 0)
				pr_warn("%s: request to send %zd bytes, actual sent %zd bytes\n",
					__func__, bytes_to_send, bytes_sent);
		}
	} else {
		ret = -EINVAL;
	}

	kfree(buf);
	kfree(payload);

	return ret ?: count;
}

static int gs_dsi_payload_show(struct seq_file *m, void *data)
{
	struct gs_dsi_reg_data *reg_data = m->private;
	char *buf;
	ssize_t rc;

	if (!reg_data->count)
		return -EINVAL;

	buf = kmalloc(reg_data->count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rc = mipi_dsi_dcs_read(reg_data->dsi, reg_data->address, buf, reg_data->count);
	if (rc > 0) {
		seq_hex_dump(m, "", DUMP_PREFIX_NONE, 16, 1, buf, rc, false);
		rc = 0;
	} else if (rc == 0) {
		dev_warn(&reg_data->dsi->dev, "no response back, addr(%#x)\n", reg_data->address);
		rc = -ENODATA;
	}
	kfree(buf);

	return rc;
}

static int gs_dsi_payload_open(struct inode *inode, struct file *file)
{
	return single_open(file, gs_dsi_payload_show, inode->i_private);
}

static const struct file_operations gs_dsi_payload_fops = {
	.owner = THIS_MODULE,
	.open = gs_dsi_payload_open,
	.write = gs_dsi_payload_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Cmdset Functions */

static u8 panel_get_cmd_type(const struct gs_dsi_cmd *cmd)
{
	if (cmd->type)
		return cmd->type;

	switch (cmd->cmd_len) {
	case 0:
		return -EINVAL;
	case 1:
		return MIPI_DSI_DCS_SHORT_WRITE;
	case 2:
		return MIPI_DSI_DCS_SHORT_WRITE_PARAM;
	default:
		return MIPI_DSI_DCS_LONG_WRITE;
	}
}

static int panel_cmdset_show(struct seq_file *m, void *data)
{
	const struct gs_dsi_cmdset *cmdset = m->private;
	const struct gs_dsi_cmd *cmd;
	u8 type;
	int i;

	for (i = 0; i < cmdset->num_cmd; i++) {
		cmd = &cmdset->cmds[i];

		type = panel_get_cmd_type(cmd);
		seq_printf(m, "0x%02x ", type);
		seq_hex_dump(m, "\t", DUMP_PREFIX_NONE, 16, 1, cmd->cmd, cmd->cmd_len, false);

		if (cmd->delay_ms)
			seq_printf(m, "wait \t%dms\n", cmd->delay_ms);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(panel_cmdset);

void gs_panel_debugfs_create_cmdset(struct dentry *parent, const struct gs_dsi_cmdset *cmdset,
				    const char *name)
{
	if (!cmdset)
		return;

	debugfs_create_file(name, 0600, parent, (void *)cmdset, &panel_cmdset_fops);
}
EXPORT_SYMBOL_GPL(gs_panel_debugfs_create_cmdset);

/* High-level Functions */

/**
 * debugfs_add_panel_folder - Adds "panel" debugfs folder
 * @entries: Pointer to use to reference output location for panel folder entry
 * @parent: debugfs entry for parent (likely the connector)
 *
 * Return: 0 on success, negative value on error
 */
static int debugfs_add_panel_folder(struct gs_panel_debugfs_entries *entries, struct dentry *parent)
{
	if (!parent)
		return -EINVAL;

	entries->panel = debugfs_create_dir("panel", parent);
	if (!entries->panel)
		return -EPERM;

	return 0;
}

/**
 * debugfs_add_driver_specific_entries() - call driver-specific debugfs_init
 * @ctx: Reference to panel data
 * @parent: debugfs entry for parent (likely the connector)
 *
 * This short function calls any driver-specific function to add to the debugfs
 * system, based on the function in the panel's struct drm_panel_funcs.
 */
static void debugfs_add_driver_specific_entries(struct gs_panel *ctx, struct dentry *parent)
{
	if (ctx->desc->panel_func && ctx->desc->panel_func->debugfs_init)
		ctx->desc->panel_func->debugfs_init(&ctx->base, parent);
}

/**
 * debugfs_add_dsi_folder - Adds debugfs folder for direct dsi operations
 * @dsi: DSI device pointer for panel
 * @entries: Reference to debugfs entries for panel
 *
 * Return: 0 on success, negative value on error
 */
static int debugfs_add_dsi_folder(struct mipi_dsi_device *dsi,
				  struct gs_panel_debugfs_entries *entries)
{
	struct dentry *reg_root;
	struct dentry *panel_entry = entries->panel;
	struct gs_dsi_reg_data *reg_data;

	reg_root = debugfs_create_dir("reg", panel_entry);
	if (!reg_root)
		return -EFAULT;

	reg_data = devm_kzalloc(&dsi->dev, sizeof(*reg_data), GFP_KERNEL);
	if (!reg_data)
		return -ENOMEM;

	reg_data->dsi = dsi;

	debugfs_create_u8("address", 0600, reg_root, &reg_data->address);
	debugfs_create_u8("type", 0600, reg_root, &reg_data->type);
	debugfs_create_size_t("count", 0600, reg_root, &reg_data->count);
	debugfs_create_u16("flags", 0600, reg_root, &reg_data->flags);
	debugfs_create_file("payload", 0600, reg_root, reg_data, &gs_dsi_payload_fops);

	debugfs_create_file("name", 0600, panel_entry, dsi, &gs_dsi_name_fops);
	debugfs_create_file("reset_panel", 0200, panel_entry, dsi, &gs_reset_panel_fops);

	entries->reg = reg_root;

	return 0;
}

/**
 * debugfs_add_cmdset_folder - Adds debugfs folder for reading cmdsets
 * @ctx: Reference to panel data
 * @entries: Reference to debugfs entries for panel
 *
 * Return: 0 on success, negative value on error
 */
static int debugfs_add_cmdset_folder(struct gs_panel *ctx, struct gs_panel_debugfs_entries *entries)
{
	const struct gs_panel_desc *desc = ctx->desc;

	entries->cmdset = debugfs_create_dir("cmdsets", entries->panel);
	if (!entries->cmdset) {
		dev_err(ctx->dev, "can't create cmdset dir\n");
		return -EFAULT;
	}

	gs_panel_debugfs_create_cmdset(entries->cmdset, desc->off_cmdset, "off");

	if (desc->lp_modes && desc->lp_cmdset) {
		struct dentry *lpd;
		int i;

		if (desc->binned_lp) {
			lpd = debugfs_create_dir("lp", entries->cmdset);
			if (!lpd) {
				dev_err(ctx->dev, "can't create lp dir\n");
				return -EFAULT;
			}

			for (i = 0; i < desc->num_binned_lp; i++) {
				const struct gs_binned_lp *b = &desc->binned_lp[i];

				gs_panel_debugfs_create_cmdset(lpd, &b->cmdset, b->name);
			}
		} else {
			lpd = entries->cmdset;
		}
		gs_panel_debugfs_create_cmdset(lpd, desc->lp_cmdset, "lp_entry");
	}

	return 0;
};

/**
 * debugfs_add_misc_panel_entries - Add other debugfs entries for the panel
 * @ctx: Reference to panel data
 * @panel_entry: Reference to "panel" debugfs entry
 *
 * Return: 0 on success, negative value on error
 */
static int debugfs_add_misc_panel_entries(struct gs_panel *ctx, struct dentry *panel_entry)
{
	debugfs_create_u32("rev", 0600, panel_entry, &ctx->panel_rev_id.id);
	debugfs_create_bool("lhbm_postwork_disabled", 0600, panel_entry,
			    &ctx->lhbm.post_work_disabled);
	debugfs_create_u32("common_work_delay_ms", 0600, panel_entry,
			   &ctx->common_work.delay_ms);
	debugfs_create_file("te2_irq_en", 0600, panel_entry, ctx, &gs_te2_irq_en_fops);
	debugfs_create_file("allowed_hs_clks", 0400, panel_entry, ctx, &gs_allowed_hs_clks_fops);

	/*
	 * TODO(tknelms)
	const struct gs_panel_desc *desc = ctx->desc;

	if (!funcs)
		return -EINVAL;

	if (funcs->print_gamma)
		debugfs_create_file("gamma", 0600, panel_entry, ctx, &panel_gamma_fops);
	*/
	return 0;
}

/* Regulators */
static int regulator_voltage_show(struct seq_file *m, void *data)
{
	struct regulator *regulator = m->private;

	seq_printf(m, "%u\n", regulator_get_voltage(regulator));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regulator_voltage);

static int regulator_enabled_show(struct seq_file *m, void *data)
{
	struct regulator *regulator = m->private;

	seq_printf(m, "%d\n", regulator_is_enabled(regulator));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regulator_enabled);

static int debugfs_add_regulator_details(struct dentry *parent,
				  const char *regulator_name, struct regulator *reg)
{
	struct dentry *regulator_details;

	if (!reg)
		return -EINVAL;

	regulator_details = debugfs_create_dir(regulator_name, parent);
	debugfs_create_file("voltage", 0400, regulator_details, reg, &regulator_voltage_fops);
	debugfs_create_file("enable", 0400, regulator_details, reg, &regulator_enabled_fops);

	return 0;
}

static int debugfs_add_regulator_folder(struct gs_panel *ctx, struct dentry *panel_entry)
{
	struct dentry *regulator_root;
	struct gs_panel_regulator *gs_reg = &ctx->regulator;

	regulator_root = debugfs_create_dir("regulator", panel_entry);

#define add_regulator_debugfs(name) \
	debugfs_add_regulator_details(regulator_root, #name, gs_reg->name)

	add_regulator_debugfs(vci);
	add_regulator_debugfs(vddi);
	add_regulator_debugfs(vddd);
	add_regulator_debugfs(vddr_en);
	add_regulator_debugfs(vddr);
	add_regulator_debugfs(avdd);
	add_regulator_debugfs(avee);

	return 0;
}

int gs_panel_create_debugfs_entries(struct gs_panel *ctx, struct dentry *parent)
{
	int ret;

	ret = debugfs_add_panel_folder(&ctx->debugfs_entries, parent);
	if (ret)
		return ret;
	ret = debugfs_add_dsi_folder(to_mipi_dsi_device(ctx->dev), &ctx->debugfs_entries);
	if (ret)
		return ret;
	ret = debugfs_add_cmdset_folder(ctx, &ctx->debugfs_entries);
	if (ret)
		return ret;
	ret = debugfs_add_misc_panel_entries(ctx, ctx->debugfs_entries.panel);
	if (ret)
		return ret;
	debugfs_add_driver_specific_entries(ctx, parent);
	debugfs_add_regulator_folder(ctx, ctx->debugfs_entries.panel);

	return 0;
}
