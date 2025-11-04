// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */

#include "bcl.h"
#include "core_pmic_defs.h"

int pmic_write(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 value)
{
	switch (pmic) {
	case CORE_PMIC_SUB:
		return core_pmic_sub_write_register(bcl_dev, reg, value, false);
	case CORE_PMIC_MAIN:
		return core_pmic_main_write_register(bcl_dev, reg, value, false);
	}
	return 0;
}

int pmic_read(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 *value)
{
	switch (pmic) {
	case CORE_PMIC_SUB:
		return core_pmic_sub_read_register(bcl_dev, reg, value, false);
	case CORE_PMIC_MAIN:
		return core_pmic_main_read_register(bcl_dev, reg, value, false);
	}
	return 0;
}

int read_uvlo_dur(struct bcl_device *bcl_dev, uint64_t *data) { return 0; }
int read_pre_uvlo_hit_cnt(struct bcl_device *bcl_dev, uint16_t *data, int pmic) { return 0; }
int read_pre_ocp_bckup(struct bcl_device *bcl_dev, int *pre_ocp_bckup, int rail) { return 0; }
int read_odpm_int_bckup(struct bcl_device *bcl_dev, int *odpm_int_bckup, u16 *type, int pmic,
			int channel)
{
	return 0;
}
u8 core_pmic_get_scratch_pad(struct bcl_device *bcl_dev) { return 0; }
void core_pmic_set_scratch_pad(struct bcl_device *bcl_dev, u8 value) {}
void core_pmic_bcl_init_bbat(struct bcl_device *bcl_dev) {}
void core_pmic_teardown(struct bcl_device *bcl_dev) {}
int core_pmic_mbox_request(struct bcl_device *bcl_dev) { return 0; }
uint32_t core_pmic_get_cpm_cached_sys_evt(struct bcl_device *bcl_dev) { return 0; }
