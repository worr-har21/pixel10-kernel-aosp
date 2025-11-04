// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 */
#include <linux/debugfs.h>
#include <core.h>

#include "pinctrl-debugfs.h"
#include "common.h"

bool fops_stats_en;
module_param(fops_stats_en, bool, false);

#define DEBUGFS_DUMP_REGS_HEADER " idx |          SOC pin           |   PARAM  |   DMUX   |  TXDATA  |  RXGATE  |  RXDATA  |   ISR    |  ISROVF  |   IER    |   IMR    |    ITR\n"
#define DEBUGFS_PINS_HEADER " idx |          SOC pin           | IRQ Trigger | Drv Strength | Direction | Value\n"
#define DEBUGFS_FOPS_HEADER " idx |       SOC pin        |   Dir input  |  Dir output  |   Dir get    |     Get      |     Set      |    Config\n"

#define F_DENTRY(filp) ((filp)->f_path.dentry)
#define PINCTRL_STRING_ATTR_BUF_INITIAL_SIZE_CHARS 1024

struct pinctrl_string_attr {
	int (*read_callback)(void *data, struct pinctrl_debugfs_attr_file *attr_file);
	int (*write_callback)(void *data, const struct pinctrl_char_buf *str_buf);
	struct pinctrl_debugfs_attr_file read_buf;
	struct pinctrl_char_buf write_buf;
	void *data;
	struct mutex mutex;
};

static int init_pinctrl_debugfs_attr_file(struct pinctrl_debugfs_attr_file *attr_file)
{
	attr_file->buf = kvmalloc(PINCTRL_STRING_ATTR_BUF_INITIAL_SIZE_CHARS, GFP_KERNEL);
	if (!attr_file->buf)
		return -ENOMEM;

	attr_file->size = PINCTRL_STRING_ATTR_BUF_INITIAL_SIZE_CHARS;
	attr_file->count = 0;

	return 0;
}

int pinctrl_string_attr_open(struct inode *inode, struct file *file,
			     int (*read_callback)(void *, struct pinctrl_debugfs_attr_file *),
			     int (*write_callback)(void *, const struct pinctrl_char_buf *))
{
	struct pinctrl_string_attr *attr;
	int ret;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	ret = init_pinctrl_debugfs_attr_file(&attr->read_buf);
	if (ret == 0)
		attr->read_callback = read_callback;

	attr->write_callback = write_callback;
	attr->data = inode->i_private;
	mutex_init(&attr->mutex);

	file->private_data = attr;

	return nonseekable_open(inode, file);
}

int pinctrl_string_attr_release(struct inode *inode, struct file *file)
{
	struct pinctrl_string_attr *attr = file->private_data;

	kvfree(attr->read_buf.buf);
	kfree(attr);

	return 0;
}

ssize_t pinctrl_string_attr_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	struct pinctrl_string_attr *attr;
	ssize_t ret;

	attr = file->private_data;

	if (!attr->read_callback)
		return -EACCES;

	ret = mutex_lock_interruptible(&attr->mutex);
	if (ret)
		return ret;

	if (!(*ppos)) {
		/* first read */
		ret = attr->read_callback(attr->data, &attr->read_buf);
		if (ret)
			goto sar_out;
	}

	ret = simple_read_from_buffer(buf, len, ppos, attr->read_buf.buf, attr->read_buf.count);

sar_out:
	mutex_unlock(&attr->mutex);
	return ret;
}

ssize_t pinctrl_string_attr_write(struct file *file, const char __user *buf, size_t len,
				  loff_t *ppos)
{
	struct pinctrl_string_attr *attr;
	int ret;

	attr = file->private_data;
	if (!attr->write_callback)
		return -EACCES;

	if (len > PINCTRL_STRING_ATTRIBUTE_MAX_FILE_SIZE)
		return -EFBIG;

	attr->write_buf.buf = kvmalloc(len, GFP_KERNEL);
	if (!attr->write_buf.buf)
		return -ENOMEM;

	ret = mutex_lock_interruptible(&attr->mutex);
	if (ret)
		goto out;

	ret = -EFAULT;
	if (copy_from_user(attr->write_buf.buf, buf, len - 1))
		goto out_locked;

	attr->write_buf.size = len;
	attr->write_buf.buf[len - 1] = '\0';

	ret = attr->write_callback(attr->data, &attr->write_buf);
	if (ret == 0)
		ret = len; /* on success, claim we got the whole input */

out_locked:
	mutex_unlock(&attr->mutex);
out:
	kvfree(attr->write_buf.buf);
	attr->write_buf.buf = NULL;
	attr->write_buf.size = 0;

	return ret;
}

ssize_t pinctrl_debugfs_string_attr_read(struct file *file, char __user *buf, size_t len,
					 loff_t *ppos)
{
	struct dentry *dentry = F_DENTRY(file);
	ssize_t ret;

	ret = debugfs_file_get(dentry);
	if (unlikely(ret))
		return ret;
	ret = pinctrl_string_attr_read(file, buf, len, ppos);
	debugfs_file_put(dentry);
	return ret;
}

ssize_t pinctrl_debugfs_string_attr_write(struct file *file, const char __user *buf, size_t len,
					  loff_t *ppos)
{
	struct dentry *dentry = F_DENTRY(file);
	ssize_t ret;

	ret = debugfs_file_get(dentry);
	if (unlikely(ret))
		return ret;
	ret = pinctrl_string_attr_write(file, buf, len, ppos);
	debugfs_file_put(dentry);
	return ret;
}

static int expand_buffer(struct pinctrl_debugfs_attr_file *attr_file)
{
	char *new_buf;

	if (attr_file->size >= PINCTRL_STRING_ATTRIBUTE_MAX_FILE_SIZE)
		return -EFBIG;

	new_buf = kvrealloc(attr_file->buf, attr_file->size, attr_file->size << 1, GFP_KERNEL);
	if (new_buf) {
		attr_file->buf = new_buf;
		attr_file->size <<= 1;
	} else {
		return -ENOMEM;
	}

	return 0;
}

void pinctrl_debugfs_attr_file_puts(struct pinctrl_debugfs_attr_file *attr_file, const char *s)
{
	int n = strlen(s);
	int free_capacity = attr_file->size - attr_file->count - 1;
	int ret;

	if (free_capacity == 0)
		return;

	/* If expand_buffer fails, just do a partial copy */
	if (n > free_capacity && !expand_buffer(attr_file))
		free_capacity = attr_file->size - attr_file->count - 1;

	ret = strscpy(attr_file->buf + attr_file->count, s, free_capacity + 1);

	if (ret > 0)
		attr_file->count += ret;
}

void pinctrl_debugfs_attr_file_printf(struct pinctrl_debugfs_attr_file *attr_file,
				      const char *fmt, ...)
{
	va_list args;
	int n;
	int free_capacity = attr_file->size - attr_file->count - 1;

	if (free_capacity == 0)
		return;

	va_start(args, fmt);
	n = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	if (n > free_capacity && !expand_buffer(attr_file))
		free_capacity = attr_file->size - attr_file->count - 1;

	va_start(args, fmt);
	n = vscnprintf(attr_file->buf + attr_file->count, free_capacity + 1, fmt, args);
	va_end(args);

	attr_file->count += n;
}

static void print_str_to_file_or_log(const char *str, struct google_pinctrl *gctl,
				     struct seq_file *file, bool print_to_seq_file)
{
	if (print_to_seq_file)
		seq_puts(file, str);
	else
		dev_info(gctl->dev, "%s", str);
}

/**
 * pin_excl_reg() - Checks if pin DOESN'T CONTAIN a register (i.e. the pin excludes the register)
 * @pin_reg_flags: register exclusion flags for a pin
 * @reg: the register to test for exclusion
 *
 * This function checks if a register is excluded or not - i.e. if it is not present on a pin. Note
 * the inverted logic - if the register is not present/is excluded - the flag will be set to 1.
 * Otherwise, if it is present/isn't excluded - the exclusion flag will be set to 0.
 *
 * Note that flags layout should have the same ordering as members of the
 * google_pinctrl_register_idx enum
 */
static bool pin_excl_reg(struct google_pinctrl_registers_flags pin_reg_flags,
			 enum google_pinctrl_register_idx reg)
{
	return pin_reg_flags.val & BIT(reg);
}

static int dump_regs_form(struct google_pinctrl *gctl, struct seq_file *file)
{
	int ngroups, ret, n_pins_excl_regs;
	int pin_excl_regs_i = 0;
	const bool print_to_seq_file = (file != NULL);
	unsigned long flags;
	char tmp_buf[sizeof(DEBUGFS_DUMP_REGS_HEADER) + 3];
	const size_t buf_size = sizeof(tmp_buf);
	const struct google_pinctrl_soc_sswrp_info *info;
	const struct google_pingroup *pingroup;
	/* Flag layout ordering must be the same as members of google_pinctrl_register_idx enum */
	const struct google_pinctrl_registers_flags *pin_excl_regs;
	u32 reg_val;
	bool pin_excl;

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	print_str_to_file_or_log(DEBUGFS_DUMP_REGS_HEADER, gctl, file,
				 print_to_seq_file);

	ngroups = gctl->info->num_groups;
	info = gctl->info;

	n_pins_excl_regs = info->npins_excl_regs;
	pin_excl_regs = n_pins_excl_regs == 0 ? NULL
					      : &info->pins_excl_regs[pin_excl_regs_i++];

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	for (int i = 0; i < ngroups; ++i) {
		size_t tmp_buf_count;

		pingroup = &gctl->info->groups[i];

		tmp_buf_count = scnprintf(tmp_buf, buf_size, "%5d %28s", pingroup->num,
					  pingroup->name);

		pin_excl = (pin_excl_regs != NULL) && (pin_excl_regs->pin_id == i);

		for (int j = 0; j < REGS_NUM; ++j) {
			if (!pin_excl || !pin_excl_reg(*pin_excl_regs, j)) {
				reg_val = google_readl(reg2offset[j], gctl, pingroup);
				tmp_buf_count += scnprintf(tmp_buf + tmp_buf_count,
							   buf_size - tmp_buf_count, " %#010X",
							   reg_val);
			} else {
				tmp_buf_count += scnprintf(tmp_buf + tmp_buf_count,
							   buf_size - tmp_buf_count, " %10s", "-");
			}
		}

		tmp_buf_count = min(tmp_buf_count, buf_size - 2);

		tmp_buf[tmp_buf_count++] = '\n';
		tmp_buf[tmp_buf_count] = '\0';

		print_str_to_file_or_log(tmp_buf, gctl, file,
					 print_to_seq_file);

		if (pin_excl) {
			pin_excl_regs = pin_excl_regs_i < n_pins_excl_regs
						? &info->pins_excl_regs[pin_excl_regs_i++] : NULL;
		}
	}

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}

int dump_regs_logs(struct google_pinctrl *gctl)
{
	return dump_regs_form(gctl, NULL);
}

static int dump_regs_show(struct seq_file *s, void *p)
{
	struct google_pinctrl *gctl = s->private;

	return dump_regs_form(gctl, s);
}
DEFINE_SHOW_ATTRIBUTE(dump_regs);

static int trigger_irq_set(void *data, u64 pin_index)
{
	struct google_pinctrl *gctl = data;
	const struct google_pingroup *pingroup;
	struct generic_ixr_t itr_reg;
	unsigned long flags;
	int ret;

	if (pin_index >= gctl->info->num_groups)
		return -EINVAL;

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	pingroup = &gctl->info->groups[pin_index];

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	itr_reg.layout.irq = 0;
	google_writel(ITR_OFFSET, itr_reg.val, gctl, pingroup);

	itr_reg.layout.irq = 1;
	google_writel(ITR_OFFSET, itr_reg.val, gctl, pingroup);

	itr_reg.layout.irq = 0;
	google_writel(ITR_OFFSET, itr_reg.val, gctl, pingroup);

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(trigger_irq_fops, NULL, trigger_irq_set, "%llu\n");

static int rpm_count_show(struct seq_file *s, void *p)
{
	struct google_pinctrl *gctl = s->private;
	int i, idx;

	seq_printf(s, "rpm use_cnt: %d\n", atomic_read(&gctl->dev->power.usage_count));
	seq_printf(s, "rpm_get_cnt: %d\n", atomic_read(&gctl->rpm_get_count));
	seq_printf(s, "rpm_put_cnt: %d\n", atomic_read(&gctl->rpm_put_count));
	seq_printf(s, "rpm_susp_cnt: %d\n", atomic_read(&gctl->rpm_suspend_count));
	seq_printf(s, "rpm_resm_cnt: %d\n", atomic_read(&gctl->rpm_resume_count));
	seq_printf(s, "sys_susp_cnt: %d\n", atomic_read(&gctl->system_suspend_count));
	seq_printf(s, "sys_resm_cnt: %d\n", atomic_read(&gctl->system_resume_count));

	seq_printf(s, "Global max runtime suspend interval (ms): %lld.%03lld\n",
		   ktime_to_ms(gctl->rpm_max_suspend_time),
		   ktime_to_us(gctl->rpm_max_suspend_time) % 1000);

	seq_printf(s, "Last %d runtime suspend/resume intervals (ms): ", RPM_STATS_SIZE);
	idx = (gctl->rpm_suspend_resume_idx - 1 + RPM_STATS_SIZE) % RPM_STATS_SIZE;
	for (i = 0; i < RPM_STATS_SIZE; i++) {
		seq_printf(s, "%lld.%03lld ",
			ktime_to_ms(gctl->rpm_suspend_resume_times[idx]),
			ktime_to_us(gctl->rpm_suspend_resume_times[idx]) % 1000);
		idx = (idx - 1 + RPM_STATS_SIZE) % RPM_STATS_SIZE;
	}
	seq_puts(s, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rpm_count);

static int fops_count_show(struct seq_file *s, void *p)
{
	struct google_pinctrl *gctl = s->private;
	int ngroups = gctl->info->num_groups;

	if (!gctl->g_pingroups_fops_stats)
		return -EINVAL;

	seq_puts(s, DEBUGFS_FOPS_HEADER);

	for (int i = 0; i < ngroups; ++i) {
		const struct google_pingroup *g = &gctl->info->groups[i];

		seq_printf(s, "%5d %22s %14d %14d %14d %14d %14d %14d\n",
			   g->num, g->name,
			   atomic_read(&gctl->g_pingroups_fops_stats[i].dir_input_count),
			   atomic_read(&gctl->g_pingroups_fops_stats[i].dir_output_count),
			   atomic_read(&gctl->g_pingroups_fops_stats[i].dir_get_count),
			   atomic_read(&gctl->g_pingroups_fops_stats[i].get_count),
			   atomic_read(&gctl->g_pingroups_fops_stats[i].set_count),
			   atomic_read(&gctl->g_pingroups_fops_stats[i].config_count)
		);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(fops_count);

static int debugfs_direction_write_callback(void *data, const struct pinctrl_char_buf *str_buf)
{
	struct google_pinctrl *gctl = data;
	const struct google_pingroup *pingroup;
	int ngroups;
	unsigned int index;
	char direction[5];
	struct param_t param_reg;
	struct txdata_t tx_reg;
	u32 param_ie, txdata_oe;
	unsigned long flags, param_format_type;
	int ret;

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	ngroups = gctl->info->num_groups;

	ret = sscanf(str_buf->buf, "%u %4s", &index, direction);
	if (ret != 2 || index >= ngroups)
		return -EINVAL;

	if (strcmp(direction, "none") == 0) {
		param_ie = 0;
		txdata_oe = 0;
	} else if (strcmp(direction, "in") == 0) {
		param_ie = 1;
		txdata_oe = 0;
	} else if (strcmp(direction, "out") == 0) {
		param_ie = 0;
		txdata_oe = 1;
	} else if (strcmp(direction, "bi") == 0) {
		param_ie = 1;
		txdata_oe = 1;
	} else {
		return -EINVAL;
	}

	pingroup = &gctl->info->groups[index];
	param_format_type = (unsigned long) gctl->info->pins[index].drv_data;

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	param_reg.val = google_readl(PARAM_OFFSET, gctl, pingroup);
	tx_reg.val = google_readl(TXDATA_OFFSET, gctl, pingroup);

	SET_PARAM_FIELD(param_reg, param_format_type, ie, param_ie);
	tx_reg.layout.oe = txdata_oe;

	google_writel(PARAM_OFFSET, param_reg.val, gctl, pingroup);
	google_writel(TXDATA_OFFSET, tx_reg.val, gctl, pingroup);

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}
DEFINE_PINCTRL_DEBUGFS_STRING_ATTRIBUTE(debugfs_direction, NULL, debugfs_direction_write_callback);

static int debugfs_drive_strength_write_callback(void *data, const struct pinctrl_char_buf *str_buf)
{
	struct google_pinctrl *gctl = data;
	const struct google_pingroup *pingroup;
	int ngroups;
	unsigned int index, drive_strength;
	struct param_t param_reg;
	unsigned long flags, param_format_type;
	int ret;

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	ngroups = gctl->info->num_groups;

	ret = sscanf(str_buf->buf, "%u %u", &index, &drive_strength);
	if (ret != 2 || index >= ngroups || drive_strength > 15)
		return -EINVAL;

	pingroup = &gctl->info->groups[index];
	param_format_type = (unsigned long) gctl->info->pins[index].drv_data;

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	param_reg.val = google_readl(PARAM_OFFSET, gctl, pingroup);
	SET_PARAM_FIELD(param_reg, param_format_type, drv, drive_strength);

	google_writel(PARAM_OFFSET, param_reg.val, gctl, pingroup);

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}
DEFINE_PINCTRL_DEBUGFS_STRING_ATTRIBUTE(debugfs_drive_strength, NULL,
					debugfs_drive_strength_write_callback);

static int debugfs_value_write_callback(void *data, const struct pinctrl_char_buf *str_buf)
{
	struct google_pinctrl *gctl = data;
	const struct google_pingroup *pingroup;
	int ngroups;
	unsigned int index, csr_pad_val;
	struct rxdata_t rxdata_reg;
	unsigned long flags;
	int ret;

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	ngroups = gctl->info->num_groups;

	ret = sscanf(str_buf->buf, "%u %u", &index, &csr_pad_val);
	if (ret != 2 || index >= ngroups || csr_pad_val > 1)
		return -EINVAL;

	pingroup = &gctl->info->groups[index];

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	rxdata_reg.val = google_readl(RXDATA_OFFSET, gctl, pingroup);
	rxdata_reg.layout.pad_val = csr_pad_val;

	google_writel(RXDATA_OFFSET, rxdata_reg.val, gctl, pingroup);

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}
DEFINE_PINCTRL_DEBUGFS_STRING_ATTRIBUTE(debugfs_value, NULL, debugfs_value_write_callback);

static const char *get_direction(u32 param_ie, u32 txdata_oe)
{
	if (param_ie == 0 && txdata_oe == 0)
		return "none";
	else if (param_ie == 1 && txdata_oe == 0)
		return "in";
	else if (param_ie == 0 && txdata_oe == 1)
		return "out";
	else if (param_ie == 1 && txdata_oe == 1)
		return "bi";
	else
		return "err?";
}

static bool pin_has_reg(const struct google_pinctrl_registers_flags *pin_excl_regs, bool pin_excl,
			enum google_pinctrl_register_idx reg)
{
	return !pin_excl || !pin_excl_reg(*pin_excl_regs, reg);
}

static void pin_show_excluded_reg_print(struct google_pinctrl *gctl,
					const struct google_pingroup *pgroup,
					const struct google_pinctrl_registers_flags *pin_excl_regs,
					bool pin_excl, enum google_pinctrl_register_idx reg,
					struct seq_file *s, unsigned int fmt_width,
					unsigned int pin_index, unsigned long param_format_type,
					struct param_t param_reg)
{
	u32 reg_val;
	u32 param_ie = 0;
	struct txdata_t tx_reg;
	struct rxdata_t rx_reg;

	if (!pin_has_reg(pin_excl_regs, pin_excl, reg)) {
		seq_printf(s, " %*s", fmt_width, "-");
		return;
	}

	reg_val = google_readl(reg2offset[reg], gctl, pgroup);

	switch (reg) {
	case PARAM_ID:
		param_format_type = (unsigned long)gctl->info->pins[pin_index].drv_data;
		param_reg.val = reg_val;
		GET_PARAM_FIELD(param_reg, param_format_type, drv, reg_val);
		break;
	case TXDATA_ID:
		GET_PARAM_FIELD(param_reg, param_format_type, ie, param_ie);
		tx_reg.val = reg_val;
		seq_printf(s, " %*s", fmt_width, get_direction(param_ie, tx_reg.layout.oe));
		return;
	case RXDATA_ID:
		rx_reg.val = reg_val;
		reg_val = rx_reg.layout.pad_val;
		break;
	case ISR_ID:
		break;
	default:
		return;
	}

	seq_printf(s, " %*u", fmt_width, reg_val);

	if (reg == PARAM_ID) {
		pin_show_excluded_reg_print(gctl, pgroup, pin_excl_regs, pin_excl, TXDATA_ID, s, 11,
					    pin_index, param_format_type, param_reg);
	}
}

static int pins_show(struct seq_file *s, void *p)
{
	struct google_pinctrl *gctl = s->private;
	int ngroups, ret, n_pins_excl_regs;
	int pin_excl_regs_i = 0;
	unsigned long flags;
	const struct google_pinctrl_soc_sswrp_info *info;
	const struct google_pingroup *pgroup;
	const struct google_pinctrl_registers_flags *pin_excl_regs;
	bool pin_excl;
	struct param_t param_reg = {
		.val = 0
	};

	ret = google_pinctrl_get_csr_pd(gctl);
	if (ret < 0)
		return ret;

	seq_puts(s, DEBUGFS_PINS_HEADER);

	ngroups = gctl->info->num_groups;
	info = gctl->info;

	n_pins_excl_regs = info->npins_excl_regs;
	pin_excl_regs = n_pins_excl_regs == 0 ? NULL : &info->pins_excl_regs[pin_excl_regs_i++];

	ret = google_pinctrl_trylock(gctl, &flags, false);
	if (ret < 0) {
		google_pinctrl_put_csr_pd(gctl);
		return ret;
	}

	for (int i = 0; i < ngroups; ++i) {
		pgroup = &gctl->info->groups[i];
		pin_excl = (pin_excl_regs != NULL) && (pin_excl_regs->pin_id == i);

		seq_printf(s, "%5d %28s", pgroup->num, pgroup->name);

		pin_show_excluded_reg_print(gctl, pgroup, pin_excl_regs, pin_excl, ISR_ID, s, 13,
					    i, 0, param_reg);
		pin_show_excluded_reg_print(gctl, pgroup, pin_excl_regs, pin_excl, PARAM_ID, s, 14,
					    i, 0, param_reg);
		pin_show_excluded_reg_print(gctl, pgroup, pin_excl_regs, pin_excl, RXDATA_ID, s, 7,
					    i, 0, param_reg);
		seq_puts(s, "\n");

		if (pin_excl) {
			pin_excl_regs = pin_excl_regs_i < n_pins_excl_regs
						? &info->pins_excl_regs[pin_excl_regs_i++] : NULL;
		}
	}

	google_pinctrl_unlock(gctl, &flags);

	google_pinctrl_put_csr_pd(gctl);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pins);

static int google_pinctrl_debugfs_create_file(const char *name, umode_t mode, struct dentry *parent,
					      void *data, const struct file_operations *fops)
{
	struct google_pinctrl *gctl = data;
	struct dentry *tmp;
	int ret;

	tmp = debugfs_create_file(name, mode, parent, data, fops);
	if (IS_ERR_OR_NULL(tmp)) {
		ret = PTR_ERR(tmp);
		dev_err(gctl->dev, "Failed to create %s: %d\n", name, ret);
		return ret;
	}

	return 0;
}

int google_pinctrl_init_debugfs(struct google_pinctrl *gctl, struct platform_device *pdev,
				unsigned int num_groups)
{
	struct dentry **pinctl_de = &gctl->de;
	struct dentry *tmp;
	unsigned int success_cnt = 0;
	int ret = 0;

	atomic_set(&gctl->rpm_get_count, 0);
	atomic_set(&gctl->rpm_put_count, 0);
	atomic_set(&gctl->rpm_suspend_count, 0);
	atomic_set(&gctl->rpm_resume_count, 0);
	atomic_set(&gctl->system_suspend_count, 0);
	atomic_set(&gctl->system_resume_count, 0);

	if (fops_stats_en) {
		gctl->g_pingroups_fops_stats = devm_kzalloc(&pdev->dev,
				num_groups * sizeof(*gctl->g_pingroups_fops_stats), GFP_KERNEL);
	}

	*pinctl_de = debugfs_create_dir(dev_name(gctl->pctl->dev), NULL);
	if (IS_ERR_OR_NULL(*pinctl_de)) {
		dev_err(gctl->dev, "Failed to create debugfs\n");
		*pinctl_de = NULL;
		return -EIO;
	}

	success_cnt += !google_pinctrl_debugfs_create_file("dump-registers", 0400, *pinctl_de, gctl,
							   &dump_regs_fops);
	success_cnt += !google_pinctrl_debugfs_create_file("trigger-irq", 0200, *pinctl_de, gctl,
							   &trigger_irq_fops);
	success_cnt += !google_pinctrl_debugfs_create_file("rpm-stats", 0400, *pinctl_de, gctl,
							   &rpm_count_fops);

	if (gctl->g_pingroups_fops_stats) {
		success_cnt += !google_pinctrl_debugfs_create_file("fops-stats", 0400, *pinctl_de,
								   gctl, &fops_count_fops);
	}

	success_cnt += !google_pinctrl_debugfs_create_file("direction", 0200, *pinctl_de, gctl,
							   &debugfs_direction_fops);
	success_cnt += !google_pinctrl_debugfs_create_file("drive-strength", 0200, *pinctl_de, gctl,
							   &debugfs_drive_strength_fops);
	success_cnt += !google_pinctrl_debugfs_create_file("value", 0200, *pinctl_de, gctl,
							   &debugfs_value_fops);

	debugfs_create_bool("suspend-dump", 0600, *pinctl_de, &gctl->suspend_dump_enabled);
	tmp = debugfs_lookup("suspend-dump", *pinctl_de);
	if (IS_ERR_OR_NULL(tmp)) {
		ret = PTR_ERR(tmp);
		dev_err(gctl->dev, "Failed to create suspend-dump: %d\n", ret);
	} else {
		++success_cnt;
		dput(tmp);
	}

	success_cnt += !google_pinctrl_debugfs_create_file("pins", 0400, *pinctl_de, gctl,
							   &pins_fops);

	if (success_cnt == 0)
		debugfs_remove(*pinctl_de);

	return ret;
}

void google_pinctrl_remove_recursive_debugfs(struct google_pinctrl *gctl)
{
	debugfs_remove_recursive(gctl->de);
}

void google_pinctrl_debugfs_suspend_dump_regs(struct google_pinctrl *gctl)
{
	bool tmp_suspend_dump_enabled;
	unsigned long flags;
	int ret;

	atomic_inc(&gctl->system_suspend_count);

	raw_spin_lock_irqsave(&gctl->lock, flags);
	tmp_suspend_dump_enabled = gctl->suspend_dump_enabled;
	raw_spin_unlock_irqrestore(&gctl->lock, flags);

	if (tmp_suspend_dump_enabled) {
		ret = dump_regs_logs(gctl);
		if (ret < 0)
			dev_err(gctl->dev, "Failed to dump logs: %d\n", ret);
	}
}

int google_pinctrl_debugfs_inc_cnt(struct google_pinctrl *gctl,
				   enum PINCTRL_DEBUGFS_GCTL_CNT cnt_sel)
{
	switch (cnt_sel) {
	case RPM_GET_CNT:
		atomic_inc(&gctl->rpm_get_count);
		break;
	case RPM_PUT_CNT:
		atomic_inc(&gctl->rpm_put_count);
		break;
	case RPM_SUSP_CNT:
		atomic_inc(&gctl->rpm_suspend_count);
		break;
	case RPM_RESM_CNT:
		atomic_inc(&gctl->rpm_resume_count);
		break;
	case SYS_SUSP_CNT:
		atomic_inc(&gctl->system_suspend_count);
		break;
	case SYS_RESM_CNT:
		atomic_inc(&gctl->system_resume_count);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

int google_pinctrl_debugfs_inc_fops_cnt(struct google_pinctrl *gctl,
					enum PINCTRL_DEBUGFS_FOPS_CNT cnt_sel, unsigned int g_sel)
{
	if (!gctl->g_pingroups_fops_stats)
		return -EFAULT;

	switch (cnt_sel) {
	case DIR_I_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].dir_input_count);
		break;
	case DIR_O_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].dir_output_count);
		break;
	case DIR_GET_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].dir_get_count);
		break;
	case GET_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].get_count);
		break;
	case SET_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].set_count);
		break;
	case CFG_CNT:
		atomic_inc(&gctl->g_pingroups_fops_stats[g_sel].config_count);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
