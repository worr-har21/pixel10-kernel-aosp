/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 Google LLC.
 */

#ifndef __POWER_CONTROLLER_LPM_SEQUENCES_SEQUENCE_H__
#define __POWER_CONTROLLER_LPM_SEQUENCES_SEQUENCE_H__

#include <linux/io.h>
#include <linux/pm_domain.h>

#include <dt-bindings/power/genpd_lga.h>

struct power_domain {
	struct device *dev;
	struct generic_pm_domain genpd;
	struct power_desc *desc;
	void __iomem **regions;
	void *data;
	u32 subdomain_id;
};

struct power_desc {
	bool default_on;
	int (*power_on)(struct power_domain *pd);
	int (*power_off)(struct power_domain *pd);
};

struct sswrp_power_desc {
	const char *const *reg_names;
	u8 region_count;
	struct power_desc *descriptors;
	u8 desc_count;
	void *data;
};

int poll_for_n(struct device *dev, void __iomem *addr, u32 mask, u32 target,
	       u32 poll_period_ms, u32 total_poll_ms);

int poll_for(struct device *dev, void __iomem *addr, u32 mask, u32 target);

int poll_for_psm_state(struct device *dev, void __iomem *psm_addr,
		       u8 target_psm_state);

extern const struct sswrp_power_desc aon_power_desc_table;

#endif /* __POWER_CONTROLLER_LPM_SEQUENCES_SEQUENCE_H__ */
