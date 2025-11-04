// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright 2024 Google LLC
 */
#include "linux/container_of.h"
#include "linux/device.h"
#include "linux/gfp_types.h"
#include "linux/kobject.h"
#include "linux/kstrtox.h"
#include "linux/list.h"
#include "linux/sysfs.h"
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/moduleparam.h>
#include <linux/minmax.h>
#include <linux/smp.h>

#include "google_cpu_lpm_stats.h"
#include "soc/google/google_gtc.h"

static LIST_HEAD(cpu_lpm_list);
#define CPU_LPM_ELEM_ATTR(_name, _mode)                  \
	struct attribute cpu_lpm_attr_##_name = {        \
		.name = __stringify(_name),              \
		.mode = VERIFY_OCTAL_PERMISSIONS(_mode), \
	}

static CPU_LPM_ELEM_ATTR(min, 0444);
static CPU_LPM_ELEM_ATTR(max, 0444);
static CPU_LPM_ELEM_ATTR(count, 0444);
static CPU_LPM_ELEM_ATTR(total_residency, 0444);
static CPU_LPM_ELEM_ATTR(average, 0444);

static ssize_t collect_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	bool collect = false;
	struct cpu_lpm_data *tmp_data;

	list_for_each_entry(tmp_data, &cpu_lpm_list, list) {
		spin_lock(&tmp_data->lock);
		if (tmp_data->collect_data) {
			collect = true;
			spin_unlock(&tmp_data->lock);
			break;
		}
		spin_unlock(&tmp_data->lock);
	}

	return sysfs_emit(buf, "%s\n", collect ? "on" : "off");
}

static ssize_t collect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cpu_lpm_data *tmp_data;
	bool user_input = false;

	if (kstrtobool(buf, &user_input))
		return -EINVAL;

	list_for_each_entry(tmp_data, &cpu_lpm_list, list) {
		spin_lock(&tmp_data->lock);
		tmp_data->collect_data = user_input;
		spin_unlock(&tmp_data->lock);
	}

	return count;
}

static DEVICE_ATTR_RW(collect);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct cpu_lpm_data *tmp_data;

	list_for_each_entry(tmp_data, &cpu_lpm_list, list) {
		spin_lock(&tmp_data->lock);
		memset(&tmp_data->data, 0, sizeof(tmp_data->data));
		memset(&tmp_data->latency_data, 0, sizeof(struct cpu_latency_stats_elem));
		spin_unlock(&tmp_data->lock);
	}

	return count;
}

static DEVICE_ATTR_WO(reset);

/* attribute groups for each power domain */
static struct attribute *cpu_lpm_stats_attrs[] = {
	&cpu_lpm_attr_min,
	&cpu_lpm_attr_max,
	&cpu_lpm_attr_count,
	&cpu_lpm_attr_total_residency,
	&cpu_lpm_attr_average,
	NULL
};

ATTRIBUTE_GROUPS(cpu_lpm_stats);

static ssize_t show_cpu_lpm_stats(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct cpu_lpm_data *lat =
		container_of(kobj, struct cpu_lpm_data, pd_residency_data);
	u64 val = 0;

	if (attr == &cpu_lpm_attr_min)
		val = lat->data.min;
	else if (attr == &cpu_lpm_attr_max)
		val = lat->data.max;
	else if (attr == &cpu_lpm_attr_count)
		val = lat->data.count;
	else if (attr == &cpu_lpm_attr_total_residency)
		val = lat->data.total_residency;
	else if (attr == &cpu_lpm_attr_average) {
		if (lat->data.count == 0)
			val = 0;
		val = lat->data.total_residency / lat->data.count;
	}

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static const struct sysfs_ops cpu_lpm_stats_ops = {
	.show = &show_cpu_lpm_stats,
};

static const struct kobj_type cpu_lpm_stats_kobj_type = {
	.sysfs_ops = &cpu_lpm_stats_ops,
	.default_groups = cpu_lpm_stats_groups,
};

#define LATENCY_STATS_ATTR(_name, _mode)                  \
	struct attribute latency_stats_attr_##_name = {        \
		.name = __stringify(_name),              \
		.mode = VERIFY_OCTAL_PERMISSIONS(_mode), \
	}

static LATENCY_STATS_ATTR(min, 0444);
static LATENCY_STATS_ATTR(max, 0444);
static LATENCY_STATS_ATTR(avg, 0444);
static LATENCY_STATS_ATTR(count, 0444);

/*
 * latency stats attribute groups for each
 * latency type for each power domain
 */
static struct attribute *latency_stats_attrs[] = {
	&latency_stats_attr_avg,
	&latency_stats_attr_max,
	&latency_stats_attr_min,
	&latency_stats_attr_count,
	NULL
};

ATTRIBUTE_GROUPS(latency_stats);

static u64 get_latency_stats_attr(struct latency_stats *stats,
				       struct attribute *attr)
{
	u64 val = 0;

	if (attr == &latency_stats_attr_avg)
		val = stats->avg;
	else if (attr == &latency_stats_attr_max)
		val = stats->max;
	else if (attr == &latency_stats_attr_min)
		val = stats->min;
	return val;
}

static ssize_t show_latency_stats(struct kobject *kobj, struct attribute *attr,
				  char *buf)
{
	struct cpu_lpm_data *lat =
		container_of(kobj->parent, struct cpu_lpm_data, pd_latency_data);
	u64 val = 0;

	if (attr == &latency_stats_attr_count)
		val = lat->data.count;
	else if (kobj == &lat->pd_fw_entry_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_entry,
					       attr);
	else if (kobj == &lat->pd_fw_exit_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_exit,
					       attr);
	else if (kobj == &lat->pd_fw_sched_entry_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_sched_entry,
					       attr);
	else if (kobj == &lat->pd_fw_sched_exit_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_sched_exit,
					       attr);
	else if (kobj == &lat->pd_fw_entry_transition_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_entry_transition,
					       attr);
	else if (kobj == &lat->pd_fw_exit_transition_stats)
		val = get_latency_stats_attr(&lat->latency_data.fw_exit_transition,
					       attr);
	else if (kobj == &lat->pd_e2e_entry_stats)
		val = get_latency_stats_attr(&lat->latency_data.e2e_entry,
					       attr);
	else if (kobj == &lat->pd_e2e_exit_stats)
		val = get_latency_stats_attr(&lat->latency_data.e2e_exit,
					       attr);
	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static const struct sysfs_ops latency_stats_ops = {
	.show = &show_latency_stats,
};

static const struct kobj_type latency_stats_kobj_type = {
	.sysfs_ops = &latency_stats_ops,
	.default_groups = latency_stats_groups,
};

static const struct kobj_type latency_data_kobj_type = {};

static void process_latency_stats(struct latency_stats *stats, u64 latency_val,
			   u64 count)
{
	stats->max = max(stats->max, latency_val);
	if (stats->min == 0 || latency_val < stats->min)
		stats->min = latency_val;

	stats->avg = ((count - 1) * stats->avg + latency_val) / count;
}

static struct cpu_lpm_data *get_exiting_cpu_pd_lpm_data(void)
{
	struct cpu_lpm_data *lat;
	int current_cpu_id = smp_processor_id();

	list_for_each_entry(lat, &cpu_lpm_list, list) {
		if (lat->core_id == current_cpu_id)
			return lat;
	}

	return NULL;
}

static struct cpu_lpm_data *get_exiting_cpu_cluster_pd_lpm_data(
	struct cpu_lpm_data *cpu_pd_lpm_stats)
{
	struct cpu_lpm_data *lat;

	list_for_each_entry(lat, &cpu_lpm_list, list) {
		if (lat->power_domain_phandle == cpu_pd_lpm_stats->cluster_pd_phandle)
			return lat;
	}

	return NULL;
}

static void update_stats(struct device *dev,
			const char *pm_domain_name,
			struct cpu_lpm_data *lat,
			struct cpu_lpm_stats_elem *data,
			struct cpu_latency_stats_elem *latency_data,
			u64 now,
			struct timestamps_buffer *ts_buff)
{
	u64 fw_entry, fw_exit, e2e_entry, e2e_exit, delta, fw_sched_exit,
		fw_exit_transition, fw_sched_entry, fw_entry_transition;
	u64 last_entry_start_ts, last_exit_end_ts;
	struct timestamps_buffer secondary_ts_buff;
	struct cpu_lpm_data *exiting_cpu_lpm_data = get_exiting_cpu_pd_lpm_data();
	struct cpu_lpm_data *exiting_cpu_cluster_lpm_stats;

	if (!exiting_cpu_lpm_data)
		return;

	if (exiting_cpu_lpm_data == lat ||
		(exiting_cpu_lpm_data->cluster_pd_phandle == lat->power_domain_phandle)) {
		last_entry_start_ts = ts_buff->last_entry_start_ts;
		last_exit_end_ts = ts_buff->last_exit_end_ts;
	} else {
		exiting_cpu_cluster_lpm_stats = get_exiting_cpu_cluster_pd_lpm_data(
			exiting_cpu_lpm_data);

		if (!exiting_cpu_cluster_lpm_stats)
			return;

		if (read_cpu_pd_latency_stats(
			exiting_cpu_cluster_lpm_stats->power_state,
			&secondary_ts_buff))
			return;

		last_entry_start_ts = secondary_ts_buff.last_entry_start_ts;
		last_exit_end_ts = secondary_ts_buff.last_exit_end_ts;
	}

	/*
	 * This is a case when ON notification is received
	 * before any OFF was received. It has to be ignored
	 * since it would corrupt avg, max and min data
	 */
	if (data->last_entry_ts == 0)
		return;

	data->last_exit_ts = now;
	if (now < data->last_entry_ts)
		return;

	// This is a rejection
	if (ts_buff->last_entry_end_ts < last_entry_start_ts
		|| ts_buff->last_entry_end_ts < data->last_entry_ts) {
		return;
	}

	if (last_exit_end_ts < ts_buff->last_exit_start_ts) {
		dev_err(dev,
			"Exit end ts lower than exit start ts for power domain %s\n",
			pm_domain_name);
		return;
	}

	if (ts_buff->last_exit_sched_ts != 0 && (
		ts_buff->last_exit_sched_ts > ts_buff->last_exit_start_ts ||
		ts_buff->last_exit_sched_ts < ts_buff->last_entry_end_ts))
		return;

	if (ts_buff->last_entry_sched_ts != 0 &&
		ts_buff->last_entry_sched_ts > last_entry_start_ts)
		return;

	data->count++;
	delta = now - data->last_entry_ts;
	data->total_residency += delta;
	if (delta > data->max)
		data->max = delta;

	if (delta < data->min || data->min == 0)
		data->min = delta;

	/*
	 * Entry/Exit start timestamps collection might not
	 * be supported for some power domains. In such cases
	 * hw latency calculations must be skipped and e2e exit
	 * latency must be calculated using exit end timestamp.
	 */
	if (ts_buff->last_entry_sched_ts > 0) {
		fw_sched_entry =
			last_entry_start_ts - ts_buff->last_entry_sched_ts;
		fw_entry_transition =
			ts_buff->last_entry_end_ts - last_entry_start_ts;
		fw_entry =
			ts_buff->last_entry_end_ts - ts_buff->last_entry_sched_ts;
		process_latency_stats(
			&latency_data->fw_sched_entry, fw_sched_entry, data->count);
		process_latency_stats(&latency_data->fw_entry_transition,
					fw_entry_transition, data->count);
		process_latency_stats(&latency_data->fw_entry, fw_entry, data->count);
	} else if (last_entry_start_ts > 0) {
		fw_entry =
			ts_buff->last_entry_end_ts - last_entry_start_ts;
		process_latency_stats(&latency_data->fw_entry, fw_entry, data->count);
	}

	e2e_entry = ts_buff->last_entry_end_ts - data->last_entry_ts;

	if (ts_buff->last_exit_sched_ts > 0) {
		fw_sched_exit =
			ts_buff->last_exit_start_ts - ts_buff->last_exit_sched_ts;
		fw_exit_transition =
			last_exit_end_ts - ts_buff->last_exit_start_ts;
		fw_exit = last_exit_end_ts - ts_buff->last_exit_sched_ts;
		process_latency_stats(
			&latency_data->fw_sched_exit, fw_sched_exit, data->count);
		process_latency_stats(&latency_data->fw_exit_transition,
					fw_exit_transition, data->count);
		process_latency_stats(&latency_data->fw_exit, fw_exit, data->count);
		e2e_exit = data->last_exit_ts - ts_buff->last_exit_sched_ts;
	} else if (ts_buff->last_exit_start_ts > 0) {
		fw_exit = last_exit_end_ts - ts_buff->last_exit_start_ts;
		process_latency_stats(&latency_data->fw_exit, fw_exit, data->count);
		e2e_exit =
			data->last_exit_ts - ts_buff->last_exit_start_ts;
	} else {
		e2e_exit = data->last_exit_ts - last_exit_end_ts;
	}

	process_latency_stats(&latency_data->e2e_entry, e2e_entry, data->count);
	process_latency_stats(&latency_data->e2e_exit, e2e_exit, data->count);
}

static int sswrp_cluster_pd_notifier(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	u64 now = goog_gtc_get_counter();
	struct cpu_lpm_data *lat = container_of(nb, struct cpu_lpm_data, nb);
	struct timestamps_buffer ts_buff;

	spin_lock(&lat->lock);
	if (!lat->collect_data) {
		spin_unlock(&lat->lock);
		return NOTIFY_OK;
	}

	if (action == GENPD_NOTIFY_OFF) {
		lat->data.last_entry_ts = now;
	} else if (action == GENPD_NOTIFY_ON) {
		if (read_cpu_pd_latency_stats(lat->power_state, &ts_buff)) {
			spin_unlock(&lat->lock);
			return NOTIFY_OK;
		}
		update_stats(lat->genpd_dev,
			lat->name, lat, &lat->data, &lat->latency_data, now, &ts_buff);
	}

	spin_unlock(&lat->lock);
	return NOTIFY_OK;
}

static int init_and_add_latency_stats(struct kobject *pd_latency_stats,
				      struct device *dev,
				      struct kobject *pd_latency_data,
				      const char *name)
{
	int err = kobject_init_and_add(pd_latency_stats,
				   &latency_stats_kobj_type,
				   pd_latency_data,
				   "%s", name);
	if (err) {
		dev_err(dev,  "Error %d during kobj init_and_add of %s\n", err,
			name);
	}
	return err;
}

static const struct kobj_type pd_dir_kobj_type = {};

static int init_core_specific_data(struct device *dev,
								struct cpu_lpm_data *lat)
{
	struct device_node *curr_cpu;
	struct device_node *curr_cpu_pd;
	struct device_node *curr_cpu_clustr_pd;
	int cpu_id;

	if (!lat->power_domain_phandle) {
		dev_err(dev,
			"Set power_domain_phandle before calling this function\n");
		return -EINVAL;
	}

	for (curr_cpu = of_get_next_cpu_node(NULL); curr_cpu;
		curr_cpu = of_get_next_cpu_node(curr_cpu)) {
		curr_cpu_pd = of_parse_phandle(curr_cpu, "power-domains", 0);

		if (!curr_cpu_pd) {
			dev_err(dev, "Unable to get current cpu power domain\n");
			return -EINVAL;
		}

		if (curr_cpu_pd != lat->power_domain_phandle)
			continue;

		curr_cpu_clustr_pd = of_parse_phandle(
			curr_cpu_pd, "power-domains", 0);

		if (!curr_cpu_clustr_pd) {
			dev_err(dev, "Unable to get current cluster power domain\n");
			return -EINVAL;
		}

		cpu_id = of_cpu_node_to_id(curr_cpu);

		if (cpu_id < 0) {
			dev_err(dev, "Unable to get current cpu id\n");
			return -ENODEV;
		}

		lat->cluster_pd_phandle = curr_cpu_clustr_pd;
		lat->core_id = cpu_id;
		return 0;
	}

	lat->cluster_pd_phandle = NULL;
	lat->core_id = -1;

	return 0;
}

static int register_device_to_genpd(struct device *dev)
{
	int err;
	struct cpu_lpm_data *lat;
	struct device *genpd_dev;
	struct device_node *power_domain_phandle;
	u32 power_state;

	genpd_dev = genpd_dev_pm_attach_by_id(dev, 0);

	if (!genpd_dev)
		return -ENODEV;

	lat = kzalloc(sizeof(*lat), GFP_KERNEL);
	if (!lat)
		return -ENOMEM;

	lat->name = dev->init_name;
	lat->genpd_dev = genpd_dev;
	lat->nb.notifier_call = sswrp_cluster_pd_notifier;

	memset(&lat->latency_data, 0, sizeof(struct cpu_latency_stats_elem));

	err = of_property_read_u32(dev->of_node, "power-state", &power_state);
	if (err) {
		dev_err(dev,
			"Error %d during read power-state property", err);
		return -EINVAL;
	}

	lat->power_state = power_state;

	power_domain_phandle = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!power_domain_phandle) {
		dev_err(dev, "Error during read power-domains property");
		return -EINVAL;
	}

	lat->power_domain_phandle = power_domain_phandle;
	err = init_core_specific_data(dev, lat);
	if (err) {
		dev_err(dev, "Error during init core specific data\n");
		return err;
	}

	spin_lock_init(&lat->lock);

	err = dev_pm_genpd_add_notifier(genpd_dev, &lat->nb);
	if (err) {
		dev_err(dev, "Error %d during add notifier of %s\n", err,
			lat->name);
		goto err;
	}

	err = kobject_init_and_add(&lat->pd_dir, &pd_dir_kobj_type,
				   &dev->kobj, "%s", lat->name);
	if (err) {
		dev_err(dev, "Error %d during kobj init_and_add of %s\n", err,
			lat->name);
		goto err;
	}

	err = kobject_init_and_add(&lat->pd_residency_data,
				   &cpu_lpm_stats_kobj_type, &lat->pd_dir,
				   "residency_data");
	if (err) {
		dev_err(dev, "Error %d during kobj init_and_add of %s\n", err,
			lat->name);
		goto residency_data_err;
	}

	err = kobject_init_and_add(&lat->pd_latency_data,
				   &latency_data_kobj_type, &lat->pd_dir,
				   "latency_data");
	if (err) {
		dev_err(dev, "Error %d during kobj init_and_add of %s\n", err,
			lat->name);
		goto latency_data_err;
	}

	err = init_and_add_latency_stats(&lat->pd_fw_entry_stats,
					 dev, &lat->pd_latency_data, "fw_entry");
	if (err)
		goto fw_entry_stats_err;

	err = init_and_add_latency_stats(&lat->pd_fw_exit_stats,
					 dev, &lat->pd_latency_data, "fw_exit");
	if (err)
		goto fw_exit_stats_err;

	err = init_and_add_latency_stats(&lat->pd_fw_sched_entry_stats,
					 dev, &lat->pd_latency_data,
					 "fw_sched_entry");
	if (err)
		goto fw_sched_entry_stats_err;

	err = init_and_add_latency_stats(&lat->pd_fw_sched_exit_stats,
					 dev, &lat->pd_latency_data,
					 "fw_sched_exit");
	if (err)
		goto fw_sched_exit_stats_err;

	err = init_and_add_latency_stats(&lat->pd_fw_entry_transition_stats,
					 dev, &lat->pd_latency_data,
					 "fw_entry_transition");
	if (err)
		goto fw_entry_transition_stats_err;

	err = init_and_add_latency_stats(&lat->pd_fw_exit_transition_stats,
					 dev, &lat->pd_latency_data,
					 "fw_exit_transition");
	if (err)
		goto fw_exit_transition_stats_err;

	err = init_and_add_latency_stats(&lat->pd_e2e_entry_stats,
					 dev, &lat->pd_latency_data, "e2e_entry");
	if (err)
		goto e2e_entry_stats_err;

	err = init_and_add_latency_stats(&lat->pd_e2e_exit_stats,
					 dev, &lat->pd_latency_data, "e2e_exit");
	if (err)
		goto e2e_exit_stats_err;

	list_add(&lat->list, &cpu_lpm_list);

	return err;
err:
	dev_pm_genpd_remove_notifier(dev);
	kfree(lat);
	return err;
e2e_exit_stats_err:
	kobject_put(&lat->pd_e2e_entry_stats);
e2e_entry_stats_err:
	kobject_put(&lat->pd_fw_exit_transition_stats);
fw_exit_transition_stats_err:
	kobject_put(&lat->pd_fw_entry_transition_stats);
fw_entry_transition_stats_err:
	kobject_put(&lat->pd_fw_sched_exit_stats);
fw_sched_exit_stats_err:
	kobject_put(&lat->pd_fw_sched_entry_stats);
fw_sched_entry_stats_err:
	kobject_put(&lat->pd_fw_exit_stats);
fw_exit_stats_err:
	kobject_put(&lat->pd_fw_entry_stats);
fw_entry_stats_err:
	kobject_put(&lat->pd_latency_data);
latency_data_err:
	kobject_put(&lat->pd_residency_data);
residency_data_err:
	kobject_put(&lat->pd_dir);
	dev_pm_genpd_remove_notifier(dev);
	kfree(lat);
	return err;
}

static int cpu_lpm_stats_probe(struct platform_device *pdev)
{
	int err;
	struct device_node *nb, *nb_bk;

	err = sysfs_create_file(&pdev->dev.kobj, &dev_attr_collect.attr);
	if (err) {
		dev_err(&pdev->dev, "Error during collect sysfs creation\n");
		return -EINVAL;
	}

	err = sysfs_create_file(&pdev->dev.kobj, &dev_attr_reset.attr);
	if (err) {
		dev_err(&pdev->dev, "Error during reset sysfs creation\n");
		sysfs_remove_file(&pdev->dev.kobj, &dev_attr_collect.attr);
		return err;
	}

	nb_bk = pdev->dev.of_node;
	for_each_child_of_node(pdev->dev.of_node, nb) {
		// change device name and device_node to register it properly
		pdev->dev.init_name = nb->name;
		pdev->dev.of_node = nb;
		err = register_device_to_genpd(&pdev->dev);
		if (err) {
			dev_err(&pdev->dev,
				"Error during registration of: %s %d\n",
				nb->name, err);
		}
	}

	// restore the correct device_node
	pdev->dev.of_node = nb_bk;

	return 0;
}

static int google_cpu_lpm_stats_remove(struct platform_device *pdev)
{
	struct cpu_lpm_data *lat, *tmp;

	list_for_each_entry_safe(lat, tmp, &cpu_lpm_list, list) {
		dev_pm_genpd_remove_notifier(lat->genpd_dev);
		kobject_put(&lat->pd_dir);
		kobject_put(&lat->pd_residency_data);
		kobject_put(&lat->pd_latency_data);
		kobject_put(&lat->pd_fw_entry_stats);
		kobject_put(&lat->pd_fw_exit_stats);
		kobject_put(&lat->pd_fw_sched_entry_stats);
		kobject_put(&lat->pd_fw_sched_exit_stats);
		kobject_put(&lat->pd_fw_entry_transition_stats);
		kobject_put(&lat->pd_fw_exit_transition_stats);
		kobject_put(&lat->pd_e2e_entry_stats);
		kobject_put(&lat->pd_e2e_exit_stats);
		list_del(&lat->list);
		kfree(lat);
	}

	return 0;
}

static const struct of_device_id cpu_lpm_stats_match_table[] = {
	{ .compatible = "google,cpu-lpm-stats" },
	{}
};
MODULE_DEVICE_TABLE(of, cpu_lpm_stats_match_table);

static struct platform_driver cpu_lpm_stats_driver = {
	.probe  = cpu_lpm_stats_probe,
	.driver = {
		   .name = "cpu_lpm_stats",
		   .of_match_table = cpu_lpm_stats_match_table,
		   .suppress_bind_attrs = true,
	},
	.remove = google_cpu_lpm_stats_remove,
};
module_platform_driver(cpu_lpm_stats_driver);

MODULE_AUTHOR("Mariano Marciello <mmarciello@google.com>");
MODULE_DESCRIPTION("Google cpu lpm stats driver");
MODULE_LICENSE("GPL");
