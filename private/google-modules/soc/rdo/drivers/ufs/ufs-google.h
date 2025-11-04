/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Google LLC
 */
#ifndef _UFS_GOOGLE_H_
#define _UFS_GOOGLE_H_

#include <clk/clk-cpm.h>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/types.h>

#include <ap-pmic/da9188.h>
#include <ufs/ufshcd.h>
#include <ufs/ufshci.h>

#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
#include "ufs-pixel.h"
#endif

struct ufs_google_host;

struct cq_irq_desc {
	struct ufs_google_host *host;
	unsigned int qid;
};

#define MAXQ 16

struct ufs_dme_attr {
	u32 sel;
	u32 val;
	u8 peer;
};

struct mphy_reg {
	u16 addr;
	u16 data;
};

enum phy_patch_mode {
	PHY_ROM_MODE = 0,
	PHY_SRAM_MODE = 1,
	PHY_BOOTLD_BYPASS_MODE = 2,
};

/**
 * Platform-specific operations.
 */
struct ufs_google_ops {
	/* MPHY initialization */
	int (*get_phy_fw_patch)(struct ufs_google_host *host, const u32 **data,
				size_t *sz);
	int (*get_phy_rmmi_tx_eq_attrs)(struct ufs_google_host *host,
					const struct ufs_dme_attr **attrs,
					size_t *len);
	int (*get_phy_rmmi_attrs)(struct ufs_google_host *host,
				  const struct ufs_dme_attr **attrs,
				  size_t *len);
	int (*poll_phy_ready)(struct ufs_google_host *host);
};

struct ufs_google_host {
#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
	struct pixel_ufs pixel_ufs;
#endif
	struct ufs_google_ops *gops;
	struct ufs_google_dbg *dbg;
	struct ufs_pa_layer_attr google_pwr_mode;
	struct ufs_hba *hba;
	bool phy_enabled;
	bool multi_intr_enabled;
	bool calibration_needed;
	bool phy_patching_needed;
	enum phy_patch_mode phy_patch_mode;
	bool clkgate_delay_set;

	void __iomem *ufs_top_mmio;
	void __iomem *ufs_phy_sram_mmio;
	void __iomem *ufs_ss_mmio;
	struct ufs_vreg *vdd0p75;
	struct ufs_vreg *vdd1p2;
	struct reset_control *a_rst;
	struct reset_control *phy_rst;
	struct notifier_block top_nb;
	struct gpio_desc *resetb;
	struct gpio_desc *pwr_en;
	struct device *ufs_prep_pd;
	struct device *ufs_pd;
	struct cq_irq_desc cq_desc[MAXQ];
	struct pm_qos_request pm_qos_req;
	struct mutex indirect_reg_mutex;
	struct clk *refclk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *refclk_on_state;
	struct pinctrl_state *refclk_off_state;
	struct clk *hsios_aux_clk;
	struct mphy_reg *phy_cal_data;
	int phy_cal_size;
	bool pm_request_active;
	bool pm_set;

	/* dts properties */
	u64 caps;
	u32 ip_idle_index;
#if IS_ENABLED(CONFIG_GOOGLE_MBA_CPM_INTERFACE)
	struct pmic_mfd_mbox mbox;
	u32 pmic_ch;
#endif
};

enum google_host_cap {
	/* Features CAPs */
	GCAP_PHY_PATCHING = BIT(0),
	GCAP_HC_AH8_PG = BIT(1),
	GCAP_HC_SWH8_PG = BIT(2),
	GCAP_PHY_CAL = BIT(3), /* calibration */

	/* Resources CAPs - starts from bit 32 */
	GCAP_RSC_IP_IDLE = BIT(32),
	GCAP_RSC_UFS_EN_GPIO = BIT(33),
	GCAP_RSC_RSTN_GPIO = BIT(34),
	GCAP_RSC_REF_CLK = BIT(35),
	GCAP_RSC_AUX_CLK = BIT(36),
	GCAP_RSC_UFS_PD = BIT(37), /* power domain */
	GCAP_RSC_REF_CLK_PINCTRL = BIT(38),
};

static inline bool ufs_should_apply_phy_patch(struct ufs_google_host *host)
{
	return host->caps & GCAP_PHY_PATCHING && host->phy_patching_needed;
}

static inline bool ufs_should_apply_phy_cal(struct ufs_google_host *host)
{
	return host->caps & GCAP_PHY_CAL && host->calibration_needed;
}

#endif /* _UFS_GOOGLE_H_ */
