// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2021 Google LLC
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <host/ufshcd-pltfrm.h>
#include <core/ufshcd-priv.h>
#include <ip-idle-notifier/google_ip_idle_notifier.h>
#include <mailbox/protocols/mba/cpm/common/pmic/pmic_service.h>
#include "ufs-google.h"
#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
#include "ufs-pixel.h"
#endif
#include "ufs-google-dbg.h"
#include "ufs-google-platform.h"
#include "ufs-pixel-crypto.h"

static bool block_c4;
module_param(block_c4, bool, 0444);
MODULE_PARM_DESC(block_c4,
		 "Block C4 CPU state while there are outstanding I/O");

#define MCQ_CFG_n(r, i)		((r) + MCQ_QCFG_SIZE * (i))
#define MCQ_OPR_OFFSET_n(p, i)	\
	(hba->mcq_opr[(p)].offset + hba->mcq_opr[(p)].stride * (i))

#define FIPS_140_TIMEOUT_US 400
#define FIPS_140_DELAY_US 10
#define PHY_STATUS_TIMEOUT_US 570000
#define PHY_STATUS_DELAY_US 190000
#define UIC_COMMAND_POLL_TIMEOUT_US 4000000
#define UIC_COMMAND_DELAY_US 10

#define PWR_EN_DELAY_MIN_US 1500
#define PWR_EN_DELAY_MAX_US 2000
#define REFCLK_DELAY_MIN_US 300
#define REFCLK_DELAY_MAX_US 310
#define CLKGATE_DELAY_MS 4
#define GOOGLE_RPM_AUTOSUSPEND_DELAY_MS 1000

#define MAX_UFS_HOSTS 2
static struct ufs_google_host *ufs_host_backup[MAX_UFS_HOSTS];
static atomic_t ufs_host_index;

static inline u32 ufs_top_csr_readl(struct ufs_google_host *host, u32 reg)
{
	return readl(host->ufs_top_mmio + reg);
}

static inline void ufs_top_csr_writel(struct ufs_google_host *host, u32 val,
				      u32 reg)
{
	writel(val, host->ufs_top_mmio + reg);
}

static inline void ufs_auto_hibern8_update(struct ufs_hba *hba, bool on)
{
	if (!ufshcd_is_auto_hibern8_supported(hba))
		return;

	ufshcd_writel(hba, on ? hba->ahit : 0, REG_AUTO_HIBERNATE_IDLE_TIMER);
}

#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
static int ufs_google_mphy_indirect_reg_read(struct ufs_hba *hba,
					     struct mphy_reg *reg);
static int ufs_google_get_phy_register(struct ufs_hba *hba,
				       struct mphy_reg *reg)
{
	int ret;

	/*
	 * Reading the phy version registers requires the devices to be active
	 * and the link not to be in hibern8 state. Add a small delay to make
	 * sure all transitions completed.
	 */
	ufshcd_rpm_get_sync(hba);
	ufshcd_hold(hba);
	ufs_auto_hibern8_update(hba, false);
	mdelay(10);

	ret = ufs_google_mphy_indirect_reg_read(hba, reg);

	ufs_auto_hibern8_update(hba, true);
	ufshcd_release(hba);
	ufshcd_rpm_put_sync(hba);

	return ret;
}

static char *ufs_google_get_phy_version(struct ufs_hba *hba)
{
	char *phy_version = NULL;
	int ret;
	struct mphy_reg reg = {
		.addr = MPHY_FIRMWARE_VER_REG0,
	};

	ret = ufs_google_get_phy_register(hba, &reg);
	if (!ret)
		phy_version = kasprintf(GFP_KERNEL, "%u.%u.%u",
					(reg.data >> 12) & 0xF,
					(reg.data >> 4) & 0xFF, reg.data & 0xF);

	return phy_version;
}

static char *ufs_google_get_phy_release_date(struct ufs_hba *hba)
{
	char *phy_release_date = NULL;
	int ret;
	struct mphy_reg reg = {
		.addr = MPHY_FIRMWARE_VER_REG1,
	};

	ret = ufs_google_get_phy_register(hba, &reg);
	if (!ret)
		phy_release_date = kasprintf(GFP_KERNEL, "%u/%02u/%02u",
					     (reg.data & 0x7) + 2018,
					     (reg.data >> 3) & 0xF,
					     (reg.data >> 7) & 0x1F);

	return phy_release_date;
}

struct pixel_ufs *to_pixel_ufs(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);

	return &host->pixel_ufs;
}

static int pixel_crypto_init(struct ufs_hba *hba)
{
	// TODO: fill out similar to exynos_crypto_init;
	return 0;
}

static int ufs_google_set_pwr_mode(struct ufs_hba *hba,
				   const struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);

	if (pwr_mode->gear_rx != pwr_mode->gear_tx ||
	    pwr_mode->gear_rx > UFS_HS_G5) {
		dev_err(hba->dev, "unsupported gears rx=%u tx=%u\n",
			pwr_mode->gear_rx, pwr_mode->gear_tx);
		return -EINVAL;
	}

	if (pwr_mode->lane_rx != pwr_mode->lane_tx ||
	    pwr_mode->lane_rx > UFS_LANE_2) {
		dev_err(hba->dev, "unsupported lanes rx=%u tx=%u\n",
			pwr_mode->lane_rx, pwr_mode->lane_tx);
		return -EINVAL;
	}

	/*
	 * We do not support PWM gear, and ufshcd_get_pwr_dev_param() only
	 * considers FAST_MODE for hs support. Make only FAST and UNCHANGED
	 * valid.
	 */
	if (pwr_mode->pwr_rx != pwr_mode->pwr_tx ||
	    (pwr_mode->pwr_rx != FAST_MODE && pwr_mode->pwr_rx != UNCHANGED)) {
		dev_err(hba->dev, "unsupported power mode rx=%u tx=%u\n",
			pwr_mode->pwr_rx, pwr_mode->pwr_tx);
		return -EINVAL;
	}

	/* We only support HS Rate B, as we do not have Rate A calibration */
	if (pwr_mode->hs_rate != PA_HS_MODE_B) {
		dev_err(hba->dev, "unsupported hs rate=%u\n",
			pwr_mode->hs_rate);
		return -EINVAL;
	}

	memcpy(&host->google_pwr_mode, pwr_mode, sizeof(*pwr_mode));

	return 0;
}

static void ufs_google_get_pwr_mode(struct ufs_hba *hba,
				    struct ufs_pa_layer_attr *pwr_mode)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);

	memcpy(pwr_mode, &host->google_pwr_mode, sizeof(*pwr_mode));
}

static const struct pixel_ops pixel_ops = {
	.crypto_init = pixel_crypto_init,
	.set_pwr_mode = ufs_google_set_pwr_mode,
	.get_pwr_mode = ufs_google_get_pwr_mode,
	.get_phy_version = ufs_google_get_phy_version,
	.get_phy_release_date = ufs_google_get_phy_release_date,
};
#endif

static int ufs_google_phy_setup_vreg(struct ufs_hba *hba, bool on);
static int ufs_google_pd_notifier(struct notifier_block *nb,
				  unsigned long action, void *data);

static int ufs_google_toggle_vreg(struct ufs_hba *hba, struct ufs_vreg *vreg,
				  bool on)
{
	int ret;

	if (!vreg || vreg->enabled == on || vreg->always_on)
		return 0;

	ret = on ? regulator_enable(vreg->reg) : regulator_disable(vreg->reg);
	if (ret) {
		dev_err(hba->dev, "vreg %s toogle %s failed (%d)", vreg->name,
			on ? "on" : "off", ret);
		return ret;
	}

	vreg->enabled = on;

	return 0;
}

static int ufs_google_init_vreg(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct ufs_google_host *host;
	struct regulator *reg;
	int ret;

	host = ufshcd_get_variant(hba);

	ret = ufshcd_populate_vreg(hba->dev, "vdd0p75", &host->vdd0p75);
	if (ret)
		return ret;

	ret = ufshcd_populate_vreg(hba->dev, "vdd1p2", &host->vdd1p2);
	if (ret)
		return ret;

	reg = devm_regulator_get(dev, "vdd0p75");
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		dev_err(dev, "Failed to get 0.75V PHY regulator(%d)\n", ret);
		return ret;
	}
	host->vdd0p75->reg = reg;

	reg = devm_regulator_get(dev, "vdd1p2");
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		dev_err(dev, "Failed to get 1.2V PHY regulator(%d)\n", ret);
		return ret;
	}
	host->vdd1p2->reg = reg;

	return 0;
}

#if IS_ENABLED(CONFIG_GOOGLE_MBA_CPM_INTERFACE)
#define SEQ_BUCK11S_8_ADDR 0x1857
enum IRQ_TYPE {
	CORE_MAIN_PMIC,
	CORE_SUB_PMIC,
	IF_PMIC,
};

static int ufs_google_init_cpm(struct ufs_hba *hba)
{
	struct ufs_google_host *host;
	struct mailbox_data req_data;
	int pmic = CORE_SUB_PMIC;
	int ret;

	host = ufshcd_get_variant(hba);

	if (!of_property_read_bool(hba->dev->of_node, "google,cpm-override"))
		return 0;

	ret = of_property_read_u32(hba->dev->of_node, "mba-pmic-dest-channel",
				   &host->pmic_ch);
	if (ret < 0) {
		dev_err(hba->dev, "%s: failed to read mba-pmic-dest-channel",
			__func__);
		return ret;
	}

	ret = da9188_mfd_mbox_request(hba->dev, &host->mbox);
	if (ret) {
		dev_err(hba->dev, "%s: request pmic mbox client failed (%d)",
			__func__, ret);
		return ret;
	}

	req_data.data[0] = pmic;
	req_data.data[1] = 0;

	ret = da9188_mfd_mbox_send_req_blocking(hba->dev, &host->mbox,
						host->pmic_ch,
						MB_PMIC_TARGET_REGISTER,
						MB_REG_CMD_SET_PMIC_REG_SINGLE,
						SEQ_BUCK11S_8_ADDR, req_data);
	if (ret) {
		dev_err(hba->dev, "%s: send mbox req failed (%d)", __func__,
			ret);
		return ret;
	}

	return 0;
}
#endif

static int ufs_google_attach_power_domains(struct ufs_google_host *host)
{
	int err;
	struct ufs_hba *hba = host->hba;

	host->ufs_prep_pd =
		dev_pm_domain_attach_by_name(hba->dev, "ufs_prep_pd");
	if (IS_ERR(host->ufs_prep_pd)) {
		err = PTR_ERR(host->ufs_prep_pd);
		host->ufs_prep_pd = NULL;
		return err;
	}

	host->ufs_pd = dev_pm_domain_attach_by_name(hba->dev, "ufs_pd");
	if (IS_ERR(host->ufs_pd)) {
		dev_pm_domain_detach(host->ufs_prep_pd, true);
		err = PTR_ERR(host->ufs_pd);
		host->ufs_pd = NULL;
		return err;
	}

	return 0;
}

static inline void ufs_google_pd_get_sync(struct ufs_google_host *host)
{
	if (!(host->caps & GCAP_RSC_UFS_PD))
		return;

	pm_runtime_get_sync(host->ufs_prep_pd);
	pm_runtime_get_sync(host->ufs_pd);
}

static inline void ufs_google_pd_put_sync(struct ufs_google_host *host)
{
	if (!(host->caps & GCAP_RSC_UFS_PD))
		return;

	pm_runtime_put_sync(host->ufs_pd);
	pm_runtime_put_sync(host->ufs_prep_pd);
}

static void ufs_google_detach_power_domains(struct ufs_google_host *host)
{
	if (!(host->caps & GCAP_RSC_UFS_PD))
		return;

	pm_runtime_disable(host->ufs_pd);
	dev_pm_domain_detach(host->ufs_pd, true);

	pm_runtime_disable(host->ufs_prep_pd);
	dev_pm_domain_detach(host->ufs_prep_pd, true);
}

static void ufs_google_init_caps(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int i;

	struct dts_cap {
		const char *prop_name;
		u64 cap;
	};

	static const struct dts_cap dts_caps[] = {
		{ "google,enable-phy-patching", GCAP_PHY_PATCHING },
		{ "google,enable-hc-ah8-pg", GCAP_HC_AH8_PG },
		{ "google,enable-hc-swh8-pg", GCAP_HC_SWH8_PG },
		{ "google,enable-phy-calibration", GCAP_PHY_CAL },
	};

	for (i = 0; i < ARRAY_SIZE(dts_caps); i++) {
		if (of_property_read_bool(hba->dev->of_node,
					  dts_caps[i].prop_name))
			host->caps |= dts_caps[i].cap;
	}
}

static int ufs_google_init(struct ufs_hba *hba)
{
	int err = 0;
	struct device *dev = hba->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct ufs_google_host *host;
	u32 ah8_timer_scale;
	u32 ah8_idle_timer_value;
	u32 ahit = 0;
	int id;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host) {
		err = -ENOMEM;
		goto out;
	}

	/* two way bind between variant and hba */
	host->hba = hba;
	ufshcd_set_variant(hba, host);

#if IS_ENABLED(CONFIG_GOOGLE_MBA_CPM_INTERFACE)
	err = ufs_google_init_cpm(hba);
	if (err) {
		// Non-fatal
		dev_err(hba->dev, "%s: failed to init ufs cpm, error code: %d",
			__func__, err);
	}
#endif

	host->ufs_top_mmio = devm_platform_ioremap_resource_byname(pdev, "ufs_top");
	if (IS_ERR(host->ufs_top_mmio)) {
		err = PTR_ERR(host->ufs_top_mmio);
		dev_err(dev, "%s: failed to init ufs_top mmio, error code: %d\n",
			__func__, err);
		goto out_unset;
	}

	host->ufs_phy_sram_mmio =
		devm_platform_ioremap_resource_byname(pdev, "ufs_phy_sram");
	if (IS_ERR(host->ufs_phy_sram_mmio)) {
		err = PTR_ERR(host->ufs_phy_sram_mmio);
		dev_err(dev,
			"%s: failed to init ufs_phy_sram mmio, error code: %d\n",
			__func__, err);
		goto out_unset;
	}

	host->ufs_ss_mmio =
		devm_platform_ioremap_resource_byname(pdev, "ufs_ss");
	if (IS_ERR(host->ufs_ss_mmio)) {
		err = PTR_ERR(host->ufs_ss_mmio);
		dev_err(dev,
			"%s: failed to init ufs_ss mmio, error code: %d. skip\n",
			__func__, err);
		host->ufs_ss_mmio = NULL;
	}

	err = ufs_google_init_vreg(hba);
	if (err) {
		dev_err(dev, "vreg init failed, error code: %d", err);
		goto out_unset;
	}

	host->phy_rst = devm_reset_control_get(dev, "phy_rst");
	if (IS_ERR(host->phy_rst)) {
		err = PTR_ERR(host->phy_rst);
		goto out_unset;
	}

	/* config pin as refclk */
	host->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(host->pinctrl)) {
		dev_err(dev, "refclk pinctrl get fail.");
		goto out_unset;
	}
	host->refclk_on_state = pinctrl_lookup_state(host->pinctrl, "refclk_on");
	if (IS_ERR(host->refclk_on_state))
		dev_err(dev, "refclk pinctrl get on state fail. skip.");

	host->refclk_off_state = pinctrl_lookup_state(host->pinctrl, "refclk_off");
	if (IS_ERR(host->refclk_off_state))
		dev_err(dev, "refclk pinctrl get off state fail. skip.");

	if (!IS_ERR(host->refclk_on_state) && !IS_ERR(host->refclk_off_state))
		host->caps |= GCAP_RSC_REF_CLK_PINCTRL;

	ufs_google_phy_setup_vreg(hba, true);

	if (!of_property_read_u32(pdev->dev.of_node, "ah8-timer-scale",
				  &ah8_timer_scale) &&
	    !of_property_read_u32(pdev->dev.of_node, "ah8-idle-timer-value",
				  &ah8_idle_timer_value)) {
		ahit = FIELD_PREP(UFSHCI_AHIBERN8_TIMER_MASK,
				  ah8_idle_timer_value) |
		       FIELD_PREP(UFSHCI_AHIBERN8_SCALE_MASK, ah8_timer_scale);
	}

	err = of_property_read_u32(pdev->dev.of_node, "ip-idle-index",
				   &host->ip_idle_index);
	if (err)
		dev_err(dev, "failed to acquire ip-idle-index value. skip\n");
	else
		host->caps |= GCAP_RSC_IP_IDLE;

	host->phy_cal_size = of_property_count_elems_of_size(pdev->dev.of_node,
							     "phy-cal-data",
							     sizeof(u32));
	if (host->phy_cal_size < 0) {
		err = host->phy_cal_size;
		dev_err(dev, "failed to obtain phy cal size (%d)\n", err);
		goto out_unset;
	}

	host->phy_cal_data =
		devm_kzalloc(dev,
			     sizeof(*host->phy_cal_data) * host->phy_cal_size,
			     GFP_KERNEL);
	if (!host->phy_cal_data) {
		err = -ENOMEM;
		goto out_unset;
	}

	err = of_property_read_u32_array(pdev->dev.of_node, "phy-cal-data",
					 (uint32_t *)host->phy_cal_data,
					 host->phy_cal_size);
	if (err) {
		dev_err(dev, "failed to read 'phy-cal-data' array (%d)\n", err);
		goto out_unset;
	}

	host->pwr_en = devm_gpiod_get_optional(dev, "power", GPIOD_OUT_HIGH);
	if (IS_ERR(host->pwr_en)) {
		err = PTR_ERR(host->pwr_en);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "failed to acquire power gpio: %d. skip\n",
				err);
	} else {
		host->caps |= GCAP_RSC_UFS_EN_GPIO;
	}

	host->refclk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(host->refclk)) {
		dev_err(dev, "failed to acquire refclk: %ld. skip\n",
			PTR_ERR(host->refclk));
		host->refclk = NULL;
	} else {
		host->caps |= GCAP_RSC_REF_CLK;
		clk_prepare_enable(host->refclk);
		usleep_range(REFCLK_DELAY_MIN_US, REFCLK_DELAY_MAX_US);
	}

	host->resetb = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(host->resetb)) {
		err = PTR_ERR(host->resetb);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "failed to acquire reset gpio: %d\n", err);
	} else {
		host->caps |= GCAP_RSC_RSTN_GPIO;
	}

	host->hsios_aux_clk = devm_clk_get(dev, "aux_clk");
	if (IS_ERR(host->hsios_aux_clk)) {
		dev_err(dev, "failed to acquire hsios_aux_clk: %ld. skip\n",
			PTR_ERR(host->hsios_aux_clk));
		host->hsios_aux_clk = NULL;
	} else {
		host->caps |= GCAP_RSC_AUX_CLK;
	}

	err = ufs_google_attach_power_domains(host);
	if (err) {
		dev_err(dev,
			"%s: failed to attach power domains, error code: %d. skip\n",
			__func__, err);
	} else {
		host->caps |= GCAP_RSC_UFS_PD;
	}

	ufs_google_init_caps(hba);
	dev_info(hba->dev, "google caps=%64pbl", &host->caps);

	if (host->caps & GCAP_PHY_PATCHING) {
		/* if google,enable-phy-patching is set, further read patching mode */
		u32 patch_mode;

		err = of_property_read_u32(hba->dev->of_node,
					   "google,phy-patching-mode",
					   &patch_mode);
		if (err) {
			dev_err(hba->dev, "invalid phy patching mode %d", err);
			goto out_unset;
		}
		host->phy_patch_mode = patch_mode;
	} else {
		/* default use ROM mode */
		host->phy_patch_mode = PHY_ROM_MODE;
	}
	dev_info(hba->dev, "phy patching mode=%d", host->phy_patch_mode);

	ufs_google_plat_set_gops(host);
	if (!host->gops) {
		dev_err(hba->dev, "gops not set\n");
		goto out_unset;
	}

	/*
	 * Turning ufs_pd ON changes PSM state from PREP to ON. So the setup required in
	 * PREP state is done with PRE_ON of ufs_pd notifier.
	 * ufs_prep_pd which changes PSM state from OFF to PREP does not need a notifier.
	 */
	host->top_nb.notifier_call = ufs_google_pd_notifier;
	dev_pm_genpd_add_notifier(host->ufs_pd, &host->top_nb);

	/*
	 * Move PSM from OFF to PREP and then PREP to ON.
	 * If the PSM is already in PREP state, ufs_prep_pd turn ON request
	 * to CPM will have no effect.
	 */
	ufs_google_pd_get_sync(host);

	hba->android_quirks |= UFSHCD_ANDROID_QUIRK_36BIT_ADDRESS_DMA;
#if IS_ENABLED(CONFIG_SOC_LGA)
	hba->android_quirks |= UFSHCD_ANDROID_QUIRK_SET_IID_TO_ONE;
#endif
	hba->caps |= UFSHCD_CAP_CLK_GATING;
	hba->caps |= UFSHCD_CAP_HIBERN8_WITH_CLK_GATING;

	if (ahit)
		hba->ahit = ahit;
	else
		hba->quirks |= UFSHCD_QUIRK_BROKEN_AUTO_HIBERN8;

	hba->caps |= UFSHCD_CAP_RPM_AUTOSUSPEND;
	hba->host->rpm_autosuspend_delay = GOOGLE_RPM_AUTOSUSPEND_DELAY_MS;

	/* store ufs host symbol for ramdump analysis */
	id = atomic_inc_return(&ufs_host_index) - 1;
	if (id < MAX_UFS_HOSTS)
		ufs_host_backup[id] = host;

	dev_info(dev, "block_c4=%s\n", block_c4 ? "true" : "false");

	/* configure desired power parameters */
	host->google_pwr_mode = (struct ufs_pa_layer_attr){
		.gear_rx = UFS_HS_G5,
		.gear_tx = UFS_HS_G5,
		.lane_rx = UFS_LANE_2,
		.lane_tx = UFS_LANE_2,
		.pwr_rx = FAST_MODE,
		.pwr_tx = FAST_MODE,
		.hs_rate = PA_HS_MODE_B,
	};

	mutex_init(&host->indirect_reg_mutex);

	ufs_google_init_dbg(hba);

#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
	err = pixel_init(hba, dev, &pixel_ops);

	if (err)
		ufs_google_detach_power_domains(host);
	return err;
#else
	return 0;
#endif

out_unset:
	ufshcd_set_variant(hba, NULL);
out:
	return err;
}

static void ufs_google_select_mphy_fw_mode(struct ufs_google_host *host)
{
	u32 data;

	data = ufs_top_csr_readl(host, REG_HSIOS_UFS_PWR_CTRL);
	if (ufs_should_apply_phy_patch(host)) {
		if (host->phy_patch_mode == PHY_BOOTLD_BYPASS_MODE) {
			data &= ~UFS_SRAM_BYPASS_MASK;
			data |= UFS_SRAM_BOOTLOAD_BYPASS_MASK;
		} else {
			dev_err(host->hba->dev, "invalid mode %d",
				host->phy_patch_mode);
		}
	} else {
		/* Use PHY_ROM_MODE */
		data |= UFS_SRAM_BYPASS_MASK;
		data |= UFS_SRAM_BOOTLOAD_BYPASS_MASK;
	}
	ufs_top_csr_writel(host, data, REG_HSIOS_UFS_PWR_CTRL);
}

/**
 * ufs_google_pd_notifier - UFS Power domain notifier
 * Handle `hsio_s_ufs_pd` power on and off callback.
 * We assume the power domain is in ON state when enter kernel.
 */
static int ufs_google_pd_notifier(struct notifier_block *nb,
				  unsigned long action, void *dat)
{
	struct ufs_google_host *host = container_of(nb, struct ufs_google_host,
						    top_nb);
	u32 data, external_mode, rext;
	struct ufs_hba *hba = host->hba;
	u32 intr_status;
	int err = 0;

	switch (action) {
	case GENPD_NOTIFY_PRE_ON:
		host->calibration_needed = true;
		host->phy_patching_needed = true;

		ufs_google_select_mphy_fw_mode(host);

		data = ufs_top_csr_readl(host, REG_HSIOS_UFS_PHYCLK_SEL);
		data |= REF_CLK_SEL_MASK & REF_CLK_SEL;
		ufs_top_csr_writel(host, data, REG_HSIOS_UFS_PHYCLK_SEL);

		data = ufs_top_csr_readl(host, REG_HSIOS_UFS_MPHY_CFG);
		data |= UFS_MPHY_CFG_MASK & UFS_MPHY_CFG;
		ufs_top_csr_writel(host, data, REG_HSIOS_UFS_MPHY_CFG);

		data = ufs_top_csr_readl(host, REG_HSIOS_UFS_CFG_CLKSEL);
		data |= CFG_CLK_SEL_MASK & CFG_CLK_SEL;
		ufs_top_csr_writel(host, data, REG_HSIOS_UFS_CFG_CLKSEL);

		/* ufs_setup_rext */
		/* TODO(b/313024923): read external_mode, rext from OTP */
		external_mode = 1;
		rext = 0;
		data = ufs_top_csr_readl(host, REG_HSIOS_UFS_REXT_CTRL);
		if (external_mode) {
			data &= ~UFS_REXT_EN_MASK;
		} else {
			data |= FIELD_PREP(UFS_REXT_CONTROL_MASK, rext);
			data |= UFS_REXT_EN_MASK;
		}
		ufs_top_csr_writel(host, data, REG_HSIOS_UFS_REXT_CTRL);

		break;
	case GENPD_NOTIFY_ON:
		/* ufs_wait_for_fips_140 */
		err = readl_poll_timeout(host->ufs_top_mmio +
						 REG_HSIOS_UFS_ENC_STAT,
					 data, data & CRYPTO_KAT_STAT_MASK,
					 FIPS_140_DELAY_US,
					 FIPS_140_TIMEOUT_US);
		if (err)
			return err;

		/* reconfigure pin as refclk */
		if (host->caps & GCAP_RSC_REF_CLK_PINCTRL) {
			err = pinctrl_select_state(host->pinctrl,
						   host->refclk_on_state);
			if (err < 0) {
				dev_err(hba->dev, "pin select state fail, %d\n",
					err);
				return err;
			}
		}

		/* 2h. reset = 0 */
		if (host->caps & GCAP_RSC_RSTN_GPIO)
			gpiod_set_value(host->resetb, 0);

		/* 2h. enable vccq */
		if (host->caps & GCAP_RSC_UFS_EN_GPIO)
			gpiod_set_value(host->pwr_en, 1);
		usleep_range(PWR_EN_DELAY_MIN_US, PWR_EN_DELAY_MAX_US);

		/* 2h. drive refclk */
		if (host->caps & GCAP_RSC_REF_CLK)
			clk_prepare_enable(host->refclk);
		usleep_range(REFCLK_DELAY_MIN_US, REFCLK_DELAY_MAX_US);

		/* 2h. reset = 1 */
		if (host->caps & GCAP_RSC_RSTN_GPIO)
			gpiod_set_value(host->resetb, 1);

		intr_status = ufshcd_readl(hba, REG_IS);
		intr_status &= VS_INTERRUPT_MASK;
		ufshcd_writel(hba, intr_status, REG_IS);

		break;
	case GENPD_NOTIFY_OFF:
		if (host->caps & GCAP_RSC_REF_CLK)
			clk_disable_unprepare(host->refclk);
		usleep_range(10, 12);

		if (host->caps & GCAP_RSC_RSTN_GPIO)
			gpiod_set_value(host->resetb, 0);

		/* Set UFS_EN to 0 to disable vcc/vccq */
		if (host->caps & GCAP_RSC_UFS_EN_GPIO)
			gpiod_set_value(host->pwr_en, 0);

		if (host->caps & GCAP_RSC_REF_CLK_PINCTRL) {
			err = pinctrl_select_state(host->pinctrl,
						   host->refclk_off_state);
			if (err < 0) {
				dev_err(hba->dev, "pin select state fail, %d\n",
					err);
				return err;
			}
		}

		host->calibration_needed = true;
		host->phy_patching_needed = true;

		break;
	default:
		break;
	}

	return err;
}

/**
 * ufshcd_google_dme_get_layer_enable - wrapper for getting DME_VS_LAYER_ENABLE,
 * used as @op in readx_poll_timeout
 * @hba: host controller instance
 */
static inline u32 ufshcd_google_dme_get_layer_enable(struct ufs_hba *hba)
{
	u32 data;

	ufshcd_dme_get(hba, UIC_ARG_MIB(UNIPRO_DME_LAYER_EN), &data);
	return data;
}

/**
 * ufshcd_google_dme_set_seq - set a sequence of DME attributes
 * @hba: host controller instance
 * @arr: array of dme attributes to be set
 * @num: size of @arr
 */
static int ufshcd_google_dme_set_seq(struct ufs_hba *hba,
				     const struct ufs_dme_attr *arr, int num)
{
	int i, err;

	for (i = 0; i < num; ++i) {
		err = ufshcd_dme_set_attr(hba, arr[i].sel, ATTR_SET_NOR,
					  arr[i].val, arr[i].peer);
		if (err)
			return err;
	}

	return 0;
}

static int ufs_google_wait_for_uic_cmd(struct ufs_hba *hba)
{
	u32 val;

	if (read_poll_timeout(ufshcd_readl, val, val & UIC_COMMAND_COMPL,
				    UIC_COMMAND_DELAY_US,
				    UIC_COMMAND_POLL_TIMEOUT_US, false, hba,
				    REG_INTERRUPT_STATUS)) {
		dev_err(hba->dev, "uic completion timeout val=%08x\n", val);
		return -ETIMEDOUT;
	}

	ufshcd_writel(hba, UIC_COMMAND_COMPL, REG_INTERRUPT_STATUS);

	return 0;
}

static int ufs_google_send_uic_cmd(struct ufs_hba *hba,
				   struct uic_command *uic_cmd)
{
	int ret = -ETIMEDOUT;

	ufshcd_hold(hba);
	mutex_lock(&hba->uic_cmd_mutex);

	/* Write Args */
	ufshcd_writel(hba, uic_cmd->argument1, REG_UIC_COMMAND_ARG_1);
	ufshcd_writel(hba, uic_cmd->argument2, REG_UIC_COMMAND_ARG_2);
	ufshcd_writel(hba, uic_cmd->argument3, REG_UIC_COMMAND_ARG_3);

	/* Write UIC Cmd */
	ufshcd_writel(hba, uic_cmd->command & COMMAND_OPCODE_MASK,
		      REG_UIC_COMMAND);

	if (!ufs_google_wait_for_uic_cmd(hba)) {
		/*
		 * The UICCMDARG2 register contains the ConfigResultCode in the
		 * lower 8 bits. A value 0 indicates success and all others
		 * indicate failure. See JESD223E 5.6.3 for additional details.
		 */
		uic_cmd->argument2 |= ufshcd_readl(hba, REG_UIC_COMMAND_ARG_2) &
				      MASK_UIC_COMMAND_RESULT;

		/*
		 * The UICCMDARG3 register contains the value of the attribute
		 * returned by the uic command in case of DME_GET or the value
		 * to be set in case of DME_SET. While retrieving the set value
		 * is redundant here, we are following the convention from the
		 * core driver. See JESD223E 5.6.4 for additional details.
		 */
		uic_cmd->argument3 = ufshcd_readl(hba, REG_UIC_COMMAND_ARG_3);

		/*
		* This function may return a positive value to indicate the
		* error is coming from the MIPI UniPro layer. Callers of this
		* function should assume any non-zero value as a failure.
		* Value Definitions:
		* 00h SUCCESS
		* 01h INVALID_MIB_ATTRIBUTE
		* 02h INVALID_MIB_ATTRIBUTE_VALUE
		* 03h READ_ONLY_MIB_ATTRIBUTE
		* 04h WRITE_ONLY_MIB_ATTRIBUTE
		* 05h BAD_INDEX
		* 06h LOCKED_MIB_ATTRIBUTE
		* 07h BAD_TEST_FEATURE_INDEX
		* 08h PEER_COMMUNICATION_FAILURE
		* 09h BUSY
		* 0Ah DME_FAILURE
		* 0Bh-FFh Reserved
		*/
		ret = uic_cmd->argument2 & MASK_UIC_COMMAND_RESULT;
	}

	mutex_unlock(&hba->uic_cmd_mutex);
	ufshcd_release(hba);

	return ret;
}

static int ufs_google_dme_set(struct ufs_hba *hba, u32 attr_sel, u32 mib_val)
{
	struct uic_command uic_cmd = {
		.command = UIC_CMD_DME_SET,
		.argument1 = attr_sel,
		.argument2 = UIC_ARG_ATTR_TYPE(ATTR_SET_NOR),
		.argument3 = mib_val,
	};
	int ret;

	ret = ufs_google_send_uic_cmd(hba, &uic_cmd);
	if (ret)
		dev_err(hba->dev, "google_dme_set failed ret=%d\n", ret);

	return ret;
}

static int ufs_google_dme_get(struct ufs_hba *hba, u32 attr_sel, u32 *mib_val)
{
	struct uic_command uic_cmd = {
		.command = UIC_CMD_DME_GET,
		.argument1 = attr_sel,
	};
	int ret;

	ret = ufs_google_send_uic_cmd(hba, &uic_cmd);
	if (ret) {
		dev_err(hba->dev, "google_dme_get failed ret=%d\n", ret);
		return ret;
	}

	*mib_val = uic_cmd.argument3;

	return 0;
}

static void ufs_google_config_uic_intr(struct ufs_hba *hba, bool enable)
{
	u32 set = ufshcd_readl(hba, REG_INTERRUPT_ENABLE);

	if (enable)
		set |= UIC_COMMAND_COMPL;
	else
		set &= ~UIC_COMMAND_COMPL;

	ufshcd_writel(hba, set, REG_INTERRUPT_ENABLE);
}

static int ufs_google_mphy_indirect_reg_write(struct ufs_hba *hba,
					      const struct mphy_reg *reg)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int ret;

	guard(mutex)(&host->indirect_reg_mutex);

	ufs_google_config_uic_intr(hba, false);

	ret = ufs_google_dme_set(hba, UIC_ARG_MIB(MPHY_G5_CBAPBPADDRLSB_OFFSET),
				 reg->addr & 0xFF);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPADDRLSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba, UIC_ARG_MIB(MPHY_G5_CBAPBPADDRMSB_OFFSET),
				 reg->addr >> 8);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPADDRMSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPWDATALSB_OFFSET),
				 reg->data & 0xFF);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPWDATALSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPWDATAMSB_OFFSET),
				 reg->data >> 8);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPWDATAMSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPWRITESEL_OFFSET),
				 0x1);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPWRITESEL_OFFSET\n");
	}

out:
	ufs_google_config_uic_intr(hba, true);

	return ret;
}

static int ufs_google_mphy_indirect_reg_read(struct ufs_hba *hba,
					     struct mphy_reg *reg)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	u32 data;
	int ret;

	guard(mutex)(&host->indirect_reg_mutex);

	ufs_google_config_uic_intr(hba, false);

	ret = ufs_google_dme_set(hba, UIC_ARG_MIB(MPHY_G5_CBAPBPADDRLSB_OFFSET),
				 reg->addr & 0xFF);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPADDRLSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba, UIC_ARG_MIB(MPHY_G5_CBAPBPADDRMSB_OFFSET),
				 reg->addr >> 8);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPADDRMSB_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_set(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPWRITESEL_OFFSET),
				 0x0);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPWRITESEL_OFFSET\n");
		goto out;
	}

	ret = ufs_google_dme_get(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPRDATALSB_OFFSET),
				 &data);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPRDATALSB_OFFSET\n");
		goto out;
	}
	reg->data = data & 0xFF;

	ret = ufs_google_dme_get(hba,
				 UIC_ARG_MIB(MPHY_G5_CBAPBPRDATAMSB_OFFSET),
				 &data);
	if (ret) {
		dev_err(hba->dev,
			"failed to set MPHY_G5_CBAPBPRDATAMSB_OFFSET\n");
		reg->data = 0;
	} else {
		reg->data |= data << 8;
	}

out:
	ufs_google_config_uic_intr(hba, true);

	return ret;
}

static void ufs_google_mphy_apb_mode_select(struct ufs_google_host *host,
					    bool enable)
{
	u32 data;

	data = ufs_top_csr_readl(host, REG_HSIOS_UFS_SRAM_APB_MODE);
	data &= ~UFS_SRAM_APB_MODE_MASK;
	data |= enable;
	ufs_top_csr_writel(host, data, REG_HSIOS_UFS_SRAM_APB_MODE);

	/* UFS HC databook suggests to wait 10 clock cycles of refclk(38.4MHz)
	 * which will be 260ns for the mode change to reflect.
	 * Documentation/timers/timers-howto.rst suggests to us udelay if
	 * SLEEPING FOR "A FEW" USECS ( < ~10us ) */
	 udelay(1);
}

static int ufs_google_set_done_bits(struct ufs_hba *hba)
{
	int ret;
	const struct mphy_reg set_done_registers[] = {
		{ RAWLANEANONN_DIG_RX_AFE_DFE_CAL_DONE_LANE0, 0xc },
		{ RAWLANEANONN_DIG_RX_AFE_DFE_CAL_DONE_LANE1, 0xc },
		{ RAWLANEANONN_DIG_RX_STARTUP_CAL_ALGO_CTL_1_LANE0, 0x2 },
		{ RAWLANEANONN_DIG_RX_STARTUP_CAL_ALGO_CTL_1_LANE1, 0x2 },
		{ RAWLANEANONN_DIG_RX_CONT_ALGO_CTL_LANE0, 0x60 },
		{ RAWLANEANONN_DIG_RX_CONT_ALGO_CTL_LANE1, 0x60 },
	};
	struct mphy_reg mpll_status = {
		.addr = RAWCMN_DIG_AON_CMNCAL_MPLL_STATUS,
		.data = 0x2,
	};

	ret = ufs_google_mphy_indirect_reg_write(hba, &mpll_status);
	if (ret) {
		dev_err(hba->dev,
			"failed to write phy reg: 0x%x, value: 0x%x (%d)\n",
			mpll_status.addr, mpll_status.data, ret);
		return ret;
	}

	/* Registers below require read modify write with bit set */
	for (size_t i = 0; i < ARRAY_SIZE(set_done_registers); i++) {
		struct mphy_reg tmp_reg;

		tmp_reg.addr = set_done_registers[i].addr;

		ret = ufs_google_mphy_indirect_reg_read(hba, &tmp_reg);
		if (ret) {
			dev_err(hba->dev, "failed to read phy reg: 0x%x (%d)\n",
				tmp_reg.addr, ret);
			return ret;
		}

		tmp_reg.data |= set_done_registers[i].data;

		ret = ufs_google_mphy_indirect_reg_write(hba, &tmp_reg);
		if (ret) {
			dev_err(hba->dev,
				"failed to write phy reg: 0x%x, value: 0x%x (%d)\n",
				tmp_reg.addr, tmp_reg.data, ret);
			return ret;
		}
	}

	return 0;
}

static int ufs_google_apply_calibration(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	size_t i;
	int ret;
	ktime_t calibration_start = ktime_get();

	for (i = 0; i < host->phy_cal_size; i++) {
		ret = ufs_google_mphy_indirect_reg_write(hba,
							 &host->phy_cal_data[i]);
		if (ret) {
			dev_err(hba->dev,
				"failed to apply calibration parameter addr=%x data=%x\n",
				host->phy_cal_data[i].addr,
				host->phy_cal_data[i].data);
			goto out;
		}
	}

	ret = ufs_google_set_done_bits(hba);

out:
	dev_info(hba->dev, "applying calibration took %lld usec\n",
		 ktime_to_us(ktime_sub(ktime_get(), calibration_start)));

	return ret;
}

static int ufs_google_phy_initialization(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int ret;
	u32 data, tactivate;
	u64 fw_update_duration_us = 0;
	ktime_t fw_update_start;
	size_t attr_len = 0;
	const struct ufs_dme_attr *rmmi_attrs;
	bool skip_phy_init =
		device_property_read_bool(hba->dev, "google,skip-ufs-phy-init");

	/*
	 * TODO(b/313024923): replace tactivate with OTP value.
	 * UniPro tActivate (0x15A8) accepts value in units of 100 us.
	 */
	tactivate = 0xa;
	ret = ufshcd_dme_set_attr(hba,
				   UIC_ARG_MIB(UNIPRO_L15_PA_T_ACTIVATE),
				   ATTR_SET_NOR, tactivate, DME_LOCAL);
	if (ret)
		return ret;

	/* Program RMMI attributes and update configuration */
	ret = ufs_plat_get_phy_rmmi_attrs(host, &rmmi_attrs, &attr_len);
	if (ret) {
		dev_err(hba->dev, "get rmmi attrs failed %d", ret);
		return ret;
	}
	ret = ufshcd_google_dme_set_seq(hba, rmmi_attrs, attr_len);
	if (ret)
		return ret;

	/*
	 * MPHY_G5_CBULPH8_OFFSET controls PHY PG which should match HC PG
	 * configuration. If neither AH8 nor SWH8 support HC PG, then disable
	 * PHY as well. Otherwise enable it.
	 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(MPHY_G5_CBULPH8_OFFSET),
			     !!(host->caps &
				(GCAP_HC_AH8_PG | GCAP_HC_SWH8_PG)));
	if (ret)
		return ret;

	/* Trigger configuration update */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(UNIPRO_DME_MPHY_CFG_UPD), 0x1);
	if (ret)
		return ret;

	/*
	 * PDTE MPHY Setting Update: b/341119164
	 * Set value to 0 for both the lanes before phy reset release
	 */
	ret = ufs_plat_get_phy_rmmi_tx_eq_attrs(host, &rmmi_attrs, &attr_len);
	if (ret) {
		dev_err(hba->dev, "get tx eq rmmi attrs failed %d", ret);
		return ret;
	}
	ret = ufshcd_google_dme_set_seq(hba, rmmi_attrs, attr_len);
	if (ret)
		return ret;

	/* 3f. Release phy reset */
	ret = reset_control_deassert(host->phy_rst);
	if (ret)
		return ret;

	if (!skip_phy_init) {
		ret = readl_poll_timeout(host->ufs_top_mmio +
						 REG_HSIOS_UFS_PWR_CTRL,
					 data, data & UFS_SRAM_INIT_DONE_MASK,
					 PHY_STATUS_DELAY_US,
					 PHY_STATUS_TIMEOUT_US);
		if (ret)
			return ret;
	}

	/* 3g. Check if Rate B calibration needs to be applied */
	if (ufs_should_apply_phy_cal(host)) {
		ret = ufs_google_apply_calibration(hba);
		if (ret)
			return ret;
		host->calibration_needed = false;
	} else {
		dev_info(hba->dev, "skipping rate B calibration");
	}

	if (ufs_should_apply_phy_patch(host)) {
		const u32 *patch_data;
		size_t patch_sz;

		fw_update_start = ktime_get();

		/* Enable APB access mode for Phy SRAM */
		ufs_google_mphy_apb_mode_select(host, true);

		/* Apply UFS PHY SRAM patch */
		ret = ufs_plat_get_phy_fw_patch(host, &patch_data, &patch_sz);
		if (ret) {
			dev_err(hba->dev, "get mphy patch failed %d", ret);
			return ret;
		}
		memcpy_toio(host->ufs_phy_sram_mmio, patch_data, patch_sz);

		/* Disable APB access mode for Phy SRAM */
		ufs_google_mphy_apb_mode_select(host, false);

		/* 3h. Start FW calibraion */
		data = ufs_top_csr_readl(host, REG_HSIOS_UFS_PWR_CTRL);
		data |= UFS_SRAM_EXT_LD_DONE_MASK;
		ufs_top_csr_writel(host, data, REG_HSIOS_UFS_PWR_CTRL);

		fw_update_duration_us +=
			ktime_to_us(ktime_sub(ktime_get(), fw_update_start));

		dev_info(hba->dev, "phy firmware update took %lld usec\n",
			 fw_update_duration_us);

		host->phy_patching_needed = false;
	}

	/* 3i. Enable MPHY */
	ret = ufshcd_dme_set(hba,
			   UIC_ARG_MIB(UNIPRO_DME_MPHY_DISABLE), 0x0);
	if (ret)
		return ret;

	if (skip_phy_init)
		return ret;

	/* 3j. Wait for MPHY ready */
	ret = ufs_plat_poll_phy_ready(host);
	if (ret)
		dev_err(hba->dev, "mphy not ready %d", ret);
	return ret;
}

/*
 * ufs_cport_setup - CPORT setup for DW UFS
 * @hba: host controller instance
 */
static int ufs_cport_setup(struct ufs_hba *hba)
{
	const struct ufs_dme_attr connection_attrs[] = {
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0x0, DME_LOCAL },
		{ UIC_ARG_MIB(N_DEVICEID), 0x0, DME_LOCAL },
		{ UIC_ARG_MIB(N_DEVICEID_VALID), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB(T_PEERDEVICEID), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB(T_PEERCPORTID), 0x0, DME_LOCAL },
		{ UIC_ARG_MIB(T_TRAFFICCLASS), 0x0, DME_LOCAL },
		{ UIC_ARG_MIB(T_CPORTFLAGS), 0x6, DME_LOCAL },
		{ UIC_ARG_MIB(T_CPORTMODE), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0x0, DME_PEER },
		{ UIC_ARG_MIB(N_DEVICEID), 0x1, DME_PEER },
		{ UIC_ARG_MIB(N_DEVICEID_VALID), 0x1, DME_PEER },
		{ UIC_ARG_MIB(T_PEERDEVICEID), 0x0, DME_PEER },
		{ UIC_ARG_MIB(T_PEERCPORTID), 0x0, DME_PEER },
		{ UIC_ARG_MIB(T_TRAFFICCLASS), 0x0, DME_PEER },
		{ UIC_ARG_MIB(T_CPORTFLAGS), 0x6, DME_PEER },
		{ UIC_ARG_MIB(T_CPORTMODE), 0x1, DME_PEER },
		{ UIC_ARG_MIB(T_CONNECTIONSTATE), 0x1, DME_PEER }
	};

	return ufshcd_google_dme_set_seq(hba, connection_attrs,
					 ARRAY_SIZE(connection_attrs));
}

static void ufs_pm_up(struct ufs_google_host *host)
{
	if (host->pm_request_active)
		return;

	if (host->caps & GCAP_RSC_IP_IDLE)
		google_update_ip_idle_status(host->ip_idle_index, STATE_BUSY);

	if (block_c4)
		cpu_latency_qos_add_request(&host->pm_qos_req,
					    PM_QOS_DEFAULT_VALUE);

	if (host->caps & GCAP_RSC_AUX_CLK) {
		int ret = clk_prepare_enable(host->hsios_aux_clk);

		if (ret) {
			struct ufs_hba *hba = host->hba;

			dev_err(hba->dev,
				"hsios_aux_clk failed to enable, ret=%d", ret);
		}
	}

	host->pm_request_active = true;
}

static void ufs_pm_down(struct ufs_google_host *host)
{
	if (!host->pm_request_active)
		return;

	if (host->caps & GCAP_RSC_IP_IDLE)
		google_update_ip_idle_status(host->ip_idle_index, STATE_IDLE);

	if (block_c4)
		cpu_latency_qos_remove_request(&host->pm_qos_req);

	if (host->caps & GCAP_RSC_AUX_CLK)
		clk_disable_unprepare(host->hsios_aux_clk);

	host->pm_request_active = false;
}

static inline void ufs_google_config_pm_lvl(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);

	if (host->pm_set)
		return;

	/* config default pm lvl only once */
	hba->rpm_lvl = UFS_PM_LVL_3;
	hba->spm_lvl = UFS_PM_LVL_5;
	host->pm_set = true;
}

struct dme_attr_requirement {
	struct ufs_dme_attr attr;
	int min_gear;
};

static int ufs_google_update_tactivate(struct ufs_hba *hba)
{
	u32 tactivate;
	int ret = 0;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(UNIPRO_L15_PA_T_ACTIVATE),
			     &tactivate);
	if (ret) {
		dev_err(hba->dev, "unable to get T_ACTIVATE, err %d\n", ret);
		return ret;
	}

	/*
	 * Synopsys recommends to update the host T_ACTIVATE by 55us after
	 * negotiating with device. b/410823571
	 */
	tactivate += 1;
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(UNIPRO_L15_PA_T_ACTIVATE),
			     tactivate);
	if (ret)
		dev_err(hba->dev, "unable to set T_ACTIVATE %u, err %d\n",
			tactivate, ret);
	return ret;
}

/*
 * ufs_google_link_startup_notify - Google specific link startup setup
 * @hba: host controller instance
 * @status: callback status from core driver
 */
static int ufs_google_link_startup_notify(struct ufs_hba *hba,
					  enum ufs_notify_change_status status)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int err;
	u32 reg_clear_mask = 0;

	switch (status) {
	case PRE_CHANGE:
		/* Program clock divider register */
		ufshcd_writel(hba,
			      (HCLKDIV_VALUE << HCLKDIV_OFFSET) & HCLKDIV_MASK,
			      REG_HCLKDIV);

#if IS_ENABLED(CONFIG_SOC_LGA)
		/* TODO(b/413175647): Misconfig of REG_UFS_AWUSER_VC */
		/* Configure vc to 1 for read and write */
		ufs_top_csr_writel(host,
				   FIELD_PREP(UFS_AWUSER_VC, 1) |
					   FIELD_PREP(UFS_ARUSER_VC, 1),
				   REG_HSIOS_UFS_AWUSER_VC);
#endif
		err = ufshcd_vops_phy_initialization(hba);
		if (err) {
			dev_err(hba->dev,
				"PHY initialization failed, error: %d\n", err);
			return err;
		}
		break;
	case POST_CHANGE:
		/* 4b. Host TActivate update */
		err = ufs_google_update_tactivate(hba);
		if (err)
			return err;

		err = ufs_cport_setup(hba);
		if (err) {
			dev_err(hba->dev, "CPORT setup failed, error: %d\n",
				err);
			return err;
		}

		/* Perform full initialization while coming out of suspend */
		ufs_google_config_pm_lvl(hba);

		if (!(host->caps & GCAP_HC_AH8_PG)) {
			reg_clear_mask |= LP_AH8_PGE_MASK;
		}

		if (!(host->caps & GCAP_HC_SWH8_PG)) {
			reg_clear_mask |= LP_PGE_MASK;
		}

		if (reg_clear_mask) {
			u32 reg = ufshcd_readl(hba, REG_BUSTHRTL);
			reg &= ~reg_clear_mask;
			ufshcd_writel(hba, reg, REG_BUSTHRTL);
		}

		ufs_pm_up(host);

		break;
	}

	return 0;
}

static int
ufs_google_pwr_change_notify(struct ufs_hba *hba,
			     enum ufs_notify_change_status status,
			     struct ufs_pa_layer_attr *dev_max_params,
			     struct ufs_pa_layer_attr *dev_req_params)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	struct ufs_dev_params ufs_google_cap;
	int ret = 0;

	if (!dev_req_params) {
		dev_err(hba->dev, "%s: incoming dev_req_params is NULL\n", __func__);
		return -EINVAL;
	}

	switch (status) {
	case PRE_CHANGE:
		ufshcd_init_pwr_dev_param(&ufs_google_cap);

		ufs_google_cap.hs_rx_gear = host->google_pwr_mode.gear_rx;
		ufs_google_cap.hs_tx_gear = host->google_pwr_mode.gear_tx;
		ufs_google_cap.rx_lanes = host->google_pwr_mode.lane_rx;
		ufs_google_cap.tx_lanes = host->google_pwr_mode.lane_tx;
		ufs_google_cap.rx_pwr_hs = host->google_pwr_mode.pwr_rx;
		ufs_google_cap.tx_pwr_hs = host->google_pwr_mode.pwr_tx;
		ufs_google_cap.hs_rate = host->google_pwr_mode.hs_rate;

		ret = ufshcd_get_pwr_dev_param(&ufs_google_cap,
					       dev_max_params,
					       dev_req_params);
		if (ret) {
			dev_err(hba->dev,
				"%s: failed to determine capabilities (%d)\n",
				__func__, ret);
			return ret;
		}

		if (dev_req_params->pwr_rx != dev_req_params->pwr_tx ||
		    dev_req_params->gear_rx != dev_req_params->gear_tx) {
			dev_err(hba->dev,
				"%s: rx/tx must be symmetrical pwr: %u/%u gear: %u/%u\n",
				__func__, dev_req_params->pwr_rx,
				dev_req_params->pwr_tx, dev_req_params->gear_rx,
				dev_req_params->gear_tx);
			return -EINVAL;
		}

		dev_info(hba->dev, "hs_gear=%u hs_series=%u hs_rate=%u\n",
			 dev_req_params->gear_rx, dev_req_params->pwr_rx,
			 dev_req_params->hs_rate);

		/* Set Adapt */
		if ((dev_req_params->pwr_rx == FAST_MODE ||
		     dev_req_params->pwr_rx == FASTAUTO_MODE) &&
		    (dev_req_params->gear_rx >= 4)) {
			ret = ufshcd_dme_set(hba,
					   UIC_ARG_MIB(PA_TXHSADAPTTYPE),
					   PA_INITIAL_ADAPT);
		} else {
			ret = ufshcd_dme_set(hba,
					   UIC_ARG_MIB(PA_TXHSADAPTTYPE),
					   PA_NO_ADAPT);
		}

		if (ret)
			dev_err(hba->dev, "%s: failed to set ADAPT(%d)\n",
				__func__, ret);
		ret = pixel_ufs_crypto_resume(hba);

		break;
	case POST_CHANGE:
		/*
		 * We would like to override the default clock gate delay
		 * value, which is set to 150 ms during ufshcd_init().
		 * Unfortunately ufshcd_init() is called after
		 * ufshcd_vops_init(), and we are unable to set it there.
		 * We would also like to only set it once, so that we do not
		 * override a value that might have been set via the
		 * clkgate_delay_ms sysfs. Performing the operation here will
		 * update the value after the first transition to high speed
		 * gear.
		 */
		if (!host->clkgate_delay_set) {
			ufshcd_clkgate_delay_set(hba->dev, CLKGATE_DELAY_MS);
			host->clkgate_delay_set = true;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ufs_google_phy_setup_vreg(struct ufs_hba *hba, bool on)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int ret = 0;

	switch (hba->uic_link_state) {
	case UIC_LINK_ACTIVE_STATE:
	case UIC_LINK_BROKEN_STATE:
	case UIC_LINK_OFF_STATE:
		ret = ufs_google_toggle_vreg(hba, host->vdd1p2, on);
		fallthrough;
	case UIC_LINK_HIBERN8_STATE:
		/* toggle only 0.75v if link is in H8 */
		ret = ufs_google_toggle_vreg(hba, host->vdd0p75, on);
		break;
	}

	return ret;
}

static int ufs_google_suspend(struct ufs_hba *hba, enum ufs_pm_op op,
			      enum ufs_notify_change_status status)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	int ret = 0;

	switch (op) {
	case UFS_RUNTIME_PM:
		switch (status) {
		case PRE_CHANGE:
			break;
		case POST_CHANGE:
			ufs_google_phy_setup_vreg(hba, false);
			break;
		}
		break;
	case UFS_SHUTDOWN_PM:
		break;
	case UFS_SYSTEM_PM:
		switch (status) {
		case PRE_CHANGE:
			break;
		case POST_CHANGE:
			ufs_google_phy_setup_vreg(hba, false);
#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
			pixel_ufs_notify_system_pm(hba, true);
#endif
			ufs_pm_down(host);
			ufs_google_pd_put_sync(host);
			break;
		}
		break;
	}

	return ret;
}

static int ufs_google_resume(struct ufs_hba *hba, enum ufs_pm_op op)
{
	ufs_google_phy_setup_vreg(hba, true);

	if (op == UFS_SYSTEM_PM) {
		struct ufs_google_host *host = ufshcd_get_variant(hba);

		ufs_google_pd_get_sync(host);
#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
		pixel_ufs_notify_system_pm(hba, false);
#endif
	}

	return 0;
}

static void ufs_hibern8_notify(struct ufs_hba *hba, enum uic_cmd_dme cmd,
			       enum ufs_notify_change_status status)
{
	switch (cmd) {
	case UIC_CMD_DME_HIBER_ENTER:
		switch (status) {
		case PRE_CHANGE:
			/*
			 * Synopsys recommends disabling AH8 before initiating SWH8 when using both
			 * AH8 and SWH8 together. Starting software hibernation without first
			 * disabling auto-hibernate may lead to unexpected behavior.
			 */
			ufs_auto_hibern8_update(hba, false);
			break;
		case POST_CHANGE:
			ufs_pm_down(ufshcd_get_variant(hba));
			break;
		default:
			break;
		}
		break;
	case UIC_CMD_DME_HIBER_EXIT:
		switch (status) {
		case PRE_CHANGE:
			ufs_pm_up(ufshcd_get_variant(hba));
			break;
		case POST_CHANGE:
			ufs_auto_hibern8_update(hba, true);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
#if IS_ENABLED(CONFIG_UFS_PIXEL_FEATURES)
	if (status == POST_CHANGE)
		pixel_ufs_record_hibern8(hba, cmd == UIC_CMD_DME_HIBER_ENTER);
#endif
}

static int ufs_google_get_hba_mac(struct ufs_hba *hba)
{
	return MAX_SUPP_MAC;
}

static const struct ufshcd_res_info ufs_res_info[RES_MAX] = {
	{.name = "ufs_mem",},
	{.name = "mcq",},
	/* Submission Queue Doorbell Address Offset */
	{.name = "mcq_sqd",},
	/* Submission Queue Interrupt Status */
	{.name = "mcq_sqis",},
	/* Completion Queue Doorbell Address Offset */
	{.name = "mcq_cqd",},
	/* Completion Queue Interrupt Status */
	{.name = "mcq_cqis",},
	/* MCQ vendor specific */
	{.name = "mcq_vs",},
};

static int ufs_google_map_memory_area_as_resource(struct ufs_hba *hba,
						  int res_info_index, uint32_t mem_offset)
{
	struct resource *res_mem, *res_mcq;
	struct platform_device *pdev = to_platform_device(hba->dev);
	struct ufshcd_res_info *res;
	int ret = 0;
	u32 mem_offsets[RES_MAX] = {
		[RES_MCQ] = MCQ_QCFG_SIZE,
		[RES_MCQ_SQD] = MAX_REG_SQDAO,
		[RES_MCQ_SQIS] = MAX_REG_SQIS,
		[RES_MCQ_CQD] = MAX_REG_CQDAO,
		[RES_MCQ_CQIS] = MAX_REG_CQIS,
	};

	res_mem = &pdev->resource[0];
	res = &hba->res[res_info_index];

	if (res_info_index == RES_UFS) {
		res->resource = res_mem;
		res->base = hba->mmio_base;
		goto out;
	}

	/* Explicitly allocate MCQ resource from ufs_mem */
	res_mcq = devm_kzalloc(hba->dev, sizeof(*res_mcq), GFP_KERNEL);
	if (!res_mcq) {
		ret = -ENOMEM;
		goto out;
	}
	res->resource = res_mcq;

	res_mcq->start = mem_offset;
	res_mcq->flags = res_mem->flags;
	res_mcq->name = ufs_res_info[res_info_index].name;
	res_mcq->end = res_mcq->start + hba->nr_hw_queues * mem_offsets[res_info_index] - 1;

	ret = insert_resource(&iomem_resource, res_mcq);
	if (ret) {
		dev_err(hba->dev, "Failed to insert MCQ resource, err=%d\n",
			ret);
		goto out;
	}

	res->base = devm_ioremap_resource(hba->dev, res_mcq);
	if (IS_ERR(res->base)) {
		dev_err(hba->dev, "MCQ %s registers mapping failed, err=%d\n",
			res->name,
			(int)PTR_ERR(res->base));
		ret = PTR_ERR(res->base);
		goto ioremap_err;
	}
	if (res_info_index == RES_MCQ)
		hba->mcq_base = res->base;
	goto out;

ioremap_err:
	res->base = NULL;
	remove_resource(res_mcq);
out:
	return ret;
}

static int ufs_google_mcq_config_resource(struct ufs_hba *hba)
{
	/* Setting up the memory backed registers for queue configuration. */
	struct platform_device *pdev = to_platform_device(hba->dev);
	struct resource *res_mem;
	int ret;

	memcpy(hba->res, ufs_res_info, sizeof(ufs_res_info));

	res_mem = &pdev->resource[0];

	ret = ufs_google_map_memory_area_as_resource(hba, RES_UFS, 0);
	if (ret)
		dev_err(hba->dev, "Failed to create UFS memory resource\n");
	ret = ufs_google_map_memory_area_as_resource(hba, RES_MCQ,
						     res_mem->start +
						     MCQ_SQATTR_OFFSET(hba->mcq_capabilities));
	if (ret)
		dev_err(hba->dev, "Failed to create MCQ queue memory resource\n");
	ret = ufs_google_map_memory_area_as_resource(hba, RES_MCQ_SQD, res_mem->start + MCQ_SQDAO);
	if (ret)
		dev_err(hba->dev, "Failed to create MCQ SQOPS memory resource\n");
	ret = ufs_google_map_memory_area_as_resource(hba, RES_MCQ_SQIS, res_mem->start + MCQ_SQINT);
	if (ret)
		dev_err(hba->dev, "Failed to create MCQ SQINT memory resource\n");
	ret = ufs_google_map_memory_area_as_resource(hba, RES_MCQ_CQD, res_mem->start + MCQ_CQDAO);
	if (ret)
		dev_err(hba->dev, "Failed to create MCQ CQOPS memory resource\n");
	ret = ufs_google_map_memory_area_as_resource(hba, RES_MCQ_CQIS, res_mem->start + MCQ_CQINT);
	if (ret)
		dev_err(hba->dev, "Failed to create MCQ CQINT memory resource\n");

	return ret;
}

static int ufs_google_op_runtime_config(struct ufs_hba *hba)
{
	struct ufshcd_res_info *mem_res, *mcq_res;
	struct ufshcd_mcq_opr_info_t *opr;
	unsigned long long mem_base_address;
	int res_index;
	unsigned long strides[] = { MAX_REG_SQDAO, MAX_REG_SQIS, MAX_REG_CQDAO, MAX_REG_CQIS };

	mem_res = &hba->res[RES_UFS];
	if (!mem_res->base)
		return -EINVAL;
	mem_base_address = mem_res->resource->start;

	/*
	 * Stride should take us from queue 0 to queue 1
	 * Offset is the start of the memory area in relation to the ufs base.
	 * example: reg = mcq_opr_base(hba, OPR_SQD, id) + REG_SQRTS;
	 *          return opr->base + (opr->stride * i);
	 * array size is nr_hw_queues in size
	 */
	for (res_index = RES_MCQ_SQD; res_index <= RES_MCQ_CQIS; ++res_index) {
		mcq_res = &hba->res[res_index];
		if (!mcq_res->base)
			return -EINVAL;
		opr = &hba->mcq_opr[OPR_SQD + res_index - RES_MCQ_SQD];
		opr->offset = mcq_res->resource->start - mem_base_address;
		opr->stride = strides[res_index - RES_MCQ_SQD];
		opr->base = mcq_res->base;
	}

	return 0;
}

static void __iomem *mcq_opr_base(struct ufs_hba *hba,
				  enum ufshcd_mcq_opr n, int i)
{
	struct ufshcd_mcq_opr_info_t *opr = &hba->mcq_opr[n];

	return opr->base + opr->stride * i;
}

/**
 * ufs_google_mcq_intr - multi interrupt service routine
 * @irq: irq number
 * @__host: pointer to host instance
 *
 * Return:
 *  IRQ_HANDLED - If interrupt is valid
 *  IRQ_NONE    - If invalid interrupt
 */
static irqreturn_t ufs_google_mcq_intr(int irq, void *desc_p)
{
	struct cq_irq_desc *desc = desc_p;
	struct ufs_google_host *host = desc->host;
	unsigned int qid = desc->qid;
	struct ufs_hba *hba = host->hba;
	struct ufs_hw_queue *hwq;
	u32 events;

	hwq = &hba->uhq[qid];

	/* clear CQIS */
	events = ufshcd_mcq_read_cqis(hba, qid);
	if (events)
		ufshcd_mcq_write_cqis(hba, events, qid);

	/* clear IAG counter by writing 1 to MCQIACRy.CTR */
	writel(FIELD_PREP(IACTH, 1) | CTR | IAPWEN | IAEN,
	       mcq_opr_base(hba, OPR_CQIS, qid) + REG_MCQIACR);

	/* process CQy and update CQ head pointer */
	if (events & UFSHCD_MCQ_CQIS_TAIL_ENT_PUSH_STS)
		ufshcd_mcq_poll_cqe_lock(hba, hwq);

	return IRQ_HANDLED;
}

static int ufs_google_config_multi_interrupt(struct ufs_hba *hba)
{
	struct ufs_google_host *host = ufshcd_get_variant(hba);
	struct platform_device *pdev = to_platform_device(hba->dev);
	u32 nr_irqs, maxq, set, legacy_irq;
	irq_handler_t ufshcd_intr = NULL;
	int ret, i;

	if (device_property_read_bool(hba->dev, "google,skip-mcq-multi-intr"))
		return -EOPNOTSUPP;

	/* Poll queues use do not require multi interrupt */
	maxq = FIELD_GET(MAX_QUEUE_SUP, hba->mcq_capabilities) + 1;
	nr_irqs = hba->nr_hw_queues - hba->nr_queues[HCTX_TYPE_POLL];
	if (nr_irqs > maxq)
		return -EINVAL;

	/* Disable CQEE(using quirk) and IAGEE */
	set = ufshcd_readl(hba, REG_INTERRUPT_ENABLE);
	set &= ~MCQ_IAG_EVENT_STATUS;
	ufshcd_writel(hba, set, REG_INTERRUPT_ENABLE);

	for (i = 0; i < maxq; ++i) {
		/* Completion Queue Configuration */
		ufsmcq_writel(hba, IAGVLD | FIELD_PREP(IAG_MASK, i),
			      MCQ_CFG_n(REG_CQCFG, i));

		/* Completion Queue Interrupt Status Address Offset */
		ufsmcq_writelx(hba, MCQ_OPR_OFFSET_n(OPR_CQIS, i),
			       MCQ_CFG_n(REG_CQISAO, i));

		/* Interrupt Aggregation Configuration */
		writel(FIELD_PREP(IACTH, 1) | CTR | IAPWEN | IAEN,
		       mcq_opr_base(hba, OPR_CQIS, i) + REG_MCQIACR);
	}

	if (host->multi_intr_enabled)
		return 0;

	/*
	 * Hack: save the legacy interrupt handler pointer and free the legacy
	 * interrupt. Without this, devm_platform_get_irqs_affinity() fails with
	 * -EBUSY. TODO(b/359673199): remove this hack.
	 */
	legacy_irq = platform_get_irq(pdev, 0);
	struct irq_desc *desc = irq_to_desc(legacy_irq);
	if (desc && desc->action) {
		ufshcd_intr = desc->action->handler;
		devm_free_irq(hba->dev, legacy_irq, hba);
	}

	struct irq_affinity affd = {
		.pre_vectors = 1,
	};
	int *irqs;

	ret = devm_platform_get_irqs_affinity(pdev, &affd, /*minvec=*/1,
					      /*maxvec=*/nr_irqs + 1, &irqs);
	if (ufshcd_intr) {
		int err;

		/*
		 * Hack: restore the legacy interrupt handler. The code below
		 * is a duplicate of code in the UFS core driver and must be
		 * kept in sync with the UFS core driver.
		 */
		err = devm_request_irq(hba->dev, legacy_irq, ufshcd_intr,
				       IRQF_SHARED, UFSHCD, hba);
		if (err) {
			pr_crit("devm_request_irq() failed: %d\n", err);
			BUG();
		}
		irq_set_status_flags(hba->irq, IRQ_DISABLE_UNLAZY);
	}

	if (ret < nr_irqs + 1) {
		WARN_ONCE(true, "ret (%d) < nr_irqs + 1 (%d)\n", ret,
			  nr_irqs + 1);
		goto out;
	}

	for (i = 0; i < nr_irqs; ++i) {
		struct cq_irq_desc *desc = &host->cq_desc[i];
		*desc = (typeof(*desc)){ .host = host, .qid = i };
		/* TODO: b/359878507 - Remove IRQF_PERCPU again. */
		ret = devm_request_irq(hba->dev, irqs[i + 1],
				       ufs_google_mcq_intr,
				       IRQF_SHARED | IRQF_PERCPU, "ufshcd-cq",
				       desc);
		if (ret) {
			dev_err(hba->dev, "%i request irq failed\n", i);
			goto out;
		}
	}

	host->multi_intr_enabled = true;
out:
	return ret;
}

static int ufs_google_device_reset(struct ufs_hba *hba)
{
	/*
	 * In prior to getting this, GENPD_NOTIFY_PRE_ON and
	 * GENPD_NOTIFY_ON were called by generic resume callback.
	 * Hence, returning zero makes ufshcd_set_ufs_dev_active()
	 * which avoids link_startup_again = true in ufshcd_link_startup.
	 */
	return 0;
}

static void ufs_google_debug_register_dump(struct ufs_hba *hba)
{
	bool uart_enabled = device_property_read_bool(hba->dev, "uart-enabled");

	dev_err(hba->dev, "uart-enabled=%s\n", uart_enabled ? "true" : "false");
}

static const struct ufs_hba_variant_ops ufs_hba_google_vops = {
	.name = "google-ufs",
	.init = ufs_google_init,
	.link_startup_notify = ufs_google_link_startup_notify,
	.phy_initialization = ufs_google_phy_initialization,
	.pwr_change_notify = ufs_google_pwr_change_notify,
	.suspend = ufs_google_suspend,
	.resume = ufs_google_resume,
	.hibern8_notify = ufs_hibern8_notify,
	.get_hba_mac = ufs_google_get_hba_mac,
	.mcq_config_resource = ufs_google_mcq_config_resource,
	.op_runtime_config = ufs_google_op_runtime_config,
	.config_esi = ufs_google_config_multi_interrupt,
	.device_reset = ufs_google_device_reset,
	.dbg_register_dump = ufs_google_debug_register_dump
};

static const struct of_device_id ufs_google_of_match[] = {
	{ .compatible = "google,ufshc", .data = &ufs_hba_google_vops },
	{},
};
MODULE_DEVICE_TABLE(of, ufs_google_of_match);

static int ufs_google_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct ufs_hba *hba;

	match = of_match_node(ufs_google_of_match, dev->of_node);
	err = ufshcd_pltfrm_init(pdev, match->data);
	if (err) {
		dev_err(dev, "ufshcd_pltfrm_init() failed %d\n", err);
		return err;
	}

	hba = platform_get_drvdata(pdev);

	ufs_google_init_debugfs(hba);

	/* TODO(b/294685860): check requirement for lazy irq disable */
	irq_set_status_flags(hba->irq, IRQ_DISABLE_UNLAZY);

	return 0;
}

static int ufs_google_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba = platform_get_drvdata(pdev);
	struct ufs_google_host *host = ufshcd_get_variant(hba);

	atomic_dec(&ufs_host_index);

	ufs_google_pd_put_sync(host);
	dev_pm_genpd_remove_notifier(host->ufs_pd);
	ufs_google_detach_power_domains(host);

	ufs_google_remove_debugfs(hba);
	ufs_google_remove_dbg(hba);
	ufshcd_remove(hba);

	ufs_google_phy_setup_vreg(hba, false);

	return 0;
}

static const struct dev_pm_ops ufs_google_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufshcd_system_suspend, ufshcd_system_resume)
	SET_RUNTIME_PM_OPS(ufshcd_runtime_suspend, ufshcd_runtime_resume, NULL)
	.prepare = ufshcd_suspend_prepare,
	.complete = ufshcd_resume_complete,
};

static struct platform_driver ufs_google_pltform = {
	.probe = ufs_google_probe,
	.remove = ufs_google_remove,
	.driver = {
		.name = "google-ufshcd",
		.pm = &ufs_google_pm_ops,
		.of_match_table = of_match_ptr(ufs_google_of_match),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(ufs_google_pltform);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google UFS Controller Driver");
MODULE_LICENSE("GPL");
