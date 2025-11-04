/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Google LLC */

#ifndef PD_LATENCY_PROFILE_H
#define PD_LATENCY_PROFILE_H

#include <linux/pm_domain.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#define DEFINE_LATENCY_TYPE(__type) \
	__type##_ON, \
	__type##_OFF

#define LATENCY_TYPE_BITMASK(__type) (1u << (__type))

enum latency_type {
	DEFINE_LATENCY_TYPE(EXEC), /* start to cpm handler latency */
	DEFINE_LATENCY_TYPE(FW), /* cpm FW latency */
	DEFINE_LATENCY_TYPE(ACK), /* start to cpm scheduling latency */
	DEFINE_LATENCY_TYPE(E2E), /* end2end latency */
	LATENCY_TYPE_COUNT
};

static const char *const profiled_stat_name[LATENCY_TYPE_COUNT] = {
	[EXEC_ON] = "exec_on",
	[EXEC_OFF] = "exec_off",
	[FW_ON] = "fw_on",
	[FW_OFF] = "fw_off",
	[ACK_ON] = "ack_on",
	[ACK_OFF] = "ack_off",
	[E2E_ON] = "e2e_on",
	[E2E_OFF] = "e2e_off",
};

struct latency_data {
	struct kobject *parent;
	u32 count;
	u64 latency_total_sum;
	u32 min_latency;
	u32 max_latency;

	u64 start_time;
};

struct latency_profile {
	bool collect_data; /* flag to enable/disable the data collection */
	struct latency_data *data;
	u32 *genpd_sswrp_ids;
	int pd_count;

	struct kobject *latencies_kobj;
	struct kobject **power_domain_kobj;
	struct kobject **data_kobj;
};

int pd_latency_profile_init(struct platform_device *pdev);
int pd_latency_profile_remove(struct platform_device *pdev);

int pd_latency_profile_start(struct generic_pm_domain *domain, u16 type_mask);

/* profile_stop takes start_time as an argument in order to compute delta */
int pd_latency_profile_stop(struct generic_pm_domain *domain, u16 type_mask, bool cancel);

/* pd_latency_profile_store() - Store the latency for the given domain in the
 *                              approprioate stat.
 *
 *  @domain: power domain where to store the latency
 *  @latency: the latecy value to store
 *  @lat_type: the latency type to store
 */
int pd_latency_profile_store(struct generic_pm_domain *domain, u64 latency,
	enum latency_type lat_type);

#endif /* PD_LATENCY_PROFILE_H */
