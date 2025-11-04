/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Google LLC
 *
 * A supplemental module to the mainline debugfs module, containing helper tools
 */

#ifndef _PINCTRL_DEBUGFS_H_
#define _PINCTRL_DEBUGFS_H_

#include <linux/platform_device.h>

struct google_pinctrl;

/* 1 MiB file size limit */
#define PINCTRL_STRING_ATTRIBUTE_MAX_FILE_SIZE (BIT(20))

#define  DEFINE_PINCTRL_DEBUGFS_STRING_ATTRIBUTE(__fops, __read_callback, __write_callback)	\
static int __fops ## _open(struct inode *inode, struct file *file)	\
{	\
	return pinctrl_string_attr_open(inode, file, __read_callback, __write_callback);	\
}	\
static const struct file_operations __fops ## _fops = {	\
	.owner = THIS_MODULE,	\
	.open = __fops ## _open,	\
	.release = pinctrl_string_attr_release,	\
	.read = pinctrl_debugfs_string_attr_read,	\
	.write = pinctrl_debugfs_string_attr_write,	\
	.llseek = no_llseek,	\
}

struct pinctrl_debugfs_attr_file {
	char *buf;
	unsigned int count;
	unsigned int size;
};

struct pinctrl_char_buf {
	char *buf;
	unsigned int size;
};

/**
 * struct fops_stats - Counts function calls towards GPIO fops API
 * @dir_input_count: Counter of GPIO direction input changes
 * @dir_output_count: Counter of GPIO direction output changes
 * @dir_get_count: Counter of GPIO direction get() function calls
 * @get_count: Counter of GPIO get() function calls
 * @set_count: Counter of GPIO set() function calls
 * @config_count: Counter of gpio config set function calls
 */
struct fops_stats {
	atomic_t dir_input_count;
	atomic_t dir_output_count;
	atomic_t dir_get_count;
	atomic_t get_count;
	atomic_t set_count;
	atomic_t config_count;
};

int dump_regs_logs(struct google_pinctrl *gctl);

enum PINCTRL_DEBUGFS_GCTL_CNT {
	RPM_GET_CNT,
	RPM_PUT_CNT,
	RPM_SUSP_CNT,
	RPM_RESM_CNT,
	SYS_SUSP_CNT,
	SYS_RESM_CNT
};

enum PINCTRL_DEBUGFS_FOPS_CNT {
	DIR_I_CNT,
	DIR_O_CNT,
	DIR_GET_CNT,
	GET_CNT,
	SET_CNT,
	CFG_CNT
};

#if IS_ENABLED(CONFIG_DEBUG_FS)

int pinctrl_string_attr_open(struct inode *inode, struct file *file,
			int (*read_callback)(void *, struct pinctrl_debugfs_attr_file *),
			int (*write_callback)(void *, const struct pinctrl_char_buf *));
int pinctrl_string_attr_release(struct inode *inode, struct file *file);
ssize_t pinctrl_string_attr_read(struct file *file, char __user *buf,
			 size_t len, loff_t *ppos);
ssize_t pinctrl_string_attr_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos);
ssize_t pinctrl_debugfs_string_attr_read(struct file *file, char __user *buf,
			size_t len, loff_t *ppos);
ssize_t pinctrl_debugfs_string_attr_write(struct file *file, const char __user *buf,
			size_t len, loff_t *ppos);

void pinctrl_debugfs_attr_file_puts(struct pinctrl_debugfs_attr_file *file, const char *s);
void pinctrl_debugfs_attr_file_printf(struct pinctrl_debugfs_attr_file *file, const char *fmt, ...);

int google_pinctrl_init_debugfs(struct google_pinctrl *gctl, struct platform_device *pdev,
				unsigned int num_groups);
void google_pinctrl_remove_recursive_debugfs(struct google_pinctrl *gctl);

void google_pinctrl_debugfs_suspend_dump_regs(struct google_pinctrl *gctl);
int google_pinctrl_debugfs_inc_cnt(struct google_pinctrl *gctl,
				   enum PINCTRL_DEBUGFS_GCTL_CNT cnt_sel);
int google_pinctrl_debugfs_inc_fops_cnt(struct google_pinctrl *gctl,
					enum PINCTRL_DEBUGFS_FOPS_CNT cnt_sel, unsigned int g_sel);

#else

#include <linux/err.h>

/*
 * We do not return NULL from these functions if CONFIG_DEBUG_FS is not enabled
 * so users have a chance to detect if there was a real error or not.  We don't
 * want to duplicate the design decision mistakes of procfs and devfs again.
 */

static inline ssize_t pinctrl_string_attr_read(struct file *file, char __user *buf,
			 size_t len, loff_t *ppos)
{
	return -ENODEV;
}

static inline ssize_t pinctrl_string_attr_write(struct file *file, const char __user *buf,
				size_t len, loff_t *ppos)
{
	return -ENODEV;
}

static inline ssize_t pinctrl_debugfs_string_attr_read(struct file *file, char __user *buf,
			size_t len, loff_t *ppos)
{
	return -ENODEV;
}

static inline ssize_t pinctrl_debugfs_string_attr_write(struct file *file, const char __user *buf,
			size_t len, loff_t *ppos)
{
	return -ENODEV;
}

static inline int google_pinctrl_init_debugfs(struct google_pinctrl *gctl,
					      struct platform_device *pdev, unsigned int num_groups)
{
	return -ENODEV;
}

static inline void google_pinctrl_remove_recursive_debugfs(struct google_pinctrl *gctl)
{
	return -ENODEV;
}

static inline void google_pinctrl_debugfs_suspend_dump_regs(struct google_pinctrl *gctl)
{
	return -ENODEV;
}

static inline int google_pinctrl_debugfs_inc_cnt(struct google_pinctrl *gctl,
						 enum PINCTRL_DEBUGFS_GCTL_CNT cnt_sel)
{
	return -ENODEV;
}

static inline int google_pinctrl_debugfs_inc_fops_cnt(struct google_pinctrl *gctl,
						      enum PINCTRL_DEBUGFS_FOPS_CNT cnt_sel,
						      unsigned int g_sel)
{
	return -ENODEV;
}

#endif /* CONFIG_DEBUG_FS */

#endif /* _PINCTRL_DEBUGFS_H_ */
