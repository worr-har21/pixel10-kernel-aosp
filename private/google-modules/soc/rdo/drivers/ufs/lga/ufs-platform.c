// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2025 Google LLC
 */

#include "ufs-google-platform.h"
#include "ufs-google.h"
#include "ufs-phy-fw-patch.h"
#include "ufs/ufshcd.h"

static int ufs_lga_plt_get_phy_fw_patch(struct ufs_google_host *host,
					const u32 **data, size_t *sz)
{
	*data = ufs_phy_fw_patch;
	*sz = ufs_phy_fw_patch_sz;
	return 0;
}

static int ufs_lga_plat_get_rmmi_tx_eq_attrs(struct ufs_google_host *host,
					     const struct ufs_dme_attr **attrs,
					     size_t *len)
{
	static const struct ufs_dme_attr rmmi_attrs_tx_eq[] = {
		{ UIC_ARG_MIB_SEL(MPHY_G5_TX_EQ_PRE_DE_0P0_LA, MPHY_TX_LANE1),
		  0x0, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_TX_EQ_PRE_DE_0P0_LA, MPHY_TX_LANE2),
		  0x0, DME_LOCAL },
		{ UIC_ARG_MIB(UNIPRO_DME_MPHY_CFG_UPD), 0x1, DME_LOCAL },
	};

	if (ufs_should_apply_phy_patch(host)) {
		/*
		 * PDTE MPHY Setting Update: b/341119164
		 * Set value to 0 for both the lanes before phy reset release
		 */
		*attrs = rmmi_attrs_tx_eq;
		*len = ARRAY_SIZE(rmmi_attrs_tx_eq);
	} else {
		*attrs = NULL;
		*len = 0;
	}

	return 0;
}

static int ufs_lga_plat_get_rmmi_attrs(struct ufs_google_host *host,
				       const struct ufs_dme_attr **attrs,
				       size_t *len)
{
	static const struct ufs_dme_attr rmmi_attrs[] = {
		{ UIC_ARG_MIB(MPHY_G5_CBMPHYBOOTCFG_OFFSET), 0x5, DME_LOCAL },
		{ UIC_ARG_MIB(MPHY_G5_CBATTR2APBCTRL_OFFSET), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_CDR_ACTIVE_LATENCY_ADJUST_OFFSET,
				  MPHY_RX_LANE1), 0x15, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_CDR_ACTIVE_LATENCY_ADJUST_OFFSET,
				  MPHY_RX_LANE2), 0x15, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_ASYNC_FILTER_OFFSET,
				  MPHY_RX_LANE1), 0x1, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(MPHY_G5_RX_ASYNC_FILTER_OFFSET,
				  MPHY_RX_LANE2), 0x1, DME_LOCAL },
	};
	*attrs = rmmi_attrs;
	*len = ARRAY_SIZE(rmmi_attrs);
	return 0;
}

static int ufs_lga_plat_poll_phy_ready(struct ufs_google_host *host)
{
	int ret;
	u32 data;
	struct ufs_hba *hba = host->hba;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data == 1,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_TX_FSM_STATE_OFFSET,
						0x0),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data == 1,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_TX_FSM_STATE_OFFSET,
						0x1),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data & 0x7,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_RX_FSM_STATE_OFFSET,
						0x4),
				&data);
	if (ret)
		return ret;

	ret = read_poll_timeout(ufshcd_dme_get, ret, data & 0x7,
				MPHY_ENABLE_DELAY_US, MPHY_ENABLE_TIMEOUT_US,
				false, hba,
				UIC_ARG_MIB_SEL(MPHY_G5_RX_FSM_STATE_OFFSET,
						0x5),
				&data);

	return ret;
}

static struct ufs_google_ops lga_gops = {
	.get_phy_fw_patch = ufs_lga_plt_get_phy_fw_patch,
	.get_phy_rmmi_tx_eq_attrs = ufs_lga_plat_get_rmmi_tx_eq_attrs,
	.get_phy_rmmi_attrs = ufs_lga_plat_get_rmmi_attrs,
	.poll_phy_ready = ufs_lga_plat_poll_phy_ready,
};

int ufs_google_plat_set_gops(struct ufs_google_host *host)
{
	host->gops = &lga_gops;
	return 0;
}
