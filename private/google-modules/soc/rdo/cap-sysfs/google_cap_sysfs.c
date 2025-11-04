// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Google LLC */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <cap-sysfs/google_cap_sysfs.h>
#include "soc/google/google_gtc.h"

#define POWER_STATE_STATS_SIZE sizeof(struct cap_statsbuf_power_state_stats)
#define DVFS_STATS_SIZE sizeof(struct cap_statsbuf_dvfs_stats)

static LIST_HEAD(stats_region_list);
static LIST_HEAD(dvfs_stats_region_list);
static LIST_HEAD(aging_clc_stats_region_list);

static LIST_HEAD(aging_clc_attr_list);
static LIST_HEAD(dvfs_attr_list);

struct google_cap_sysfs {
	struct device *dev;
	struct mutex
		power_state_stats_lock; /* userspace access power state lock */
	struct mutex dvfs_lock; /* userspace access dvfs_stats lock */
	struct kobject *aging_clc_stats_dir;
};

static struct google_cap_sysfs *cap_sysfs;

static ssize_t fill_cap_stats_buf(struct google_cap_sysfs *cap_sysfs,
				  void *buf_first, void *buf_second,
				  void __iomem *addr, u32 size)
{
	int i;

	for (i = 0; i < READ_TIME; ++i) {
		memcpy_fromio(buf_first, addr, size);
		memcpy_fromio(buf_second, addr, size);
		if (!memcmp(buf_first, buf_second, size))
			break;
	}

	if (i == READ_TIME) {
		dev_warn(cap_sysfs->dev, "Read/Write Collisions\n");
		return -EAGAIN;
	}
	return 0;
}

static uint64_t get_ms_from_ticks(uint64_t ticks)
{
	return ticks / GTC_TICKS_PER_MS;
}

static ssize_t power_state_stats_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	u64 curr_time;
	u64 last_entry_ts = 0;
	u64 curr_power_state_last_entry_ts = 0;
	u64 last_entry_end_ts, last_exit_end_ts, total_ticks, state_entry_count;
	struct stats_region *region;
	struct cap_statsbuf_power_state_stats *stats;
	int curr_power_state_index = 0;
	ssize_t count = 0;
	int ret, i;

	ret = mutex_lock_interruptible(&cap_sysfs->power_state_stats_lock);
	if (ret)
		return ret;

	curr_time = goog_gtc_get_counter();
	list_for_each_entry(region, &stats_region_list, list) {
		ret = fill_cap_stats_buf(cap_sysfs, region->read_first,
				 region->read_second,
				 region->start_addr,
				 POWER_STATE_STATS_SIZE * region->number);
		if (ret)
			goto power_state_stats_show_exit;
	}

	list_for_each_entry(region, &stats_region_list, list) {
		stats = (struct cap_statsbuf_power_state_stats *)region->read_first;
		for (i = 0; i < region->number; i++) {
			last_entry_ts = stats[i].last_entry_end_ts;
			if (last_entry_ts > curr_power_state_last_entry_ts) {
				curr_power_state_index = region->ids[i];
				curr_power_state_last_entry_ts = last_entry_ts;
			}
		}

	}

	list_for_each_entry(region, &stats_region_list, list) {
		stats = (struct cap_statsbuf_power_state_stats *)region->read_first;
		for (i = 0; i < region->number; ++i) {
			last_entry_end_ts = stats[i].last_entry_end_ts;
			last_exit_end_ts = stats[i].last_exit_end_ts;
			total_ticks = stats[i].total_time_in_state_ticks;
			state_entry_count = stats[i].state_entry_count;

			count += sysfs_emit_at(buf, count, "---\n");
			count += sysfs_emit_at(buf, count, "state: %s\n",
							region->names[i]);
			count += sysfs_emit_at(buf, count,
					       "last_entry_ts_ticks: %llu\n", last_entry_end_ts);
			count += sysfs_emit_at(buf, count,
						"last_exit_ts_ticks: %llu\n", last_exit_end_ts);

			/* Add elapsed time for current power state. */
			if (curr_time > 0) {
				if (region->ids[i] == curr_power_state_index &&
						curr_power_state_last_entry_ts > 0)
					total_ticks += (curr_time - curr_power_state_last_entry_ts);
			}

			count += sysfs_emit_at(buf, count,
						"total_state_residency_ms: %llu\n",
						get_ms_from_ticks(total_ticks));
			count += sysfs_emit_at(buf, count,
						"last_state_entry_counts: %llu\n",
						state_entry_count);
		}

	}

power_state_stats_show_exit:
	mutex_unlock(&cap_sysfs->power_state_stats_lock);
	return ret == 0 ? count : ret;
}

static ssize_t power_state_latency_stats_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	struct stats_region *region;
	struct cap_statsbuf_power_state_stats *stats;
	int ret, i;
	ssize_t count = 0;

	ret = mutex_lock_interruptible(&cap_sysfs->power_state_stats_lock);
	if (ret)
		return ret;

	list_for_each_entry(region, &stats_region_list, list) {
		ret = fill_cap_stats_buf(cap_sysfs, region->read_first,
				 region->read_second,
				 region->start_addr,
				 POWER_STATE_STATS_SIZE * region->number);
		if (ret)
			goto power_state_latency_stats_show_exit;
	}

	list_for_each_entry(region, &stats_region_list, list) {
		stats = (struct cap_statsbuf_power_state_stats *)region->read_first;
		for (i = 0; i < region->number; ++i) {
			count += sysfs_emit_at(buf, count, "---\n");
			count += sysfs_emit_at(buf, count, "state: %s\n", region->names[i]);
			count += sysfs_emit_at(buf, count, "last_entry_start_ts_ticks: %llu\n",
						stats[i].last_entry_start_ts);
			count += sysfs_emit_at(buf, count, "last_entry_end_ts_ticks: %llu\n",
						stats[i].last_entry_end_ts);
			count += sysfs_emit_at(buf, count, "last_exit_start_ts_ticks: %llu\n",
						stats[i].last_exit_start_ts);
			count += sysfs_emit_at(buf, count, "last_exit_end_ts_ticks: %llu\n",
						stats[i].last_exit_end_ts);
		}
	}

power_state_latency_stats_show_exit:
	mutex_unlock(&cap_sysfs->power_state_stats_lock);
	return ret == 0 ? count : ret;
}

DEVICE_ATTR_RO(power_state_stats);
DEVICE_ATTR_RO(power_state_latency_stats);

static ssize_t dvfs_stats_clock_domain_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	u64 curr_time, total_ticks, last_entry_ts;
	struct cap_statsbuf_dvfs_stats *dvfs_data;
	void __iomem *dvfs_stats_addr;
	void *buf_first, *buf_second;
	ssize_t cnt = 0;
	int ret, i;
	struct dvfs_attribute *dvfs_attr = NULL;

	list_for_each_entry(dvfs_attr, &dvfs_attr_list, list) {
		if (dvfs_attr->dev_attr == attr)
			break;
	}

	if (!dvfs_attr) {
		dev_err(cap_sysfs->dev, "Failed to find dvfs attribute\n");
		return -EINVAL;
	}

	struct stats_region *region = dvfs_attr->dvfs_stats_region;
	int index = dvfs_attr->id;

	ret = mutex_lock_interruptible(&cap_sysfs->dvfs_lock);
	if (ret) {
		dev_warn(cap_sysfs->dev, "Failed to acquire mutex, ret %d",
			 ret);
		return ret;
	}

	curr_time = goog_gtc_get_counter();

	buf_first = &region->read_first[index];
	buf_second = &region->read_second[index];
	dvfs_stats_addr = region->start_addr +
			  index * sizeof(struct cap_statsbuf_dvfs_stats);

	ret = fill_cap_stats_buf(cap_sysfs, buf_first, buf_second,
				 dvfs_stats_addr, sizeof(struct cap_statsbuf_dvfs_stats));
	if (ret)
		goto dvfs_stats_exit;
	dvfs_data = (struct cap_statsbuf_dvfs_stats *)(&region->read_first[index]);
	cnt += sysfs_emit_at(buf, cnt, "---\n");
	cnt += sysfs_emit_at(buf, cnt, "clock_domain: %s\n",
			     region->names[index]);

	if (dvfs_data->domain_current_level >= MAX_DVFS_LEVELS)
		cnt += sysfs_emit_at(buf, cnt, "domain_current_level: %s\n",
			     "DOMAIN SUSPENDED");
	else
		cnt += sysfs_emit_at(buf, cnt, "domain_current_level: %u\n",
			     dvfs_data->domain_current_level);

	cnt += sysfs_emit_at(buf, cnt, "domain_target_level: %u\n",
			     dvfs_data->domain_target_level);
	cnt += sysfs_emit_at(buf, cnt, "per_op_level_stats: \n");
	for (i = 0; i < MAX_DVFS_LEVELS; ++i) {
		cnt += sysfs_emit_at(buf, cnt, "  - oplevel: %d\n", i);
		cnt += sysfs_emit_at(buf, cnt, "    entry_count: %llu\n",
				     dvfs_data->domain_level_stats[i]
					     .entry_count);
		total_ticks =
			dvfs_data->domain_level_stats[i].total_residency_ticks;
		last_entry_ts = dvfs_data->domain_level_stats[i].last_entry_ts;

		/* Add elapsed time for current DVFS level. */
		if (curr_time > 0) {
			if (i == dvfs_data->domain_current_level && last_entry_ts > 0)
				total_ticks += (curr_time - last_entry_ts);
		}

		cnt += sysfs_emit_at(buf, cnt,
			"    total_residency_ms: %llu\n",
			get_ms_from_ticks(total_ticks));
		cnt += sysfs_emit_at(buf, cnt,
			"    last_entry_timestamp_ticks: %llu\n",
			last_entry_ts);
	}
dvfs_stats_exit:
	mutex_unlock(&cap_sysfs->dvfs_lock);
	return ret == 0 ? cnt : ret;
}

static int get_dvfs_stats_attr_name(struct device *dev, struct device_attribute *attr,
				const char *rail_verbose)
{
	int attr_name_char_count = strlen("dvfs_stats_") + strlen(rail_verbose);
	char *buff = devm_kcalloc(dev, attr_name_char_count, sizeof(char), GFP_KERNEL);

	if (!buff)
		return -ENOMEM;
	sprintf(buff, "dvfs_stats_%s", rail_verbose);
	attr->attr.name = buff;
	return 0;
}

static int dvfs_sysfs_node_create(struct device *dev)
{
	int ret, i;
	struct stats_region *region;
	struct dvfs_attribute *dvfs_attr_list_elem;

	list_for_each_entry(region, &dvfs_stats_region_list, list) {
		for (i = 0; i < region->number; i++) {
			struct device_attribute *dev_attr =
				devm_kzalloc(dev, sizeof(struct device_attribute), GFP_KERNEL);

			if (!dev_attr) {
				ret = -ENOMEM;
				goto dvfs_sysfs_create_error;
			}

			sysfs_attr_init(&dev_attr->attr);
			ret = get_dvfs_stats_attr_name(dev, dev_attr, region->names[i]);
			if (ret)
				goto dvfs_sysfs_create_error;

			dev_attr->attr.mode = 0444;
			dev_attr->show = dvfs_stats_clock_domain_show;

			struct dvfs_attribute *dvfs_attr =
				devm_kzalloc(dev, sizeof(struct dvfs_attribute), GFP_KERNEL);

			if (!dvfs_attr) {
				ret = -ENOMEM;
				goto dvfs_sysfs_create_error;
			}
			dvfs_attr->dvfs_stats_region = region;
			dvfs_attr->dev_attr = dev_attr;
			dvfs_attr->id = i;

			ret = device_create_file(dev, dev_attr);
			if (ret) {
				dev_err(dev, "Failed to create %s sysfs file\n",
					dev_attr->attr.name);
				goto dvfs_sysfs_create_error;
			}
			list_add(&dvfs_attr->list, &dvfs_attr_list);
		}
	}

	return 0;

dvfs_sysfs_create_error:
	list_for_each_entry(dvfs_attr_list_elem, &dvfs_attr_list, list) {
		device_remove_file(dev, dvfs_attr_list_elem->dev_attr);
	}
	return ret;
}

#define AGING_CLC_STATS_ATTR(_name, _mode)                  \
	struct attribute aging_clc_stats_##_name = {        \
		.name = __stringify(_name),              \
		.mode = VERIFY_OCTAL_PERMISSIONS(_mode), \
	}

static AGING_CLC_STATS_ATTR(last_measurement_ts, 0444);
static AGING_CLC_STATS_ATTR(last_aging_sensor_measurement, 0444);
static AGING_CLC_STATS_ATTR(last_adjustment_pmic_step, 0444);
static AGING_CLC_STATS_ATTR(error_count, 0444);
static AGING_CLC_STATS_ATTR(last_status_code, 0444);

static struct attribute *aging_clc_stats_attrs[] = {
	&aging_clc_stats_last_measurement_ts,
	&aging_clc_stats_last_aging_sensor_measurement,
	&aging_clc_stats_last_adjustment_pmic_step,
	&aging_clc_stats_error_count,
	&aging_clc_stats_last_status_code,
	NULL
};

ATTRIBUTE_GROUPS(aging_clc_stats);

static ssize_t show_aging_clc_stats(struct kobject *kobj, struct attribute *attr,
				    char *buf)
{
	int ret, index;
	ssize_t cnt = 0;
	struct stats_region *region;
	struct aging_clc_attribute *aging_clc_attr;
	struct cap_statsbuf_clc_aging_stats *aging_clc_data;
	void __iomem *aging_clc_stats_addr;
	void *buf_first, *buf_second;

	list_for_each_entry(aging_clc_attr, &aging_clc_attr_list, list) {
		if (aging_clc_attr->kobj == kobj)
			break;
	}

	region = aging_clc_attr->aging_clc_region;
	index = aging_clc_attr->id;
	buf_first = &region->read_first[index];
	buf_second = &region->read_second[index];
	aging_clc_stats_addr = region->start_addr +
			  index * sizeof(struct cap_statsbuf_clc_aging_stats);

	ret = fill_cap_stats_buf(cap_sysfs, buf_first, buf_second,
				 aging_clc_stats_addr, sizeof(struct cap_statsbuf_clc_aging_stats));
	if (ret)
		goto aging_clc_stats_exit;
	aging_clc_data = (struct cap_statsbuf_clc_aging_stats *)(&region->read_first[index]);

	if (attr == &aging_clc_stats_last_measurement_ts)
		cnt += sysfs_emit_at(buf, cnt, "%u\n", aging_clc_data->last_measurement_ts);
	else if (attr == &aging_clc_stats_last_aging_sensor_measurement)
		cnt += sysfs_emit_at(buf, cnt, "%u\n",
					aging_clc_data->last_aging_sensor_measurement);
	else if (attr == &aging_clc_stats_last_adjustment_pmic_step)
		cnt += sysfs_emit_at(buf, cnt, "%hhu\n", aging_clc_data->last_adjustment_pmic_step);
	else if (attr == &aging_clc_stats_error_count)
		cnt += sysfs_emit_at(buf, cnt, "%hhu\n", aging_clc_data->error_count);
	else if (attr == &aging_clc_stats_last_status_code)
		cnt += sysfs_emit_at(buf, cnt, "%hhu\n", aging_clc_data->last_status_code);

aging_clc_stats_exit:
	return ret == 0 ? cnt : ret;
}

static const struct sysfs_ops aging_clc_stats_ops = {
	.show = show_aging_clc_stats,
};

static const struct kobj_type aging_clc_kobj_type = {
	.sysfs_ops = &aging_clc_stats_ops,
	.default_groups = aging_clc_stats_groups,
};

static const struct kobj_type aging_clc_stats_dir_kobj_type = {};

static int aging_clc_sysfs_node_create(struct device *dev)
{
	struct aging_clc_attribute *aging_clc_attr_list_elem;
	struct stats_region *region;
	int err, i;

	cap_sysfs->aging_clc_stats_dir = devm_kzalloc(dev, sizeof(struct kobject), GFP_KERNEL);
	if (!cap_sysfs->aging_clc_stats_dir)
		return -ENOMEM;

	err = kobject_init_and_add(cap_sysfs->aging_clc_stats_dir, &aging_clc_stats_dir_kobj_type,
					&dev->kobj, "aging_clc_stats");
	if (err) {
		dev_err(dev, "Error %d when creating aging_clc_stats dir\n", err);
		return err;
	}

	list_for_each_entry(region, &aging_clc_stats_region_list, list) {
		for (i = 0; i < region->number; i++) {
			struct aging_clc_attribute *aging_clc_attr =
				devm_kzalloc(dev, sizeof(struct aging_clc_attribute), GFP_KERNEL);

			if (!aging_clc_attr) {
				err = -ENOMEM;
				goto aging_clc_sysfs_create_error;
			}

			aging_clc_attr->kobj =
				devm_kzalloc(dev, sizeof(struct kobject), GFP_KERNEL);
			if (!aging_clc_attr->kobj) {
				err = -ENOMEM;
				goto aging_clc_sysfs_create_error;
			}

			err = kobject_init_and_add(aging_clc_attr->kobj,
						&aging_clc_kobj_type,
						cap_sysfs->aging_clc_stats_dir,
						"%s", region->names[i]);
			if (err) {
				dev_err(dev, "Failed to create aging clc stats for rail %s\n",
					region->names[i]);
				goto aging_clc_sysfs_create_error;
			}

			aging_clc_attr->aging_clc_region = region;
			aging_clc_attr->id = region->ids[i];
			list_add(&aging_clc_attr->list, &aging_clc_attr_list);
		}
	}

	return 0;

aging_clc_sysfs_create_error:
	list_for_each_entry(aging_clc_attr_list_elem, &aging_clc_attr_list, list) {
		kobject_put(aging_clc_attr_list_elem->kobj);
	}
	kobject_put(cap_sysfs->aging_clc_stats_dir);
	return err;
}

static int google_cap_sysfs_remove(struct platform_device *pdev)
{
	struct dvfs_attribute *dvfs_attr_list_elem, *tmp;
	struct aging_clc_attribute *aging_clc_attr_list_elem, *tmp3;
	struct stats_region *region, *tmp2;

	list_for_each_entry(dvfs_attr_list_elem, &dvfs_attr_list, list) {
		device_remove_file(&pdev->dev, dvfs_attr_list_elem->dev_attr);
	}

	list_for_each_entry(aging_clc_attr_list_elem, &aging_clc_attr_list, list) {
		kobject_put(aging_clc_attr_list_elem->kobj);
	}
	kobject_put(cap_sysfs->aging_clc_stats_dir);

	device_remove_file(&pdev->dev, &dev_attr_power_state_stats);
	device_remove_file(&pdev->dev, &dev_attr_power_state_latency_stats);

	list_for_each_entry_safe(dvfs_attr_list_elem, tmp, &dvfs_attr_list, list) {
		list_del(&dvfs_attr_list_elem->list);
		devm_kfree(&pdev->dev, dvfs_attr_list_elem);
	}

	list_for_each_entry_safe(aging_clc_attr_list_elem, tmp3, &aging_clc_attr_list, list) {
		list_del(&aging_clc_attr_list_elem->list);
		devm_kfree(&pdev->dev, aging_clc_attr_list_elem);
	}

	list_for_each_entry_safe(region, tmp2, &stats_region_list, list) {
		list_del(&region->list);
		devm_kfree(&pdev->dev, region);
	}

	list_for_each_entry_safe(region, tmp2, &dvfs_stats_region_list, list) {
		list_del(&region->list);
		devm_kfree(&pdev->dev, region);
	}

	list_for_each_entry_safe(region, tmp2, &aging_clc_stats_region_list, list) {
		list_del(&region->list);
		devm_kfree(&pdev->dev, region);
	}

	return 0;
}

static int alloc_stats_buffers(struct device *dev, void **buf1, void **buf2,
				u32 count, ssize_t stats_struct_size)
{
	*buf1 = devm_kcalloc(dev, count, stats_struct_size, GFP_KERNEL);
	if (!*buf1)
		return -ENOMEM;

	*buf2 = devm_kcalloc(dev, count, stats_struct_size, GFP_KERNEL);
	if (!*buf2)
		return -ENOMEM;
	return 0;
}

static int parse_stats_regions_prop(struct device *dev, struct device_node *stats_regions,
				struct list_head *list,
				ssize_t stats_struct_size,
				const char *entities_key,
				const char *entities_verbose_key)
{
	int ret;
	u64 raw_reg_addr, reg_size;
	struct device_node *child_stats_region;

	for_each_child_of_node(stats_regions, child_stats_region) {
		struct stats_region *region =
			devm_kzalloc(dev, sizeof(struct stats_region), GFP_KERNEL);

		if (!region)
			return -ENOMEM;

		ret = of_property_read_reg(child_stats_region, 0, &raw_reg_addr, &reg_size);
		if (ret < 0) {
			dev_err(dev, "stats region could not be read\n");
			return ret;
		}

		region->start_addr = devm_ioremap(dev, raw_reg_addr, reg_size);
		if (!region->start_addr) {
			dev_err(dev, "Failed to iomap %s stats region property\n",
				child_stats_region->name);
			return -ENOMEM;
		}

		ret = of_property_count_u32_elems(child_stats_region, entities_key);
		if (ret < 0) {
			dev_err(dev, "Failed to read %s prop length of region %s\n",
				entities_key,
				child_stats_region->name);
			return ret;
		}

		region->number = ret;
		region->ids = devm_kcalloc(dev, ret, sizeof(u32), GFP_KERNEL);
		if (!region->ids)
			return -ENOMEM;

		ret = of_property_read_u32_array(
				child_stats_region,
				entities_key,
				region->ids,
				region->number);
		if (ret) {
			dev_err(dev, "Failed to read %s prop of region %s\n",
				entities_key,
				child_stats_region->name);
			return ret;
		}

		region->names = devm_kcalloc(dev,
					region->number, sizeof(const char *),
					GFP_KERNEL);
		if (!region->names)
			return -ENOMEM;

		ret = of_property_read_string_array(
				child_stats_region,
				entities_verbose_key,
				region->names,
				region->number);
		if (ret < 0) {
			dev_err(dev, "Failed to read %s prop of region %s\n",
				entities_verbose_key,
				child_stats_region->name);
			return ret;
		}

		if (ret != region->number) {
			dev_err(dev,
				"Mismatch %s and %s props length for region %s\n",
				entities_key, entities_verbose_key,
				child_stats_region->name);
			return -EINVAL;
		}

		ret = alloc_stats_buffers(dev, &region->read_first, &region->read_second,
				region->number, stats_struct_size);
		if (ret)
			return ret;

		list_add(&region->list, list);
	}

	return 0;
}

static int google_cap_sysfs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *power_stats_regions;
	struct device_node *dvfs_stats_regions;
	struct device_node *aging_clc_stats_regions;
	int ret;

	cap_sysfs = devm_kzalloc(dev, sizeof(*cap_sysfs), GFP_KERNEL);
	if (!cap_sysfs)
		return -ENOMEM;
	cap_sysfs->dev = dev;
	platform_set_drvdata(pdev, cap_sysfs);
	power_stats_regions = of_get_child_by_name(pdev->dev.of_node, "idle_pm_stats_regions");

	if (!power_stats_regions) {
		ret = -EINVAL;
		dev_err(dev, "power_stats_regions node not found\n");
		goto probe_exit;
	}

	ret = parse_stats_regions_prop(cap_sysfs->dev, power_stats_regions,
				&stats_region_list, sizeof(struct cap_statsbuf_power_state_stats),
				"states", "states-verbose");
	if (ret)
		goto probe_exit;

	dvfs_stats_regions = of_get_child_by_name(pdev->dev.of_node, "dvfs_stats_regions");
	if (!dvfs_stats_regions) {
		ret = -EINVAL;
		dev_err(dev, "dvfs_stats_regions node not found\n");
		goto probe_exit;
	}

	ret = parse_stats_regions_prop(cap_sysfs->dev, dvfs_stats_regions,
				&dvfs_stats_region_list, sizeof(struct cap_statsbuf_dvfs_stats),
				"rails", "rails-verbose");
	if (ret)
		goto probe_exit;

	aging_clc_stats_regions =
		of_get_child_by_name(pdev->dev.of_node, "aging_clc_stats_regions");
	if (!aging_clc_stats_regions) {
		ret = -EINVAL;
		dev_err(dev, "aging_clc_stats_regions node not found\n");
		goto probe_exit;
	}

	ret = parse_stats_regions_prop(cap_sysfs->dev, aging_clc_stats_regions,
				&aging_clc_stats_region_list,
				sizeof(struct cap_statsbuf_clc_aging_stats),
				"rails", "rails-verbose");
	if (ret)
		goto probe_exit;

	mutex_init(&cap_sysfs->power_state_stats_lock);
	mutex_init(&cap_sysfs->dvfs_lock);
	ret = device_create_file(dev, &dev_attr_power_state_stats);
	if (ret) {
		dev_err(dev, "Failed to create power_state sysfs file\n");
		goto probe_exit;
	}

	ret = device_create_file(dev, &dev_attr_power_state_latency_stats);
	if (ret) {
		dev_err(dev, "Failed to create power_state_latency_stats sysfs file\n");
		goto probe_exit;
	}

	ret = dvfs_sysfs_node_create(dev);
	if (ret)
		goto probe_exit;

	ret = aging_clc_sysfs_node_create(dev);
	if (ret)
		goto probe_exit;
probe_exit:
	return ret;
}

static const struct of_device_id google_cap_sysfs_of_match_table[] = {
	{ .compatible = "google,cap-sysfs" },
	{},
};
MODULE_DEVICE_TABLE(of, google_cap_sysfs_of_match_table);

struct platform_driver google_cap_sysfs_driver = {
	.driver = {
		.name = "google-cap-sysfs",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(google_cap_sysfs_of_match_table),
	},
	.probe  = google_cap_sysfs_probe,
	.remove = google_cap_sysfs_remove,
};

module_platform_driver(google_cap_sysfs_driver);

int read_cpu_pd_latency_stats(u32 power_state,
		struct timestamps_buffer *ts_buff)
{
	struct stats_region *region;
	int i;
	void __iomem *state_stats_addr = NULL;

	list_for_each_entry(region, &stats_region_list, list) {
		for (i = 0; i < region->number; ++i) {
			if (region->ids[i] == power_state) {
				state_stats_addr = region->start_addr + i * POWER_STATE_STATS_SIZE;
				break;
			}
		}
	}

	if (!state_stats_addr)
		return -EINVAL;

	memcpy_fromio(ts_buff, state_stats_addr, sizeof(struct timestamps_buffer));
	return 0;
}
EXPORT_SYMBOL(read_cpu_pd_latency_stats);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google cap-sysfs driver");
MODULE_LICENSE("GPL");
