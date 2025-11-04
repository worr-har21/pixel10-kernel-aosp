// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Google LLC */

#include <linux/container_of.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/stringify.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>

#include "pd_latency_profile.h"
#include "power_controller.h"

static struct power_controller *pcdbg;
static bool module_initialized;

static inline struct power_controller *to_power_controller(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct platform_device *pdev = to_platform_device(dev);

	return platform_get_drvdata(pdev);
}

static struct latency_data *find_latency_record(struct kobject *kobj)
{
	/* on/off->power_domain->latency_stats->power_controller */
	struct power_controller *pc =
		to_power_controller(kobj->parent->parent->parent);

	struct latency_profile *latency = pc->latency;

	for (int i = 0; i < latency->pd_count * LATENCY_TYPE_COUNT; i++) {
		if (latency->data[i].parent == kobj)
			return &latency->data[i];
	}

	return NULL;
}

static ssize_t min_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	struct latency_data *data = find_latency_record(kobj);

	if (!data)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", data->min_latency);
}

static ssize_t max_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	struct latency_data *data = find_latency_record(kobj);

	if (!data)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", data->max_latency);
}

static ssize_t average_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	struct latency_data *data = find_latency_record(kobj);

	if (!data)
		return -EINVAL;

	return sysfs_emit(buf, "%llu\n", data->latency_total_sum / data->count);
}

static ssize_t count_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct latency_data *data = find_latency_record(kobj);

	if (!data)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", data->count);
}

static struct kobj_attribute stats_attrs[] = {
	__ATTR(min, 0444, min_show, NULL),
	__ATTR(max, 0444, max_show, NULL),
	__ATTR(average, 0444, average_show, NULL),
	__ATTR(count, 0444, count_show, NULL),
	__ATTR_NULL,
};

static struct attribute *attrs[] = {
	&stats_attrs[0].attr,
	&stats_attrs[1].attr,
	&stats_attrs[2].attr,
	&stats_attrs[3].attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static ssize_t collect_latency_data_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	/* latency_stats->power_controller */
	struct power_controller *pc = to_power_controller(kobj->parent);

	return sysfs_emit(buf, "%s\n",
			  pc->latency->collect_data ? "on" : "off");
}

static ssize_t collect_latency_data_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	/* latency_stats->power_controller */
	struct power_controller *pc = to_power_controller(kobj->parent);

	if (kstrtobool(buf, &pc->latency->collect_data))
		return -EINVAL;

	return count;
}
static struct kobj_attribute collect_latency_data_attr =
	__ATTR(collect_latency_data, 0644, collect_latency_data_show,
	       collect_latency_data_store);

static ssize_t reset_latencies_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	/* latency_stats->power_controller */
	struct power_controller *pc = to_power_controller(kobj->parent);

	for (int i = 0; i < pc->latency->pd_count * LATENCY_TYPE_COUNT; ++i) {
		pc->latency->data[i].latency_total_sum = 0;
		pc->latency->data[i].max_latency = 0;
		pc->latency->data[i].min_latency = 0;
		pc->latency->data[i].count = 0;
	}

	return count;
}

/* create_stats_directory() - Create a directory that contains the stats
 *                            and register it with sysfs.
 *
 * @name: the name of the directory
 * @power_domain_kobj: the parent kobject of the data_kobj
 * @attr_group: the attribute group to register to the sysfs
 */
static struct kobject *create_stats_directory(const char *name,
					      struct kobject *power_domain_kobj,
					      struct attribute_group *attr_group)
{
	struct kobject *data_kobj =
		kobject_create_and_add(name, power_domain_kobj);

	if (!data_kobj)
		return NULL;

	if (sysfs_create_group(data_kobj, attr_group))
		return NULL;

	return data_kobj;
}

static struct kobj_attribute reset_latencies_attr =
	__ATTR(reset_latencies, 0200, NULL, reset_latencies_store);

static int pd_latency_index(struct power_domain *pd)
{
	int idx;
	struct power_controller *pc = pd->power_controller;

	for (idx = 0; idx < pc->pd_count; idx++)
		if (pc->latency->genpd_sswrp_ids[idx] == pd->genpd_sswrp_id)
			return idx * LATENCY_TYPE_COUNT;

	return -EINVAL;
}

/* mask should have 0s on every bit reresenting the state*/
/* example: PD_STATE_ON has 0s on *_ON bits */
static const u16 state_masks[] = {
	[PD_STATE_ON] = 0xAAAA,
	[PD_STATE_OFF] = 0x5555
};

int pd_latency_profile_start(struct generic_pm_domain *domain, u16 type_mask)
{
	struct power_domain *pd =
		container_of(domain, struct power_domain, genpd);
	struct power_controller *pc = pd->power_controller;
	int idx;
	u64 start_time;
	enum latency_type lat_type;

	if (!module_initialized)
		return -ECANCELED;

	if (type_mask & LATENCY_TYPE_BITMASK(FW_ON) ||
		type_mask & LATENCY_TYPE_BITMASK(FW_OFF))
		return -EINVAL;

	if (!pc->latency || !pc->latency->collect_data)
		return 0;

	/* we won't start profiling if the device is already in a same state */
	type_mask &= state_masks[pd->state];
	if (type_mask == 0) {
		dev_dbg(pc->dev, "Same state/none profiling requested. Skipping");
		return 0;
	}

	idx = pd_latency_index(pd);
	if (idx < 0)
		return idx;

	start_time = sched_clock();
	for (lat_type = 0; lat_type < LATENCY_TYPE_COUNT; ++lat_type) {
		if (type_mask & BIT(lat_type))
			pc->latency->data[idx + lat_type].start_time = start_time;
	}

	return 0;
}

static void __pd_latency_profile_store(int idx, u64 latency)
{
	struct latency_data *data = &pcdbg->latency->data[idx];

	data->latency_total_sum += latency;
	if (latency > data->max_latency)
		data->max_latency = latency;
	if (latency < data->min_latency || data->min_latency == 0)
		data->min_latency = latency;

	data->count++;
}

int pd_latency_profile_stop(struct generic_pm_domain *domain, u16 type_mask, bool cancel)
{
	struct power_domain *pd =
		container_of(domain, struct power_domain, genpd);
	struct power_controller *pc = pd->power_controller;
	int idx;
	u64 end_time, delta;
	enum latency_type lat_type;

	if (!module_initialized)
		return -ECANCELED;

	idx = pd_latency_index(pd);
	if (idx < 0)
		return idx;

	end_time = sched_clock();
	for (lat_type = 0; lat_type < LATENCY_TYPE_COUNT; ++lat_type) {
		if (type_mask & BIT(lat_type)) {
			int lat_idx = idx + lat_type;

			/* there was no start call before the stop call */
			if (pc->latency->data[lat_idx].start_time == 0)
				continue;

			/* if the cancel flag is set, we won't store the latency */
			if (!cancel) {
				delta = (end_time - pc->latency->data[lat_idx].start_time) / 1000;

				__pd_latency_profile_store(lat_idx, delta);
			}

			/* signal that now the start call needs to be invoked */
			pc->latency->data[lat_idx].start_time = 0;
		}
	}

	return 0;
}

int pd_latency_profile_store(struct generic_pm_domain *domain, u64 latency,
		enum latency_type lat_type)
{
	struct power_domain *pd =
		container_of(domain, struct power_domain, genpd);
	struct power_controller *pc = pd->power_controller;
	int idx;

	if (!module_initialized)
		return -ECANCELED;

	if (!pc->latency || !pc->latency->collect_data)
		return 0;

	idx = pd_latency_index(pd);
	if (idx < 0)
		return idx;

	__pd_latency_profile_store(idx + lat_type, latency);

	return 0;
}

int pd_latency_profile_init(struct platform_device *pdev)
{
	struct power_controller *pc = platform_get_drvdata(pdev);
	struct device_node *child_np, *np = pdev->dev.of_node;
	int ret, idx_pd, idx_data, i;

	pcdbg = pc;

	pc->latency = devm_kzalloc(pc->dev, sizeof(struct latency_profile),
				   GFP_KERNEL);
	if (!pc->latency)
		return -ENOMEM;

	struct latency_profile *latency = pc->latency;

	latency->pd_count = 0;
	for_each_available_child_of_node(np, child_np)
		latency->pd_count++;

	latency->data = devm_kcalloc(pc->dev,
				     latency->pd_count * LATENCY_TYPE_COUNT,
				     sizeof(struct latency_data), GFP_KERNEL);
	if (!latency->data)
		return -ENOMEM;

	latency->data_kobj =
		devm_kcalloc(pc->dev, latency->pd_count * LATENCY_TYPE_COUNT,
			     sizeof(struct kobject *), GFP_KERNEL);
	if (!latency->data_kobj)
		return -ENOMEM;

	latency->power_domain_kobj = devm_kcalloc(pc->dev, latency->pd_count,
						  sizeof(struct kobject *),
						  GFP_KERNEL);
	if (!latency->power_domain_kobj)
		return -ENOMEM;

	latency->genpd_sswrp_ids =
		devm_kzalloc(pc->dev, sizeof(u32) * latency->pd_count,
		GFP_KERNEL);
	if (!latency->genpd_sswrp_ids)
		return -ENOMEM;

	latency->latencies_kobj =
		kobject_create_and_add("latency_stats", &pc->dev->kobj);
	if (!latency->latencies_kobj)
		return -ENOMEM;

	idx_pd = 0;
	idx_data = 0;
	for_each_available_child_of_node(np, child_np) {
		/* Create power domain directory */
		latency->power_domain_kobj[idx_pd] =
			kobject_create_and_add(child_np->name,
					       latency->latencies_kobj);

		if (!latency->power_domain_kobj[idx_pd])
			goto remove_sysfs_groups;

		/* Create switch on/off/fw stats directory */
		for (i = 0; i < LATENCY_TYPE_COUNT; ++i) {
			latency->data_kobj[idx_data] =
				create_stats_directory(profiled_stat_name[i],
						       latency->power_domain_kobj
							       [idx_pd],
						       &attr_group);
			if (!latency->data_kobj[idx_data])
				goto remove_sysfs_groups;

			latency->data[idx_data].parent =
				latency->data_kobj[idx_data];
			idx_data++;
		}

		latency->genpd_sswrp_ids[idx_pd] = pc->pds[idx_pd].genpd_sswrp_id;

		idx_pd++;
	}

	latency->collect_data = 0;
	if (sysfs_create_file(latency->latencies_kobj,
			      &reset_latencies_attr.attr)) {
		dev_err(pc->dev,
			"Couldn't create collect_latency_stats sysfs node.\n");
		goto remove_sysfs_groups;
	}

	ret = sysfs_create_file(latency->latencies_kobj,
				&collect_latency_data_attr.attr);
	if (ret) {
		dev_err(pc->dev,
			"Couldn't create reset_latencies sysfs node.\n");
		goto remove_control_files;
	}

	module_initialized = true;

	return 0;

remove_control_files:
	sysfs_remove_file(latency->latencies_kobj,
			  &collect_latency_data_attr.attr);
remove_sysfs_groups:
	for (; idx_data >= 0; idx_data--) {
		sysfs_remove_group(latency->data_kobj[idx_data], &attr_group);
		kobject_put(latency->power_domain_kobj[idx_data]);
	}
	for (; idx_pd >= 0; idx_pd--)
		kobject_put(latency->power_domain_kobj[idx_pd]);

	kobject_put(latency->latencies_kobj);

	return -ENOMEM;
}

int pd_latency_profile_remove(struct platform_device *pdev)
{
	struct power_controller *pc = platform_get_drvdata(pdev);
	int idx_pd = pc->latency->pd_count - 1;
	int idx_data = pc->latency->pd_count * LATENCY_TYPE_COUNT - 1;

	sysfs_remove_file(pc->latency->latencies_kobj,
			  &reset_latencies_attr.attr);
	sysfs_remove_file(pc->latency->latencies_kobj,
			  &collect_latency_data_attr.attr);
	for (; idx_data >= 0; idx_data--) {
		sysfs_remove_group(pc->latency->data_kobj[idx_data],
				   &attr_group);
		kobject_put(pc->latency->power_domain_kobj[idx_data]);
	}
	for (; idx_pd >= 0; idx_pd--)
		kobject_put(pc->latency->power_domain_kobj[idx_pd]);

	kobject_put(pc->latency->latencies_kobj);

	module_initialized = false;

	return 0;
}
