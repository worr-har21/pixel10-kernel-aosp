/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GOOGLE_IRM_REG_INTERNAL_H
#define _GOOGLE_IRM_REG_INTERNAL_H

#include <linux/bitops.h>

#include <interconnect/google_irm_reg.h>

#define GMC_OFFSET			0x0
#define GSLC_OFFSET			0x20

#define DVFS_REQ_RD_BW_AVG		0x0
#define DVFS_REQ_WR_BW_AVG		0x4
#define DVFS_REQ_RD_BW_VCDIST		0x8
#define DVFS_REQ_WR_BW_VCDIST		0xc
#define DVFS_REQ_RD_BW_PEAK		0x10
#define DVFS_REQ_WR_BW_PEAK		0x14
#define DVFS_REQ_LATENCY		0x18
#define DVFS_REQ_LTV			0x1c

#define DVFS_TRIG_EN			BIT(0)

#define BW_MASK				0xFFFF
#define LATENCY_MASK			0xFFFF

#define BW_MAX_VAL			0xFFFF
#define LATENCY_LTV_MAX_VAL		0xFFFE

#endif /* _GOOGLE_IRM_REG_INTERNAL_H */
