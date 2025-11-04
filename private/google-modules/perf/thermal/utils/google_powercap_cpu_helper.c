// SPDX-License-Identifier: GPL-2.0-only
/*
 * google_powercap_cpu_helper.c driver providing helper for powercap CPU.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>

#include "cdev_cpufreq_helper.h"
#include "google_powercap.h"
#include "google_powercap_cpu.h"
#include "google_powercap_helper_mock.h"
#include "perf/core/google_pm_qos.h"

LIST_HEAD(gpowercap_cpu_list);

u64 __gpc_cpu_set_cluster_power_limit(struct gpowercap *gpowercap, u64 power_limit)
{
	struct gpowercap_cpu *gpowercap_cpu = to_gpowercap_cpu(gpowercap);
	unsigned int freq;
	int i;

	/* The power limit value here is clamped already using the min and max power.
	 * So they will not be out of bounds and no need to check that case. The index will
	 * start from 1 to avoid redundant check for idx = 0.
	 */
	for (i = 1; i < gpowercap_cpu->num_opps; i++) {
		if (gpowercap_cpu->opp_table[i].power > power_limit)
			break;
	}

	freq = gpowercap_cpu->opp_table[i - 1].freq;
	power_limit = gpowercap_cpu->opp_table[i - 1].power;

	pr_debug("CPU:%d freq limit:%u power limit:%llu",
		 gpowercap_cpu->cpu, freq, power_limit);
	gpc_freq_qos_update_request(&gpowercap_cpu->qos_req, freq);
#if KERNEL_VERSION(6, 12, 0) > LINUX_VERSION_CODE
	gpc_apply_thermal_pressure(gpowercap_cpu->related_cpus, freq,
				   THERMAL_PRESSURE_TYPE_TSKIN);
#endif
	return power_limit;
}

u64 __gpc_cpu_get_cluster_power_uw(struct gpowercap *gpowercap)
{
	struct gpowercap_cpu *gpowercap_cpu = to_gpowercap_cpu(gpowercap);
	unsigned long freq;
	int i;

	freq = gpc_cpufreq_quick_get(gpowercap_cpu->cpu);

	for (i = 0; i < gpowercap_cpu->num_opps; i++) {
		if (gpowercap_cpu->opp_table[i].freq < freq)
			continue;

		pr_debug("CPU:%d freq:%u power:%uuw\n",
			gpowercap_cpu->cpu,
			gpowercap_cpu->opp_table[i].freq,
			gpowercap_cpu->opp_table[i].power);
		return gpowercap_cpu->opp_table[i].power;
	}

	return 0;
}

int __gpc_cpu_update_cluster_power_uw(struct gpowercap *gpowercap)
{
	struct gpowercap_cpu *gpowercap_cpu = to_gpowercap_cpu(gpowercap);

	gpowercap->power_min = gpowercap_cpu->opp_table[0].power;
	gpowercap->power_max = gpowercap_cpu->opp_table[gpowercap_cpu->num_opps - 1].power;
	gpowercap->power_limit = gpowercap->power_max;
	gpowercap->num_opps = gpowercap_cpu->num_opps;
	gpowercap->opp_table = gpowercap_cpu->opp_table;
	pr_debug("CPU%d powercap min power:%llu max power:%llu\n",
		 gpowercap_cpu->cpu, gpowercap->power_min, gpowercap->power_max);

	return 0;
}

void __gpc_cpu_pd_release(struct gpowercap *gpowercap)
{
	struct gpowercap_cpu *gpowercap_cpu = to_gpowercap_cpu(gpowercap);
	struct cpufreq_policy *policy;

	if (!gpowercap_cpu) {
		pr_err("Invalid gpowercap CPU node.\n");
		return;
	}
	pr_info("Releasing QOS request for CPU:%d", gpowercap_cpu->cpu);
	if (freq_qos_request_active(&gpowercap_cpu->qos_req)) {
		policy = gpc_cpufreq_get_policy(gpowercap_cpu->cpu);
		if (policy) {
			gpc_pm_qos_remove_cpufreq_request(policy, &gpowercap_cpu->qos_req);
			gpc_cpufreq_cpu_put(policy);
		} else
			pr_warn("CPU:%d cpufreq policy get error. QOS unregister skip.\n",
				gpowercap_cpu->cpu);
	}

	list_del(&gpowercap_cpu->node);
	kfree(gpowercap_cpu->opp_table);
	kfree(gpowercap_cpu);
}

static struct gpowercap_ops gpowercap_cpu_ops = {
	.set_power_uw	 = __gpc_cpu_set_cluster_power_limit,
	.get_power_uw	 = __gpc_cpu_get_cluster_power_uw,
	.update_power_uw = __gpc_cpu_update_cluster_power_uw,
	.release	 = __gpc_cpu_pd_release,
};

int __gpc_cpu_setup(int cpu, struct gpowercap *parent, enum hw_dev_type cdev_id)
{
	struct gpowercap_cpu *gpowercap_cpu;
	struct cpufreq_policy *policy;
	char name[CPUFREQ_NAME_LEN];
	int ret = 0;
	struct list_head *pos;

	policy = gpc_cpufreq_get_policy(cpu);
	if (!policy)
		return -ENODEV;

	list_for_each(pos, &gpowercap_cpu_list) {
		struct gpowercap_cpu *p_cpu = list_entry(pos, struct gpowercap_cpu, node);

		if (cpumask_test_cpu(p_cpu->cpu, policy->related_cpus)) {
			pr_warn("powercap for cpu:[%d] already initialized.\n", cpu);
			goto release_policy;
		}
	}

	gpowercap_cpu = kzalloc(sizeof(*gpowercap_cpu), GFP_KERNEL);
	if (!gpowercap_cpu) {
		ret = -ENOMEM;
		goto release_policy;
	}

	gpowercap_init(&gpowercap_cpu->gpowercap, &gpowercap_cpu_ops);
	gpowercap_cpu->cpu = cpu;
	cpumask_copy(&gpowercap_cpu->related_cpus, policy->related_cpus);

	ret = gpc_cdev_cpufreq_get_opp_count(cpu);
	if (ret <= 0) {
		ret = ret ? : -ENODEV;
		goto out_kfree_gpowercap_cpu;
	}
	gpowercap_cpu->num_opps = ret;
	gpowercap_cpu->opp_table = kcalloc(gpowercap_cpu->num_opps,
					   sizeof(*gpowercap_cpu->opp_table),
					   GFP_KERNEL);
	if (!gpowercap_cpu->opp_table) {
		ret = -ENOMEM;
		goto out_kfree_gpowercap_cpu;
	}
	ret = gpc_cdev_cpufreq_update_opp_table(cpu, cdev_id,
						gpowercap_cpu->opp_table,
						gpowercap_cpu->num_opps);
	if (ret)
		goto out_kfree_power_table;

	snprintf(name, sizeof(name), "cpufreq-cpu%d", gpowercap_cpu->cpu);

	ret = gpc_gpowercap_register(name, &gpowercap_cpu->gpowercap, parent);
	if (ret)
		goto out_kfree_power_table;

	ret = gpc_pm_qos_add_cpufreq_request(
			policy,
			&gpowercap_cpu->qos_req,
			FREQ_QOS_MAX,
			gpowercap_cpu->opp_table[gpowercap_cpu->num_opps - 1].freq);
	if (ret < 0) {
		pr_err("QOS request init error:%d\n", ret);
		goto out_gpowercap_unregister;
	}

	gpc_cpufreq_cpu_put(policy);
	list_add_tail(&gpowercap_cpu->node, &gpowercap_cpu_list);
	return 0;

out_gpowercap_unregister:
	gpc_cpufreq_cpu_put(policy);
	gpowercap_unregister(&gpowercap_cpu->gpowercap);
	return ret;

out_kfree_power_table:
	kfree(gpowercap_cpu->opp_table);

out_kfree_gpowercap_cpu:
	kfree(gpowercap_cpu);

release_policy:
	gpc_cpufreq_cpu_put(policy);
	return ret;
}
