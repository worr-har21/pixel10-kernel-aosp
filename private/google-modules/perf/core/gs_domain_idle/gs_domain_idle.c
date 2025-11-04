// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC.
 *
 * Google domain idle governor.
 *
 * Made with inspiration from MailBox File System Mirror
 */

#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/pm_domain.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include "linux/notifier.h"

#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/hrtimer.h>
#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/of.h>

#include <linux/of_device.h>
#include <linux/types.h>
#include <linux/errno.h>

#include <perf/core/gs_domain_idle.h>
#include <trace/events/power.h>

#define GENPD_MAX_FILE_NAME_SIZE 128
#define NS_PER_US 1000
#define C4_DISABLE_MIN_RES_US 99999999

#define DEV_ERR(dev, fmt, ...) dev_err(dev, "%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define DEV_INFO(dev, fmt, ...) dev_info(dev, "%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define DEV_DBG(dev, fmt, ...) dev_dbg(dev, fmt, ##__VA_ARGS__)

/**
 * gs_domain_parameter_type - Enum for genpd parameters.
 *
 * Used to differentiate between min-residency, exit-latency, and
 * entry-latency.
 */
enum gs_domain_parameter_type {
	GS_DOMAIN_RESIDENCY,
	GS_DOMAIN_ENTRY_LATENCY,
	GS_DOMAIN_EXIT_LATENCY
};

/**
 * gs_domain_data - Data container for domain idle data
 */
struct gs_domain_data {
	struct notifier_block nb;		/* Notifier for idle entry / exit state change. */
	struct generic_pm_domain *genpd;	/* Genpd associated with this power domain. */
	int num_attached_policies_ids;		/* Which index is the power domain. */
	int *attached_policies_ids;		/* List of attached cpu policies. */
	bool is_initialized;			/* Is this domain initilalized. */
	int c4_disable_cnt;			/* used to track enable/disable C4 */
	s64 default_residency_ns;		/* default min residency */
};

/* Array containing gs_domain_data for each power domain. */
static struct gs_domain_data *gs_domain_idle_data_arr;
static int top_pwr_domain_id;

/**
 * gs_domain_parameter_names - Container for parameter names.
 */
static const char * const gs_domain_parameter_names[] = {
	"min-residency-us",
	"power-on-latency-us",
	"power-off-latency-us"
};

/**
 * gs_genpd_param_file - Represents the sysfs file for a parameter.
 */
struct gs_genpd_param_file {
	struct attribute base_attr;			/* Attribute to for sysfs. */
	struct genpd_power_state *state;		/* Idle state attached. */
	struct generic_pm_domain *genpd;		/* Genpd containing the state. */
	enum gs_domain_parameter_type config_type;	/* Which parameter to store. */
	struct list_head node;
};

/**
 * gs_genpd_state_folder - Represents the sysfs folder for a genpd power state.
 */
struct gs_genpd_state_folder {
	struct device *dev;
	struct kobject base_kobj;
	struct list_head params_list;
};

/* The first parameter is cluster id, the second parameter is enable/disable. */
void (*set_cluster_enabled_cb)(int, int) = NULL;

/**
 * register_set_cluster_enabled_cb - Callback for all cluster notifiers.
 */
void register_set_cluster_enabled_cb(void (*func)(int, int))
{
	// This function could only be registered once.
	BUG_ON(set_cluster_enabled_cb);
	set_cluster_enabled_cb = func;
}
EXPORT_SYMBOL_GPL(register_set_cluster_enabled_cb);

/**
 * genpd_lock/unlock_spin - Synchronization helpers from domain.c
 */
static void genpd_lock_spin(struct generic_pm_domain *genpd)
	__acquires(&genpd->slock)
{
	unsigned long flags;

	spin_lock_irqsave(&genpd->slock, flags);
	genpd->lock_flags = flags;
}

static void genpd_unlock_spin(struct generic_pm_domain *genpd)
	__releases(&genpd->slock)
{
	spin_unlock_irqrestore(&genpd->slock, genpd->lock_flags);
}

static void gs_domain_c4_set_state(bool enable)
{
	struct gs_domain_data *genpd_data;
	struct genpd_power_state *state = NULL;

	if (!gs_domain_idle_data_arr)
		return;

	if (top_pwr_domain_id == -1)
		return;

	genpd_data = &gs_domain_idle_data_arr[top_pwr_domain_id];
	state = &genpd_data->genpd->states[0];

	genpd_lock_spin(genpd_data->genpd);

	if (enable) {
		/* Enable C4 (decrement counter). */
		if (genpd_data->c4_disable_cnt == 0) {
			DEV_ERR(&genpd_data->genpd->dev, "C4 disable counter already 0\n");
			genpd_unlock_spin(genpd_data->genpd);
			return;
		}
		genpd_data->c4_disable_cnt--;
		if (genpd_data->c4_disable_cnt == 0) {
			DEV_INFO(&genpd_data->genpd->dev, "C4 enabled");
			state->residency_ns = genpd_data->default_residency_ns;
		}
	} else {
		/* Disable C4 (increment counter). */
		if (genpd_data->c4_disable_cnt == 0) {
			DEV_INFO(&genpd_data->genpd->dev, "C4 disabled\n");
			/* on the first disable save the default residency value */
			if (genpd_data->default_residency_ns == -1)
				genpd_data->default_residency_ns = state->residency_ns;
			state->residency_ns = (u64)C4_DISABLE_MIN_RES_US * NS_PER_US;
		}
		genpd_data->c4_disable_cnt++;
	}

	genpd_unlock_spin(genpd_data->genpd);
}

/**
 * gs_domain_c4_enable - Enables the C4 idle state.
 */
void gs_domain_c4_enable(void)
{
	gs_domain_c4_set_state(true);
}
EXPORT_SYMBOL_GPL(gs_domain_c4_enable);

/**
 * gs_domain_c4_disable - Disables the C4 idle state.
 */
void gs_domain_c4_disable(void)
{
	gs_domain_c4_set_state(false);
}
EXPORT_SYMBOL_GPL(gs_domain_c4_disable);

/**
 * gs_genpd_state_param_show - Show function for genpd states.
 *
 * We expose min-residency-us, entry-latency-us, and exit-latency-us.
 */
static ssize_t gs_genpd_state_param_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct gs_genpd_param_file *idle_state_file =
		container_of(attr, struct gs_genpd_param_file, base_attr);

	int ret = 0;
	s64 parameter_value = 0;

	genpd_lock_spin(idle_state_file->genpd);
	switch (idle_state_file->config_type) {
	case GS_DOMAIN_RESIDENCY:
		parameter_value = idle_state_file->state->residency_ns;
		break;
	case GS_DOMAIN_ENTRY_LATENCY:
		parameter_value = idle_state_file->state->power_on_latency_ns;
		break;
	case GS_DOMAIN_EXIT_LATENCY:
		parameter_value = idle_state_file->state->power_off_latency_ns;
		break;
	}
	genpd_unlock_spin(idle_state_file->genpd);

	ret = sysfs_emit(buf, "%llu\n", parameter_value / NS_PER_US);

	return ret;
}


/**
 * gs_genpd_state_param_store - Store function for genpd states.
 *
 * Write function to the parameters. We find the parameter to write to using
 * config_type enum.
 */
static ssize_t gs_genpd_state_param_store(struct kobject *kobj, struct attribute *attr,
						const char *buf, size_t count)
{
	struct gs_genpd_param_file *idle_state_file = container_of(attr, struct gs_genpd_param_file,
							base_attr);
	unsigned long val;
	int ret = 0;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	genpd_lock_spin(idle_state_file->genpd);
	switch (idle_state_file->config_type) {
	case GS_DOMAIN_RESIDENCY:
		idle_state_file->state->residency_ns = val * NS_PER_US;
		break;
	case GS_DOMAIN_ENTRY_LATENCY:
		idle_state_file->state->power_on_latency_ns = val * NS_PER_US;
		break;
	case GS_DOMAIN_EXIT_LATENCY:
		idle_state_file->state->power_off_latency_ns = val * NS_PER_US;
		break;
	}
	genpd_unlock_spin(idle_state_file->genpd);

	return count;
}

static const struct sysfs_ops gs_genpd_sysfs_ops = {
	.show = gs_genpd_state_param_show,
	.store = gs_genpd_state_param_store,
};

static const struct kobj_type gs_genpd_domain_ktype = {
	.release = NULL,
	.sysfs_ops = &gs_genpd_sysfs_ops,
};

/**
 * gs_genpd_delete_state_folder - Cleans up a genpd idle state and params.
 *
 * Remove all param files and put the folder at the end.
 */
static void gs_genpd_delete_state_folder(struct device *dev, void *res)
{
	struct gs_genpd_state_folder *folder = res;
	struct gs_genpd_param_file *curr_param;

	list_for_each_entry(curr_param, &folder->params_list, node)
		sysfs_remove_file(&folder->base_kobj, &curr_param->base_attr);

	kobject_put(&folder->base_kobj);
}

/**
 * gs_domain_idle_cluster_notifier - Cleans up a genpd idle state and params.
 *
 * This function marks CPU cluster enabled and disabled based on power states.
 * Also calls a callback function for clients wanting genpd notifier info.
 *
 * @nb: Notifier Block
 * @action: GENPD action on this domain.
 * @data: Unused
 */
static int gs_domain_idle_cluster_notifier(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct gs_domain_data *genpd_data = container_of(nb, struct gs_domain_data, nb);
	int enabled;

	// Determine if we need to trigger cluster on or off
	if (action == GENPD_NOTIFY_OFF)
		enabled = 0;
	else if (action == GENPD_NOTIFY_ON)
		enabled = 1;
	else
		return NOTIFY_OK;

	// Set cluster enabled for all cpu policies attached to current power domain.
	if (likely(set_cluster_enabled_cb)) {
		int policy_idx;
		unsigned int num_policies = genpd_data->num_attached_policies_ids;

		for (policy_idx = 0; policy_idx < num_policies; policy_idx++)
			set_cluster_enabled_cb(genpd_data->attached_policies_ids[policy_idx],
						enabled);

	}
	trace_clock_set_rate(genpd_data->genpd->name, enabled, raw_smp_processor_id());

	return NOTIFY_OK;
}

/**
 * gs_domain_idle_add_idle_states_sysfs - Create sysfs structures for idle states of a genpd.
 *
 * We create a sysfs folder for the each domain state and attach sysfs nodes for each
 * of the satate parameters we wish to expose.
 *
 * @dev: The device added to this genpd.
 * @genpd_data: The power domain containing the power states.
 */
static int gs_domain_idle_add_idle_states_sysfs(struct device *dev,
					struct gs_domain_data *genpd_data)
{
	int ret;
	struct generic_pm_domain *genpd = genpd_data->genpd;
	int num_params = ARRAY_SIZE(gs_domain_parameter_names);
	int num_states = genpd->state_count;
	int state_idx, param_idx;

	for (state_idx = 0; state_idx < num_states; state_idx++) {
		// Allocate containers for both the state folder and parameter files.
		char state_sysfs_folder_name[GENPD_MAX_FILE_NAME_SIZE];
		struct gs_genpd_param_file *genpd_state_params;
		struct gs_genpd_state_folder *folder;

		folder = devres_alloc(gs_genpd_delete_state_folder, sizeof(*folder), GFP_KERNEL);
		if (!folder)
			return -ENOMEM;

		genpd_state_params =
			devm_kzalloc(dev, num_params * sizeof(*genpd_state_params), GFP_KERNEL);
		if (!genpd_state_params)
			return -ENOMEM;
		INIT_LIST_HEAD(&folder->params_list);

		// We name each domain power state using a -state-X suffix.
		scnprintf(state_sysfs_folder_name, GENPD_MAX_FILE_NAME_SIZE,
				"%s-state-%d", genpd->name, state_idx);

		folder->dev = dev;
		ret = kobject_init_and_add(&folder->base_kobj, &gs_genpd_domain_ktype, &dev->kobj,
					state_sysfs_folder_name);
		if (ret) {
			DEV_ERR(dev, "Error calling kobject_init_and_add(): %d\n", ret);
			return ret;
		}
		devres_add(dev, folder);

		// Create sysfs nodes for each of the parameters.
		for (param_idx = 0; param_idx < num_params; param_idx++) {
			struct gs_genpd_param_file *genpd_param = &genpd_state_params[param_idx];
			const char *param_name = gs_domain_parameter_names[param_idx];

			genpd_param->base_attr.name = param_name;
			genpd_param->base_attr.mode = 0644;
			genpd_param->state = &genpd->states[state_idx];
			genpd_param->genpd = genpd;
			genpd_param->config_type = param_idx;
			sysfs_attr_init(&genpd_param->base_attr);
			ret = sysfs_create_file(&folder->base_kobj, &genpd_param->base_attr);
			if (ret) {
				DEV_ERR(dev, "Create sysfs file failed: %s\n", param_name);
				return ret;
			}
			INIT_LIST_HEAD(&genpd_param->node);
			list_add(&genpd_param->node, &folder->params_list);
		}
	}
	return ret;
}

/**
 * gs_domain_idle_cleanup - Removal function for gs_domain_idle.
 *
 * Puts back notifier blocks and returns sysfs resources.
 */
void gs_domain_idle_cleanup(struct device *dev)
{
	int num_power_domains = of_get_child_count(dev->of_node);
	int cluster_idx;

	for (cluster_idx = 0; cluster_idx < num_power_domains; cluster_idx++) {
		struct gs_domain_data *genpd_data = &gs_domain_idle_data_arr[cluster_idx];
		struct genpd_power_state *state;

		/* Enable C4 on cleanup */
		if (cluster_idx == top_pwr_domain_id) {
			state = &genpd_data->genpd->states[0];
			state->residency_ns = genpd_data->default_residency_ns;
			genpd_data->c4_disable_cnt = 0;
			genpd_data->default_residency_ns = -1;
			top_pwr_domain_id = -1;
		}

		if (genpd_data->is_initialized)
			dev_pm_genpd_remove_notifier(&genpd_data->genpd->dev);

	}
}

static int gs_domain_idle_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *power_domain_np;
	struct device_node *top_np;
	int power_domain_idx = 0;
	int ret = 0;

	// Allocate containers for both the folder and files for all the parameters.
	gs_domain_idle_data_arr = devm_kzalloc(
		dev, of_get_child_count(np) * sizeof(*gs_domain_idle_data_arr), GFP_KERNEL);
	if (!gs_domain_idle_data_arr)
		return -ENOMEM;

	top_pwr_domain_id = -1;
	top_np = of_parse_phandle(np, "top-power-domain", 0);
	if (!top_np)
		DEV_ERR(dev, "top-power-domain not found");

	// Probe and populate each power domain.
	for_each_child_of_node(np, power_domain_np) {
		struct gs_domain_data *genpd_data = &gs_domain_idle_data_arr[power_domain_idx];
		struct device *genpd_dev;
		struct generic_pm_domain *genpd;

		// Modify the name to be representative of the power domain we are associating.
		dev->init_name = power_domain_np->name;
		dev->of_node = power_domain_np;

		ret = of_property_read_u32(power_domain_np, "num-cpu-freq-policies",
			&genpd_data->num_attached_policies_ids);
		if (ret) {
			DEV_ERR(dev, "Could not parse policies. Bailing.");
			return -ENODEV;
		}

		genpd_data->attached_policies_ids = devm_kzalloc(dev,
			genpd_data->num_attached_policies_ids * sizeof(u32), GFP_KERNEL);
		if (!genpd_data->attached_policies_ids)
			return -ENOMEM;

		// Populate power domain to cpu policies mapping.
		if (genpd_data->num_attached_policies_ids > 0)
			of_property_read_u32_array(power_domain_np, "cpu-freq-policies",
				genpd_data->attached_policies_ids,
					genpd_data->num_attached_policies_ids);

		genpd_dev = genpd_dev_pm_attach_by_id(dev, 0);
		if (IS_ERR_OR_NULL(genpd_dev)) {
			DEV_ERR(dev, "Could not attach device to genpd.");
			ret = PTR_ERR(genpd_dev);
			goto err_out;
		}

		/*
		 * Registering a device by overriding the init_name and of node of the parent
		 * device does not need a detach or pm_remove. Therefore, nothing needs to be
		 * undone and error handling here can be achieved by continuting.
		 */
		if (!genpd_dev->pm_domain) {
			DEV_ERR(dev, "Attached device has no pm_domain.");
			goto err_out;
		}

		genpd = container_of(genpd_dev->pm_domain, struct generic_pm_domain, domain);
		genpd_data->genpd = genpd;
		genpd_data->nb.notifier_call = gs_domain_idle_cluster_notifier;

		ret = gs_domain_idle_add_idle_states_sysfs(dev, genpd_data);
		if (ret) {
			DEV_ERR(dev, "Unable to attach sysfs nodes to pm_domain %s. Bailing.",
				genpd->name);
			goto err_out;
		}

		// Register the notifier to the genpd domain.
		ret = dev_pm_genpd_add_notifier(genpd_dev, &genpd_data->nb);
		if (ret) {
			DEV_ERR(dev, "Unable to attach notifier to pm_domain %s. Bailing.",
				genpd->name);
			goto err_out;
		}

		if (top_np && power_domain_np == top_np)
			top_pwr_domain_id = power_domain_idx;

		genpd_data->is_initialized = true;
		power_domain_idx += 1;
		genpd_data->c4_disable_cnt = 0;
		genpd_data->default_residency_ns = -1;
	}

	// Restore the old device np.
	dev->of_node = np;
	return 0;

err_out:
	dev->of_node = np;
	gs_domain_idle_cleanup(dev);
	return ret;
}

static int gs_domain_idle_driver_remove(struct platform_device *pdev)
{
	gs_domain_idle_cleanup(&pdev->dev);
	return 0;
}

static const struct of_device_id gs_domain_idle_root_match[] = {
	{
		.compatible = "google,gs_domain_idle",
	},
	{}
};

static struct platform_driver gs_domain_idle_platform_driver = {
	.probe = gs_domain_idle_driver_probe,
	.remove = gs_domain_idle_driver_remove,
	.driver = {
		.name = "gs_domain_idle_driver",
		.owner = THIS_MODULE,
		.of_match_table = gs_domain_idle_root_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(gs_domain_idle_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Google Pixel Domain Idle Governor");
MODULE_AUTHOR("Will Song <jinpengsong@google.com>");
