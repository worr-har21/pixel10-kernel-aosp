// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */

#include "core_pmic_defs.h"

int google_bcl_configure_modem(struct bcl_device *bcl_dev)
{
	return 0;
}

void compute_mitigation_modules(struct bcl_device *bcl_dev,
				struct bcl_mitigation_conf *mitigation_conf, u32 *odpm_lpf_value)
{
}

int meter_write(int pmic, struct bcl_device *bcl_dev, int idx, u8 value) { return 0; }
int meter_read(int pmic, struct bcl_device *bcl_dev, int idx, u8 *value) { return 0; }
