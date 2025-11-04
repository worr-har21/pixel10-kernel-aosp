/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2022 Google LLC
 */

#include <clk/clk-cpm.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/devm-helpers.h>
#include <linux/pcie_google_if.h>
#include <linux/pinctrl/consumer.h>
#include <linux/notifier.h>
#include <linux/irqdomain.h>
#include <interconnect/google_icc_helper.h>

#include "pcie-designware.h"

#define GPCIE_MAX_IRQ_NAME	64

#define GPCIE_INIT_NUM_RSTS 2

struct fw_patch_entry {
	u32 addr;
	u16 val;
};

extern const struct fw_patch_entry phy_fw_patch[];
extern const u32 phy_fw_patch_size;

enum link_states {
	L0 = 0,
	RECOVERY,
	L0S,
	L1,
	L11,
	L12,
	L2,
	L2V,
	UNKNOWN
};

enum link_duration_opcodes {
	LINK_DURATION_INIT,
	LINK_DURATION_RESET,
	LINK_DURATION_UP,
	LINK_DURATION_DOWN,
	LINK_DURATION_SPD_CHG,
	LINK_DURATION_OPCODE_MAX
};

struct google_pcie {
	struct device *dev;
	struct dw_pcie *pci;
	void __iomem *top_base;
	void __iomem *phy_sram_base;
	size_t phy_sram_size;
	size_t sii_size;
	void __iomem *sii_base;
	u32 curr_mapped_busdev;

	struct gpio_desc *perstn_gpio;
	unsigned int perst_delay_us;
	u8 max_link_speed;
	u8 max_link_width;
	u8 current_link_speed; /* in range [1-max speed] if link is up. 0 if link is down */
	u8 current_link_width; /* in range [1-max width] if link is up. 0 if link is down */
	u8 target_link_speed;
	u8 target_link_width;

	u8 link_timeout_ms;
	u16 aer;
	int domain;
	void *debugfs;
	struct pci_saved_state *saved_state;
	struct list_head node;
	bool enumeration_done;
	bool is_link_up;
	bool in_cpl_timeout;
	bool in_link_down;
	bool powered_on;
	bool l1_pwrgate_disable;
	bool skip_link_eq;
	bool allow_suspend_in_linkup;

	struct clk *phy_fw_clk;
	struct clk *aux_clk;

	struct notifier_block top_nb;

	struct reset_control *init_rst;
	struct reset_control *pwr_up_rst;
	struct reset_control *perst_rst;
	struct reset_control *app_hold_phy_rst;

	spinlock_t power_stats_lock;	/* Protect power_stats_show from update */
	spinlock_t power_on_lock;	/* Protect powered_on access */
	spinlock_t link_up_lock;	/* Protect EP access */
	spinlock_t link_duration_lock;	/* Protect link_duration_stats access */
	struct mutex link_lock;		/* Serialize link poweron and poweroff */

	struct google_pcie_power_stats link_up;
	struct google_pcie_power_stats link_down;
	struct google_pcie_link_duration_stats link_duration_stats;
	struct google_pcie_link_stats link_stats;

	struct workqueue_struct	*pcie_wq;
	struct delayed_work cpl_timeout_work;
	int cpl_timeout_irq;
	char cpl_timeout_irqname[GPCIE_MAX_IRQ_NAME];
	struct delayed_work link_down_work;
	int link_down_irq;
	char link_down_irqname[GPCIE_MAX_IRQ_NAME];
	google_pcie_callback_func cb_func;
	void *cb_priv;

	bool rescan_bus_on_linkup;
	bool remove_bus_on_linkdown;
	u32 l1ss;
	u32 aspm_pwr_on_val_us;
	u32 aspm_pwr_on_scale;
	u32 aspm_cmrt_us;
	u32 posted_rx_q_credits;

	struct google_icc_path *icc_path;
	u32 avg_bw;
	u32 peak_bw;
	struct cpumask msi_ctrl_to_cpu[MAX_MSI_CTRLS];
};

void google_pcie_assert_perst_n(struct google_pcie *gpcie, bool val);

#ifdef CONFIG_DEBUG_FS
int google_pcie_init_debugfs(struct google_pcie *gpcie);
void google_pcie_exit_debugfs(void *data);
#else
static int google_pcie_init_debugfs(struct google_pcie *gpcie) { return 0; }
static void google_pcie_exit_debugfs(void *data) {}
#endif
