// SPDX-License-Identifier: GPL-2.0
/*
 * cdev_uclamp.c Cooling device to place thermal uclamp vote.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 *
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include "thermal_core.h"
#include "google_uclamp_cdev.h"
#include "cdev_helper.h"
#include "sched.h"
#include "pixel_em.h"

static int __thermal_uclamp_cpu_opp_table_setup(struct thermal_uclamp_cdev *uclamp_cdev)
{
	struct pixel_em_profile **profile_ptr_snapshot, *profile;
	int i, num_opps;

	profile_ptr_snapshot = READ_ONCE(vendor_sched_pixel_em_profile);
	if (!profile_ptr_snapshot) {
		pr_err("uclamp Error fetching profile snapshot.\n");
		return -ENODATA;
	}
	profile = READ_ONCE(*profile_ptr_snapshot);
	if (!profile) {
		pr_err("uclamp Error fetching profile.\n");
		return -ENODATA;
	}

	if (!profile->cpu_to_cluster[uclamp_cdev->cpu]->num_opps) {
		pr_err("uclamp No OPP values available for cpu:%d\n", uclamp_cdev->cpu);
		return -ENODATA;
	}

	num_opps = profile->cpu_to_cluster[uclamp_cdev->cpu]->num_opps;
	uclamp_cdev->max_state = num_opps - 1;
	uclamp_cdev->opp_table = kcalloc(num_opps,
					   sizeof(*uclamp_cdev->opp_table),
					   GFP_KERNEL);
	if (!uclamp_cdev->opp_table)
		return -ENOMEM;

	for (i = 0; i < profile->cpu_to_cluster[uclamp_cdev->cpu]->num_opps; i++) {
		uclamp_cdev->opp_table[i].freq =
			profile->cpu_to_cluster[uclamp_cdev->cpu]->opps[i].freq;
		uclamp_cdev->opp_table[i].power =
			profile->cpu_to_cluster[uclamp_cdev->cpu]->opps[i].power;
		pr_debug("lvl:%d CPU:%d freq:%u power:%uuw\n",
			i, uclamp_cdev->cpu,
			uclamp_cdev->opp_table[i].freq,
			uclamp_cdev->opp_table[i].power);
	}
	return 0;

}

static int thermal_uclamp_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct thermal_uclamp_cdev *uclamp_cdev = cdev->devdata;

	*state = uclamp_cdev->max_state;

	return 0;
}

static int thermal_uclamp_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct thermal_uclamp_cdev *uclamp_cdev = cdev->devdata;

	mutex_lock(&therm_cdev_list_lock);
	*state = uclamp_cdev->cur_state;
	mutex_unlock(&therm_cdev_list_lock);

	return 0;
}

static int thermal_uclamp_set_cur_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct thermal_uclamp_cdev *uclamp_cdev = cdev->devdata;
	int idx = 0;

	mutex_lock(&therm_cdev_list_lock);
	if (state != uclamp_cdev->cur_state) {
		idx = uclamp_cdev->max_state - state;
		pr_debug("cdev:[%s] new state request:[%lu] frequeny:[%u]\n",
			 cdev->type, state,
			 uclamp_cdev->opp_table[idx].freq);
		uclamp_cdev->cur_state = state;
		sched_thermal_freq_cap(uclamp_cdev->cpu,
				       uclamp_cdev->opp_table[idx].freq);
	}
	mutex_unlock(&therm_cdev_list_lock);


	return 0;
}

static struct thermal_cooling_device_ops thermal_uclamp_cdev_ops = {
	.get_max_state = thermal_uclamp_get_max_state,
	.get_cur_state = thermal_uclamp_get_cur_state,
	.set_cur_state = thermal_uclamp_set_cur_state,
};

ssize_t
state2power_table_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = to_cooling_device(dev);
	struct thermal_uclamp_cdev *uclamp_cdev = cdev->devdata;
	int i, count = 0, idx;
	u32 power;

	if (!uclamp_cdev)
		return -ENODEV;

	for (i = 0; i <= uclamp_cdev->max_state; i++) {
		idx = uclamp_cdev->max_state - i;
		power = uclamp_cdev->opp_table[idx].power;
		count += sysfs_emit_at(buf, count, "%u ", DIV_ROUND_UP(power, 1000));
	}
	count += sysfs_emit_at(buf, count, "\n");

	return count;
}

static DEVICE_ATTR_RO(state2power_table);

static void thermal_uclamp_cleanup(void)
{
	struct thermal_uclamp_cdev *uclamp_cdev = NULL, *n;

	mutex_lock(&therm_cdev_list_lock);
	list_for_each_entry_safe(uclamp_cdev, n, &therm_uclamp_cdev_list, cdev_list) {
		list_del(&uclamp_cdev->cdev_list);
		device_remove_file(&uclamp_cdev->cdev->device,
							&dev_attr_state2power_table);
		if (uclamp_cdev->cdev) {
			pr_info("Un-registered cdev:%s\n",
				uclamp_cdev->cdev->type);
			thermal_cooling_device_unregister(uclamp_cdev->cdev);
			uclamp_cdev->cdev = NULL;
		}

		sched_thermal_freq_cap(
				uclamp_cdev->cpu,
				uclamp_cdev->opp_table[uclamp_cdev->max_state].freq);

		kfree(uclamp_cdev->opp_table);
		kfree(uclamp_cdev);
	}
	mutex_unlock(&therm_cdev_list_lock);
}

static int __init thermal_uclamp_init(void)
{
	int ret = 0;
	unsigned int cpu = 0;
	struct cpufreq_policy *policy = NULL;
	struct thermal_uclamp_cdev *uclamp_cdev = NULL;
	char *name = NULL;

	if (!list_empty(&therm_uclamp_cdev_list)) {
		pr_err("Thermal uclamp cdev already initialized\n");
		return -EALREADY;
	}

	mutex_lock(&therm_cdev_list_lock);
	for (; cpu < num_possible_cpus(); ) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("cpufreq policy %d fetch error. Trying again...\n", cpu);
			ret = -EPROBE_DEFER;
			goto cleanup_exit;
		}

		uclamp_cdev = kzalloc(sizeof(*uclamp_cdev), GFP_KERNEL);
		if (!uclamp_cdev) {
			ret = -ENOMEM;
			goto cleanup_exit;
		}

		uclamp_cdev->cpu = policy->cpu;
		cpu += cpumask_weight(policy->related_cpus);
		cpufreq_cpu_put(policy);
		policy = NULL;

		ret = __thermal_uclamp_cpu_opp_table_setup(uclamp_cdev);
		if (ret)
			goto cleanup_exit;

		list_add_tail(&uclamp_cdev->cdev_list, &therm_uclamp_cdev_list);
		sched_thermal_freq_cap(
				uclamp_cdev->cpu,
				uclamp_cdev->opp_table[uclamp_cdev->max_state].freq);

		name = kasprintf(GFP_KERNEL, "thermal-uclamp-%d", uclamp_cdev->cpu);
		uclamp_cdev->cdev = thermal_cooling_device_register(name, uclamp_cdev,
								    &thermal_uclamp_cdev_ops);
		if (IS_ERR(uclamp_cdev->cdev)) {
			ret = PTR_ERR(uclamp_cdev->cdev);
			pr_err("uclamp cdev:[%s] register error. err:%d\n", name, ret);
			uclamp_cdev->cdev = NULL;
			goto cleanup_exit;
		}

		ret = device_create_file(&uclamp_cdev->cdev->device,
					 &dev_attr_state2power_table);
		if (ret) {
			pr_err("cdev:[%s] state2power attr failed. err:%d\n",
			       name, ret);
			goto cleanup_exit;
		}

		pr_info("Registered cdev:%s\n", name);
	}
	mutex_unlock(&therm_cdev_list_lock);
	return ret;

cleanup_exit:
	mutex_unlock(&therm_cdev_list_lock);
	if (policy) {
		cpufreq_cpu_put(policy);
		policy = NULL;
	}

	thermal_uclamp_cleanup();

	return ret;
}
module_init(thermal_uclamp_init);

static void __exit thermal_uclamp_exit(void)
{
	thermal_uclamp_cleanup();
}
module_exit(thermal_uclamp_exit);

MODULE_DESCRIPTION("Cooling device for placing uclamp max for CPU clusters");
MODULE_AUTHOR("Ram Chandrasekar <rchandrasekar@google.com>");
MODULE_AUTHOR("Peter YM <peterym@google.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:google_thermal_uclamp");
