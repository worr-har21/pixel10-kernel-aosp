// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC
 */
#include "../google_powerdashboard_iface.h"
#include "../google_powerdashboard_helper.h"
#include "google_powerdashboard_impl.h"

#include <linux/platform_device.h>

#define DEFINE_PWRBLK_NODE(a, b, c) SYSFS_PWRBLK_NODE(PD_PWRBLK_ID(a), b, c);
#define DEFINE_LPCM_NODE(a, b) SYSFS_LPCM_NODE(PD_LPCM_ID(a), b, PWRBLK_SINGLE_LPCM);
#define DEFINE_PWRBLK_ATTR(a, b, c) static DEVICE_ATTR_RO(pwrblk_##b);
#define DEFINE_LPCM_ATTR(a, b) static DEVICE_ATTR_RO(lpcm_##b);

LGA_PWRBLK_X_MACRO_TABLE(DEFINE_PWRBLK_NODE)
LGA_LPCM_X_MACRO_TABLE(DEFINE_LPCM_NODE)

LGA_PWRBLK_X_MACRO_TABLE(DEFINE_PWRBLK_ATTR)
LGA_LPCM_X_MACRO_TABLE(DEFINE_LPCM_ATTR)

#define ASSIGN_PWRBLK_ATTR(a, b, c) (&dev_attr_pwrblk_##b.attr),
#define ASSIGN_LPCM_ATTR(a, b) (&dev_attr_lpcm_##b.attr),

struct attribute *google_powerdashboard_sswrp_attrs[] = {
	LGA_PWRBLK_X_MACRO_TABLE(ASSIGN_PWRBLK_ATTR)
	LGA_LPCM_X_MACRO_TABLE(ASSIGN_LPCM_ATTR)
	NULL,
};

POWER_STATE_BLOCKERS_NODE(dormant_suspend, SOC_POWER_STATE_DORMANT_SUSPEND);
POWER_STATE_BLOCKERS_NODE(ambient_sec_acc, SOC_POWER_STATE_AMBIENT_SEC_ACC);
POWER_STATE_BLOCKERS_NODE(ambient_suspend, SOC_POWER_STATE_AMBIENT_SUSPEND);

FABRIC_ACG_APG_RES_NODE(fabmed_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(fabrt_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(fabstby_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(fabsyss_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(fabhbw_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(fabmem_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(gslc01_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(gslc23_acc_val_acg);
FABRIC_ACG_APG_RES_NODE(gslc01_acc_val_apg);
FABRIC_ACG_APG_RES_NODE(gslc23_acc_val_apg);

GMC_ACG_APG_RES_NODE(gmc_acc_val_acg);
GMC_ACG_APG_RES_NODE(gmc_acc_val_apg);

static DEVICE_ATTR_RO(blockers_dormant_suspend);
static DEVICE_ATTR_RO(blockers_ambient_suspend);
static DEVICE_ATTR_RO(blockers_ambient_sec_acc);
static DEVICE_ATTR_RO(csr_fabmed_acc_val_acg);
static DEVICE_ATTR_RO(csr_fabrt_acc_val_acg);
static DEVICE_ATTR_RO(csr_fabstby_acc_val_acg);
static DEVICE_ATTR_RO(csr_fabsyss_acc_val_acg);
static DEVICE_ATTR_RO(csr_fabhbw_acc_val_acg);
static DEVICE_ATTR_RO(csr_fabmem_acc_val_acg);
static DEVICE_ATTR_RO(csr_gslc01_acc_val_acg);
static DEVICE_ATTR_RO(csr_gslc23_acc_val_acg);
static DEVICE_ATTR_RO(csr_gslc01_acc_val_apg);
static DEVICE_ATTR_RO(csr_gslc23_acc_val_apg);
static DEVICE_ATTR_RO(csr_gmc_acc_val_acg);
static DEVICE_ATTR_RO(csr_gmc_acc_val_apg);

struct attribute *google_powerdashboard_power_state_attrs[] = {
	&dev_attr_blockers_dormant_suspend.attr,
	&dev_attr_blockers_ambient_sec_acc.attr,
	&dev_attr_blockers_ambient_suspend.attr,
	&dev_attr_csr_fabmed_acc_val_acg.attr,
	&dev_attr_csr_fabrt_acc_val_acg.attr,
	&dev_attr_csr_fabstby_acc_val_acg.attr,
	&dev_attr_csr_fabsyss_acc_val_acg.attr,
	&dev_attr_csr_fabhbw_acc_val_acg.attr,
	&dev_attr_csr_fabmem_acc_val_acg.attr,
	&dev_attr_csr_gslc01_acc_val_acg.attr,
	&dev_attr_csr_gslc23_acc_val_acg.attr,
	&dev_attr_csr_gslc01_acc_val_apg.attr,
	&dev_attr_csr_gslc23_acc_val_apg.attr,
	&dev_attr_csr_gmc_acc_val_acg.attr,
	&dev_attr_csr_gmc_acc_val_apg.attr,
	NULL
};
