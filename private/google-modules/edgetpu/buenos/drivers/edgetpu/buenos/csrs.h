/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent CSRs.
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/sizes.h>

enum edgetpu_csrs {
	EDGETPU_REG_WDT0_CONTROL = 0x18d008,
	EDGETPU_REG_WDT0_KEY = 0x18d010,
	EDGETPU_REG_WDT1_CONTROL = 0x18e008,
	EDGETPU_REG_WDT1_KEY = 0x18e010,
	EDGETPU_REG_CPUNS_TIMESTAMP = 0x1a01c0,
	EDGETPU_LPM_CONTROL_CSR = 0x1e0028,
	EDGETPU_LPM_CORE_CSR = 0x1e0030,
	EDGETPU_LPM_CLUSTER_CSR0 = 0x1e0038,
	EDGETPU_LPM_CLUSTER_CSR1 = 0x1e0040,
};

/*
 * Laguna PA and size of sswrp_cpm > cpm_top > cpm_periph_csrs > cpm_lpb > LPB_SSWRP_TPU_CSRS
 * (aka LPB_SSWRP_CSRS[32]).
 */
#define LPB_SSWRP_DEFAULT_TPU_CSRS	0x5330800
#define LPB_SSWRP_DEFAULT_TPU_CSRS_SIZE	0x18

/*
 * Laguna PA and size of sswrp_cpm > cpm_top > cpm_periph_csrs > cpm_lpb >
 * LPB_CLIENT_CSRS[1] (TPU).
 */
#define LPB_CLIENT_TPU_CSRS		0x50033000
#define LPB_CLIENT_TPU_CSRS_SIZE	SZ_4K

/* LPCM_LPM_TPU_CSRS register offsets. */
#define LPCM_LPM_TPU_PSM0_STATUS	0x2244
#define LPCM_LPM_TPU_PSM1_STATUS	0x3294
#define LPCM_LPM_TPU_PSM2_STATUS	0x4264
