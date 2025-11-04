/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * google_powercap_cpu.h Google cpu powercap related functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#ifndef _GOOGLE_POWERCAP_CPU_H_
#define _GOOGLE_POWERCAP_CPU_H_

#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/pm_qos.h>

#include "google_powercap.h"

struct gpowercap_cpu {
	struct gpowercap gpowercap;
	struct freq_qos_request qos_req;
	unsigned int cpu;
	unsigned int num_opps;
	cpumask_t related_cpus;
	struct cdev_opp_table *opp_table;
	struct list_head node;
};

static inline struct gpowercap_cpu *to_gpowercap_cpu(struct gpowercap *gpowercap)
{
	return container_of(gpowercap, struct gpowercap_cpu, gpowercap);
}

u64 __gpc_cpu_set_cluster_power_limit(struct gpowercap *gpowercap, u64 power_limit);
u64 __gpc_cpu_get_cluster_power_uw(struct gpowercap *gpowercap);
int __gpc_cpu_update_cluster_power_uw(struct gpowercap *gpowercap);
void __gpc_cpu_pd_release(struct gpowercap *gpowercap);
int __gpc_cpu_setup(int cpu, struct gpowercap *parent, enum hw_dev_type cdev_id);
#endif // _GOOGLE_POWERCAP_CPU_H_

