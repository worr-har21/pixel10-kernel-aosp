// SPDX-License-Identifier: GPL-2.0-only
#include <linux/debugfs.h>
#include <linux/devfreq.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/units.h>
#include <perf/core/google_vote_manager.h>
#include <perf/core/google_pm_qos.h>
#include <trace/events/power.h>

#define to_devfreq(device)\
container_of((device), struct devfreq, dev)

enum vote_idx {
	SOFT_MIN_FREQ_IDX,
	SOFT_MAX_FREQ_IDX,
	DEBUG_MIN_FREQ_IDX,
	DEBUG_MAX_FREQ_IDX,
	THERMAL_MAX_FREQ_IDX,
	POWERHINT_MIN_FREQ_IDX,
	POWERHINT_MAX_FREQ_IDX,
	NUM_VOTE_REQ
};

enum domain_type {
	CPUFREQ_VOTE,
	DEVFREQ_VOTE,
};

enum vote_type {
	VOTE_MIN_FREQ,
	VOTE_MAX_FREQ,
};

struct vote_meta_info {
	const char *name;
	const enum vote_type type;
};

static const struct vote_meta_info vote_meta_infos[NUM_VOTE_REQ] = {
	[SOFT_MIN_FREQ_IDX] = {"soft_min_freq", VOTE_MIN_FREQ},
	[SOFT_MAX_FREQ_IDX] = {"soft_max_freq", VOTE_MAX_FREQ},
	[DEBUG_MIN_FREQ_IDX] = {"debug_min_freq", VOTE_MIN_FREQ},
	[DEBUG_MAX_FREQ_IDX] = {"debug_max_freq", VOTE_MAX_FREQ},
	[THERMAL_MAX_FREQ_IDX] = {"thermal_max_freq", VOTE_MAX_FREQ},
	[POWERHINT_MIN_FREQ_IDX] = {"powerhint_min_freq", VOTE_MIN_FREQ},
	[POWERHINT_MAX_FREQ_IDX] = {"powerhint_max_freq", VOTE_MAX_FREQ},
};

struct vote_data {
	enum domain_type type;
	union {
		struct devfreq *devfreq;
		struct cpufreq_policy *cpufreq_policy;
	} domain;
	union {
		struct freq_qos_request cpu_reqs[NUM_VOTE_REQ];
		struct dev_pm_qos_request dev_reqs[NUM_VOTE_REQ];
	} reqs;
	struct list_head list;
};

static LIST_HEAD(google_vote_data_list);

static DEFINE_MUTEX(vote_manager_lock);

static struct vote_data *get_devfreq_vote_data(struct devfreq *devfreq)
{
	struct vote_data *data;

	list_for_each_entry(data, &google_vote_data_list, list) {
		if (data->type == DEVFREQ_VOTE && data->domain.devfreq == devfreq)
			return data;
	}
	return NULL;
}

static struct vote_data *get_cpufreq_vote_data(struct cpufreq_policy *policy)
{
	struct vote_data *data;

	list_for_each_entry(data, &google_vote_data_list, list) {
		if (data->type == CPUFREQ_VOTE && data->domain.cpufreq_policy == policy)
			return data;
	}
	return NULL;
}

static void add_request(struct vote_data *data, enum vote_idx idx,
		enum vote_type type, u32 initial_freq)
{
	switch (data->type) {
	case CPUFREQ_VOTE:
		google_pm_qos_add_cpufreq_request(data->domain.cpufreq_policy,
			&data->reqs.cpu_reqs[idx],
			type == VOTE_MIN_FREQ ? FREQ_QOS_MIN : FREQ_QOS_MAX, initial_freq);
		break;
	case DEVFREQ_VOTE:
		google_pm_qos_add_devfreq_request(data->domain.devfreq,
			&data->reqs.dev_reqs[idx],
			type == VOTE_MIN_FREQ ? DEV_PM_QOS_MIN_FREQUENCY :
			DEV_PM_QOS_MAX_FREQUENCY, initial_freq);
		break;
	default:
		WARN(1, "Unknown vote type in %s\n", __func__);
		break;
	}
}

static void add_votes_from_dt(struct vote_data *data)
{
	struct device_node *vote_mgr_node;
	struct device *dev;
	unsigned int val;
	int ret = 0;

	switch (data->type) {
	case CPUFREQ_VOTE:
		dev = get_cpu_device(data->domain.cpufreq_policy->cpu);
		break;
	case DEVFREQ_VOTE:
		dev = data->domain.devfreq->dev.parent;
		break;
	default:
		WARN(1, "Unknown vote type in %s\n", __func__);
		break;
	}

	vote_mgr_node = of_get_child_by_name(dev->of_node, "vote_manager");
	if (!vote_mgr_node)
		dev_dbg(dev, "vote_manager node not defined.\n");

	for (int i = 0; i < NUM_VOTE_REQ; i++) {
		ret = of_property_read_u32(vote_mgr_node, vote_meta_infos[i].name, &val);
		if (ret < 0) {
			val = vote_meta_infos[i].type == VOTE_MIN_FREQ ? 0 : S32_MAX;
		} else if (data->type == DEVFREQ_VOTE) {
			/*
			 * Only round for devfreq values from dtsi; otherwise, max_freq might
			 * be capped at S32_MAX
			 */
			val = DIV_ROUND_UP(val, HZ_PER_KHZ);
		}
		add_request(data, i, vote_meta_infos[i].type, val);
	}

}

static void remove_vote_data(struct device *dev, struct vote_data *data)
{
	for (int i = 0; i < NUM_VOTE_REQ; i++) {
		switch (data->type) {
		case CPUFREQ_VOTE:
			google_pm_qos_remove_cpufreq_request(data->domain.cpufreq_policy,
				&data->reqs.cpu_reqs[i]);
			break;
		case DEVFREQ_VOTE:
			google_pm_qos_remove_devfreq_request(data->domain.devfreq,
				&data->reqs.dev_reqs[i]);
			break;
		default:
			WARN(1, "Unknown vote type in %s\n", __func__);
			break;
		}
	}
	list_del(&data->list);
	devm_kfree(dev, data);
}

#define show_cpu_vote(file_name, index)							\
static ssize_t show_##file_name								\
(struct cpufreq_policy *policy, char *buf)						\
{											\
	struct vote_data *data;								\
	ssize_t ret;									\
											\
	mutex_lock(&vote_manager_lock);							\
	data = get_cpufreq_vote_data(policy);						\
	if (WARN(!freq_qos_request_active(&data->reqs.cpu_reqs[index]),			\
		"%s() called for unknown object\n", __func__)) {			\
		mutex_unlock(&vote_manager_lock);					\
		return -EINVAL;								\
	}										\
	ret  = sysfs_emit(buf, "%d\n",							\
	data->reqs.cpu_reqs[index].pnode.prio);						\
	mutex_unlock(&vote_manager_lock);						\
	return ret;									\
}

#define store_cpu_vote(file_name, index)						\
static ssize_t store_##file_name							\
(struct cpufreq_policy *policy, const char *buf, size_t count)				\
{											\
	unsigned long val;								\
	int ret;									\
	struct vote_data *data;								\
											\
	ret = kstrtoul(buf, 0, &val);							\
	if (ret)									\
		return ret;								\
											\
	mutex_lock(&vote_manager_lock);							\
	data = get_cpufreq_vote_data(policy);						\
											\
	ret = freq_qos_update_request(&data->reqs.cpu_reqs[index], val);		\
	mutex_unlock(&vote_manager_lock);						\
	return ret >= 0 ? count : ret;							\
}

show_cpu_vote(soft_min_freq, SOFT_MIN_FREQ_IDX);
show_cpu_vote(soft_max_freq, SOFT_MAX_FREQ_IDX);
show_cpu_vote(debug_min_freq, DEBUG_MIN_FREQ_IDX);
show_cpu_vote(debug_max_freq, DEBUG_MAX_FREQ_IDX);
show_cpu_vote(thermal_max_freq, THERMAL_MAX_FREQ_IDX);
show_cpu_vote(powerhint_min_freq, POWERHINT_MIN_FREQ_IDX);
show_cpu_vote(powerhint_max_freq, POWERHINT_MAX_FREQ_IDX);

store_cpu_vote(soft_min_freq, SOFT_MIN_FREQ_IDX);
store_cpu_vote(soft_max_freq, SOFT_MAX_FREQ_IDX);
store_cpu_vote(debug_min_freq, DEBUG_MIN_FREQ_IDX);
store_cpu_vote(debug_max_freq, DEBUG_MAX_FREQ_IDX);
store_cpu_vote(thermal_max_freq, THERMAL_MAX_FREQ_IDX);
store_cpu_vote(powerhint_min_freq, POWERHINT_MIN_FREQ_IDX);
store_cpu_vote(powerhint_max_freq, POWERHINT_MAX_FREQ_IDX);

cpufreq_freq_attr_rw(soft_min_freq);
cpufreq_freq_attr_rw(soft_max_freq);
cpufreq_freq_attr_rw(debug_min_freq);
cpufreq_freq_attr_rw(debug_max_freq);
cpufreq_freq_attr_rw(thermal_max_freq);
cpufreq_freq_attr_rw(powerhint_min_freq);
cpufreq_freq_attr_rw(powerhint_max_freq);

static struct attribute *cpufreq_sysfs_entries[] = {
	&soft_min_freq.attr,
	&soft_max_freq.attr,
	&debug_min_freq.attr,
	&debug_max_freq.attr,
	&thermal_max_freq.attr,
	&powerhint_min_freq.attr,
	&powerhint_max_freq.attr,
	NULL,
};

static struct attribute_group cpufreq_attr_group = {
	.name = "vote_manager",
	.attrs = cpufreq_sysfs_entries,
};

#define show_dev_vote(file_name, index)							\
static ssize_t file_name ## _show							\
(struct device *dev, struct device_attribute *attr, char *buf)				\
{											\
	struct vote_data *data;								\
	ssize_t ret;									\
	struct devfreq *df = to_devfreq(dev);						\
											\
	mutex_lock(&vote_manager_lock);							\
	data = get_devfreq_vote_data(df);						\
	if (WARN(!dev_pm_qos_request_active(&data->reqs.dev_reqs[index]),		\
		"%s() called for unknown object\n", __func__)) {			\
		mutex_unlock(&vote_manager_lock);					\
		return -EINVAL;								\
	}										\
	ret = sysfs_emit(buf, "%lu\n",							\
			HZ_PER_KHZ * data->reqs.dev_reqs[index].data.freq.pnode.prio);	\
	mutex_unlock(&vote_manager_lock);						\
	return ret;									\
}

#define store_dev_vote(file_name, index)						\
static ssize_t file_name ## _store							\
(struct device *dev, struct device_attribute *attr,					\
		const char *buf, size_t count)						\
{											\
	unsigned long val;								\
	int ret;									\
	struct vote_data *data;								\
	struct devfreq *df = to_devfreq(dev);						\
											\
	ret = kstrtoul(buf, 0, &val);							\
	if (ret)									\
		return ret;								\
											\
	mutex_lock(&vote_manager_lock);							\
	data = get_devfreq_vote_data(df);						\
											\
	ret = dev_pm_qos_update_request(&data->reqs.dev_reqs[index],			\
				DIV_ROUND_UP(val, HZ_PER_KHZ));				\
	mutex_unlock(&vote_manager_lock);						\
	return ret >= 0 ? count : ret;							\
}

show_dev_vote(soft_min_freq, SOFT_MIN_FREQ_IDX);
show_dev_vote(soft_max_freq, SOFT_MAX_FREQ_IDX);
show_dev_vote(debug_min_freq, DEBUG_MIN_FREQ_IDX);
show_dev_vote(debug_max_freq, DEBUG_MAX_FREQ_IDX);
show_dev_vote(thermal_max_freq, THERMAL_MAX_FREQ_IDX);
show_dev_vote(powerhint_min_freq, POWERHINT_MIN_FREQ_IDX);
show_dev_vote(powerhint_max_freq, POWERHINT_MAX_FREQ_IDX);

store_dev_vote(soft_min_freq, SOFT_MIN_FREQ_IDX);
store_dev_vote(soft_max_freq, SOFT_MAX_FREQ_IDX);
store_dev_vote(debug_min_freq, DEBUG_MIN_FREQ_IDX);
store_dev_vote(debug_max_freq, DEBUG_MAX_FREQ_IDX);
store_dev_vote(thermal_max_freq, THERMAL_MAX_FREQ_IDX);
store_dev_vote(powerhint_min_freq, POWERHINT_MIN_FREQ_IDX);
store_dev_vote(powerhint_max_freq, POWERHINT_MAX_FREQ_IDX);

static DEVICE_ATTR_RW(soft_min_freq);
static DEVICE_ATTR_RW(soft_max_freq);
static DEVICE_ATTR_RW(debug_min_freq);
static DEVICE_ATTR_RW(debug_max_freq);
static DEVICE_ATTR_RW(thermal_max_freq);
static DEVICE_ATTR_RW(powerhint_min_freq);
static DEVICE_ATTR_RW(powerhint_max_freq);

static struct attribute *devfreq_sysfs_entries[] = {
	&dev_attr_soft_min_freq.attr,
	&dev_attr_soft_max_freq.attr,
	&dev_attr_debug_min_freq.attr,
	&dev_attr_debug_max_freq.attr,
	&dev_attr_thermal_max_freq.attr,
	&dev_attr_powerhint_min_freq.attr,
	&dev_attr_powerhint_max_freq.attr,
	NULL,
};

static struct attribute_group devfreq_attr_group = {
	.name = "vote_manager",
	.attrs = devfreq_sysfs_entries,
};

/**
 * vote_manager_init_cpufreq - expose sysfs for userland cpu frequency voting
 * @policy: cpufreq policy
 *
 * This function adds QoS frequency requests derived from dtsi setting and enables the
 * associated sysfs nodes for userland cpu frequency voting
 *
 * Return 0 if the sysfs nodes were successfully created, or a negative error code
 * on failures.
 */
int vote_manager_init_cpufreq(struct cpufreq_policy *policy)
{
	struct vote_data *data;
	int ret;
	struct device *dev = get_cpu_device(policy->cpu);

	if (!policy)
		return -EINVAL;

	mutex_lock(&vote_manager_lock);
	data = get_cpufreq_vote_data(policy);
	if (data) {
		dev_err(dev, "%s: cpu device already exists!\n",__func__);
		ret = -EINVAL;
		goto out;
	}

	data = devm_kzalloc(dev, sizeof(struct vote_data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out;
	}

	data->type = CPUFREQ_VOTE;
	data->domain.cpufreq_policy = policy;

	list_add_tail(&data->list, &google_vote_data_list);

	add_votes_from_dt(data);
	ret = sysfs_create_group(get_governor_parent_kobj(policy), &cpufreq_attr_group);
	if (ret) {
		dev_warn(dev, "failed create sysfs for cpufreq data\n");
		remove_vote_data(dev, data);
	}

out:
	mutex_unlock(&vote_manager_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vote_manager_init_cpufreq);

/**
 * vote_manager_init_devfreq - expose sysfs for userland device frequency voting
 * @devfreq: The devfreq object
 *
 * This function adds QoS frequency requests derived from dtsi setting and enables the
 * associated sysfs nodes for userland device frequency voting
 *
 * Return 0 if the sysfs nodes were successfully created, or a negative error code
 * on failures.
 */
int vote_manager_init_devfreq(struct devfreq *devfreq)
{
	struct vote_data *data;
	int ret;
	struct device *dev = devfreq->dev.parent;

	if (!devfreq)
		return -EINVAL;

	mutex_lock(&vote_manager_lock);
	data = get_devfreq_vote_data(devfreq);
	if (data) {
		dev_err(dev, "%s: devfreq device already exists!\n",__func__);
		ret = -EINVAL;
		goto out;
	}

	data = kzalloc(sizeof(struct vote_data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out;
	}

	data->type = DEVFREQ_VOTE;
	data->domain.devfreq = devfreq;

	list_add_tail(&data->list, &google_vote_data_list);

	add_votes_from_dt(data);
	ret = sysfs_create_group(&devfreq->dev.kobj, &devfreq_attr_group);
	if (ret) {
		dev_warn(&devfreq->dev, "failed create sysfs for devfreq data\n");
		remove_vote_data(dev, data);
	}

out:
	mutex_unlock(&vote_manager_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vote_manager_init_devfreq);

/**
 * vote_manager_remove_cpufreq - remove sysfs for userland cpu frequency voting
 * @policy: cpufreq policy
 *
 * This function remove QoS frequency requests and disable the associated sysfs
 * for userland cpu frequency voting
 *
 * Should come in pair with vote_manager_init_cpufreq()
 *
 * Return 0 if success, or a negative error code on failures.
 */
int vote_manager_remove_cpufreq(struct cpufreq_policy *policy)
{
	struct vote_data *data;
	int ret = 0;
	struct device *dev = get_cpu_device(policy->cpu);

	if (!policy)
		return -EINVAL;

	mutex_lock(&vote_manager_lock);
	data = get_cpufreq_vote_data(policy);
	if (!data) {
		dev_err(dev, "%s: cpufreq vote is not registered!\n",__func__);
		ret = -EINVAL;
		goto out;
	}
	remove_vote_data(dev, data);
	sysfs_remove_group(get_governor_parent_kobj(policy), &cpufreq_attr_group);
out:
	mutex_unlock(&vote_manager_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vote_manager_remove_cpufreq);

/**
 * vote_manager_remove_devfreq - remove sysfs for userland device frequency voting
 * @devfreq: The devfreq object
 *
 * This function remove QoS frequency requests and disable the associated sysfs
 * for userland device frequency voting
 *
 * Should come in pair with vote_manager_init_devfreq()
 *
 * Return 0 if success, or a negative error code on failures.
 */
int vote_manager_remove_devfreq(struct devfreq *devfreq)
{
	struct vote_data *data;
	int ret = 0;
	struct device *dev = devfreq->dev.parent;

	if (!devfreq)
		return -EINVAL;

	mutex_lock(&vote_manager_lock);
	data = get_devfreq_vote_data(devfreq);
	if (!data) {
		dev_err(dev, "%s: devfreq vote is not registered!\n",__func__);
		ret = -EINVAL;
		goto out;
	}
	remove_vote_data(dev, data);
	sysfs_remove_group(&devfreq->dev.kobj, &devfreq_attr_group);
out:
	mutex_unlock(&vote_manager_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(vote_manager_remove_devfreq);

MODULE_AUTHOR("Ziyi Cui <ziyic@google.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Google Vote Manager");
