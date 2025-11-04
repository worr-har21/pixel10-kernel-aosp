// SPDX-License-Identifier: GPL-2.0-only
/*
 * google_powercap_helper.c driver to register the powercap nodes and tree.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "google_powercap_helper.h"
#include "google_powercap_helper_mock.h"

static DEFINE_MUTEX(gpowercap_lock);
static struct powercap_control_type *pct;
static struct gpowercap *root;

static const char *constraint_name[] = {
	"Instantaneous",
};

struct gpowercap_subsys_ops gpc_test_device_ops = {
	.name = "powercap_test_ops",
	.setup = gpc_test_setup_dt,
	.exit = gpc_test_exit,
};

static int get_time_window_us(struct powercap_zone *pcz, int cid, u64 *window)
{
	return -ENOSYS;
}

static int set_time_window_us(struct powercap_zone *pcz, int cid, u64 window)
{
	return -ENOSYS;
}

static int get_max_power_range_uw(struct powercap_zone *pcz, u64 *max_power_uw)
{
	struct gpowercap *gpowercap = to_gpowercap(pcz);

	*max_power_uw = gpowercap->power_max - gpowercap->power_min;

	return 0;
}

static int get_power_uw(struct powercap_zone *pcz, u64 *power_uw)
{
	return __get_power_uw(to_gpowercap(pcz), power_uw);
}

/**
 * gpowercap_update_power - Update the power on the gpowercap
 * @gpowercap: a pointer to a gpowercap structure to update
 *
 * Function to update the power values of the gpowercap node specified in
 * parameter. These new values will be propagated to the tree.
 *
 * Return: zero on success, -EINVAL if the values are inconsistent
 */
int gpowercap_update_power(struct gpowercap *gpowercap)
{
	return __gpowercap_update_power(gpowercap);
}

/**
 * gpowercap_release_zone - Cleanup when the node is released
 * @pcz: a pointer to a powercap_zone structure
 *
 * Do some housecleaning and update the power on the tree. The
 * release will be denied if the node has children. This function must
 * be called by the specific release callback of the different
 * backends.
 *
 * Return: 0 on success, -EBUSY if there are children
 */
int gpowercap_release_zone(struct powercap_zone *pcz)
{
	return __gpowercap_release_zone(pcz);
}

static int get_power_limit_uw(struct powercap_zone *pcz,
			      int cid, u64 *power_limit)
{
	*power_limit = to_gpowercap(pcz)->power_limit;

	return 0;
}

static int set_power_limit_uw(struct powercap_zone *pcz, int cid, u64 power_limit)
{
	struct gpowercap *gpowercap = to_gpowercap(pcz);

	return __set_power_limit_uw(gpowercap, power_limit);
}

static const char *get_constraint_name(struct powercap_zone *pcz, int cid)
{
	return constraint_name[cid];
}

static int get_max_power_uw(struct powercap_zone *pcz, int id, u64 *max_power)
{
	*max_power = to_gpowercap(pcz)->power_max;

	return 0;
}

static struct powercap_zone_constraint_ops constraint_ops = {
	.set_power_limit_uw = set_power_limit_uw,
	.get_power_limit_uw = get_power_limit_uw,
	.set_time_window_us = set_time_window_us,
	.get_time_window_us = get_time_window_us,
	.get_max_power_uw = get_max_power_uw,
	.get_name = get_constraint_name,
};

static struct powercap_zone_ops zone_ops = {
	.get_max_power_range_uw = get_max_power_range_uw,
	.get_power_uw = get_power_uw,
	.release = gpowercap_release_zone,
};

int __get_power_uw(struct gpowercap *gpowercap, u64 *power_uw)
{
	struct gpowercap *child;
	u64 power;
	int ret = 0;

	if (gpowercap->ops) {
		// Opps not initialized yet.
		if (!gpowercap->num_opps)
			return -EAGAIN;

		*power_uw = gpowercap->ops->get_power_uw(gpowercap);
		return 0;
	}

	*power_uw = 0;

	list_for_each_entry(child, &gpowercap->children, siblings) {
		ret = __get_power_uw(child, &power);
		if (ret)
			break;
		*power_uw += power;
	}

	return ret;
}

void __gpowercap_sub_power(struct gpowercap *gpowercap)
{
	struct gpowercap *parent = gpowercap->parent;

	while (parent) {
		parent->power_min -= gpowercap->power_min;
		parent->power_max -= gpowercap->power_max;
		parent->power_limit -= gpowercap->power_limit;
		parent = parent->parent;
	}
}

void __gpowercap_add_power(struct gpowercap *gpowercap)
{
	struct gpowercap *parent = gpowercap->parent;

	while (parent) {
		parent->power_min += gpowercap->power_min;
		parent->power_max += gpowercap->power_max;
		parent->power_limit += gpowercap->power_limit;
		parent = parent->parent;
	}
}

int __gpowercap_update_power(struct gpowercap *gpowercap)
{
	int ret = 0;

	__gpowercap_sub_power(gpowercap);

	ret = gpowercap->ops->update_power_uw(gpowercap);
	if (ret)
		pr_err("Failed to update power for '%s': %d\n",
		       gpowercap->zone.name, ret);

	if (!test_bit(GPOWERCAP_POWER_LIMIT_FLAG, &gpowercap->flags))
		gpowercap->power_limit = gpowercap->power_max;

	__gpowercap_add_power(gpowercap);

	return ret;
}

int __gpowercap_release_zone(struct powercap_zone *pcz)
{
	struct gpowercap *gpowercap = to_gpowercap(pcz);
	struct gpowercap *parent = gpowercap->parent;

	if (!list_empty(&gpowercap->children))
		return -EBUSY;

	if (parent)
		list_del(&gpowercap->siblings);

	__gpowercap_sub_power(gpowercap);

	if (gpowercap->ops)
		gpowercap->ops->release(gpowercap);
	else
		kfree(gpowercap);

	return 0;
}

int __set_power_limit_uw(struct gpowercap *gpowercap, u64 power_limit)
{
	// Intermediate nodes will be a NOP for set power for now.
	if (!gpowercap->ops)
		return 0;

	// Opps not initialized yet.
	if (!gpowercap->num_opps)
		return -EAGAIN;

	/*
	 * Don't allow values outside of the power range previously
	 * set when initializing the power numbers.
	 */
	power_limit = clamp_val(power_limit, gpowercap->power_min, gpowercap->power_max);

	mutex_lock(&gpowercap->lock);
	/*
	 * A max power limitation means we remove the power limit,
	 * otherwise we set a constraint and flag the gpowercap node.
	 */
	if (power_limit == gpowercap->power_max)
		clear_bit(GPOWERCAP_POWER_LIMIT_FLAG, &gpowercap->flags);
	else
		set_bit(GPOWERCAP_POWER_LIMIT_FLAG, &gpowercap->flags);

	pr_debug("%s: power limit: %llu uW, power max: %llu uW\n",
		 gpowercap->zone.name, gpowercap->power_limit, gpowercap->power_max);

	if (test_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG, &gpowercap->flags))
		gpowercap->power_limit = power_limit;
	else
		gpowercap->power_limit = gpowercap->ops->set_power_uw(gpowercap, power_limit);

	mutex_unlock(&gpowercap->lock);

	return 0;
}

void __power_limit_bypass_clear_work(struct work_struct *work)
{
	struct gpowercap *gpowercap = container_of(work, struct gpowercap,
						   bypass_work.work);

	mutex_lock(&gpowercap->lock);
	clear_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG, &gpowercap->flags);
	if (test_bit(GPOWERCAP_POWER_LIMIT_FLAG, &gpowercap->flags)) {
		gpowercap->power_limit = gpowercap->ops->set_power_uw(gpowercap,
								      gpowercap->power_limit);
		pr_info("%s: Power limit bypass cleared. limit:%llu uW applied\n",
			gpowercap->zone.name, gpowercap->power_limit);
	}
	mutex_unlock(&gpowercap->lock);
}

int __power_limit_bypass(struct gpowercap *gpowercap, int time_ms)
{
	int bypass_time_msec = 0;

	mutex_lock(&gpowercap->lock);
	bypass_time_msec = clamp_val(time_ms, 0, GPOWERCAP_POWER_LIMIT_BYPASS_TIME_MSEC_MAX);

	set_bit(GPOWERCAP_POWER_LIMIT_BYPASS_FLAG, &gpowercap->flags);
	if (test_bit(GPOWERCAP_POWER_LIMIT_FLAG, &gpowercap->flags) && bypass_time_msec) {
		gpowercap->ops->set_power_uw(gpowercap, gpowercap->power_max);
		pr_info("%s: Power limit bypass for %d milli-seconds\n",
			gpowercap->zone.name, bypass_time_msec);
	}
	mutex_unlock(&gpowercap->lock);

	mod_delayed_work(system_highpri_wq, &gpowercap->bypass_work,
			 msecs_to_jiffies(bypass_time_msec));

	return bypass_time_msec;
}

static ssize_t power_limit_bypass_msec_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct gpowercap *gpowercap;
	struct powercap_zone *pcz = to_powercap_zone(dev);
	int time_ms;

	if (!pcz)
		return -ENODEV;

	gpowercap = to_gpowercap(pcz);
	if (!gpowercap)
		return -ENODEV;

	if (kstrtoint(buf, 10, &time_ms))
		return -EINVAL;

	__power_limit_bypass(gpowercap, time_ms);

	return count;
}

static DEVICE_ATTR_WO(power_limit_bypass_msec);

ssize_t __power_levels_uw_show(struct gpowercap *gpowercap, char *buf)
{
	int i, buf_ct = 0;

	// Opps not initialized yet.
	if (!gpowercap->num_opps)
		return -EAGAIN;

	for (i = (gpowercap->num_opps - 1); i >= 0; i--)
		buf_ct += sysfs_emit_at(buf, buf_ct, "%u ",
					gpowercap->opp_table[i].power);
	buf_ct += sysfs_emit_at(buf, buf_ct, "\n");

	return buf_ct;
}

ssize_t
power_levels_uw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gpowercap *gpowercap;
	struct powercap_zone *pcz = to_powercap_zone(dev);

	if (!pcz)
		return -ENODEV;

	gpowercap = to_gpowercap(pcz);
	if (!gpowercap)
		return -ENODEV;

	return __power_levels_uw_show(gpowercap, buf);
}

static DEVICE_ATTR_RO(power_levels_uw);

int __gpowercap_register(const char *name, struct gpowercap *gpowercap, struct gpowercap *parent)
{
	struct powercap_zone *pcz;
	int ret = 0;

	if (!pct)
		return -EAGAIN;

	if (root && !parent)
		return -EBUSY;

	if (!root && parent)
		return -EINVAL;

	if (parent && parent->ops)
		return -EINVAL;

	if (!gpowercap)
		return -EINVAL;

	if (gpowercap->ops && !(gpowercap->ops->set_power_uw &&
			   gpowercap->ops->get_power_uw &&
			   gpowercap->ops->update_power_uw &&
			   gpowercap->ops->release))
		return -EINVAL;

	pcz = register_zone(&gpowercap->zone, pct, name, parent ? &parent->zone : NULL,
			    &zone_ops, MAX_GOOGLE_POWERCAP_CONSTRAINTS, &constraint_ops);
	if (IS_ERR(pcz))
		return PTR_ERR(pcz);

	if (parent) {
		list_add_tail(&gpowercap->siblings, &parent->children);
		gpowercap->parent = parent;
	} else {
		root = gpowercap;
	}

	if (gpowercap->ops && !gpowercap->ops->update_power_uw(gpowercap)) {
		__gpowercap_add_power(gpowercap);
		gpowercap->power_limit = gpowercap->power_max;
	}

	if (gpowercap->ops) {
		pr_debug("power cap zone:%s create power_level\n", gpowercap->zone.name);
		ret = dev_create_file(&gpowercap->zone.dev, &dev_attr_power_levels_uw);
		if (ret)
			pr_warn("Power cap zone:%s power level sysfs creation err:%d\n",
				gpowercap->zone.name, ret);
		ret = dev_create_file(&gpowercap->zone.dev, &dev_attr_power_limit_bypass_msec);
		if (ret)
			pr_warn("Power cap zone:%s power level sysfs creation err:%d\n",
				gpowercap->zone.name, ret);
	}

	pr_debug("Registered gpowercap node '%s' / %llu-%llu uW, \n",
		 gpowercap->zone.name, gpowercap->power_min, gpowercap->power_max);

	return 0;
}

struct gpowercap *__gpowercap_setup_virtual(const struct gpowercap_node *hierarchy,
					    struct gpowercap *parent)
{
	struct gpowercap *gpowercap;
	int ret = 0;

	gpowercap = kzalloc(sizeof(*gpowercap), GFP_KERNEL);
	if (!gpowercap)
		return ERR_PTR(-ENOMEM);
	gpowercap_init(gpowercap, NULL);

	ret = __gpowercap_register(hierarchy->name, gpowercap, parent);
	if (ret) {
		pr_err("Failed to register gpowercap node '%s': %d\n",
		       hierarchy->name, ret);
		kfree(gpowercap);
		return ERR_PTR(ret);
	}

	return gpowercap;
}

struct gpowercap *__gpowercap_setup_leaf(const struct gpowercap_node *hierarchy,
					 struct gpowercap *parent)
{
	struct device_node *np;
	int ret = 0;

	if (hierarchy->type >= ARRAY_SIZE(gpc_device_ops)) {
		pr_err("Missing ops for type:%d\n", hierarchy->type);
		return ERR_PTR(-ENODEV);
	}
	if (!gpc_device_ops[hierarchy->type]->setup) {
		pr_err("Missing setup for type:%d\n", hierarchy->type);
		return ERR_PTR(-ENODEV);
	}

	if (hierarchy->cdev_id >= HW_CDEV_MAX) {
		pr_err("Invalid cdev ID:%d for type:%s\n",
		       hierarchy->cdev_id, hierarchy->name);
		return ERR_PTR(-EINVAL);
	}

	np = of_find_node_by_path(hierarchy->name);
	if (!np) {
		pr_err("Failed to find '%s'\n", hierarchy->name);
		return ERR_PTR(-ENXIO);
	}

	ret = gpc_device_ops[hierarchy->type]->setup(parent, np, hierarchy->cdev_id);
	if (ret) {
		pr_err("Failed to setup '%s': %d\n", gpc_device_ops[hierarchy->type]->name, ret);
		of_node_put(np);
		return ERR_PTR(ret);
	}

	of_node_put(np);

	/*
	 * By returning a NULL pointer, we let know the caller there
	 * is no child for us as we are a leaf of the tree
	 */
	return NULL;
}

/**
 * __for_each_powercap_child - traverse the input array and create powercap
 * nodes.
 *
 * Traverse the array recursively and create individual nodes and their
 * childrens. Based on node type, their corresponding setup callback will
 * be called and the children list, siblings list and parent pointer will
 * be initialized.
 *
 * Return: 0 on success,
 * Error returned by the individual setup functions.
 */
int __for_each_powercap_child(const struct gpowercap_node *hierarchy,
			    const struct gpowercap_node *it, struct gpowercap *parent)
{
	struct gpowercap *gpowercap;
	int i, ret = 0;

	for (i = 0; hierarchy[i].name; i++) {

		if (hierarchy[i].parent != it)
			continue;

		switch (hierarchy[i].type) {
		case GPOWERCAP_NODE_VIRTUAL:
			gpowercap = __gpowercap_setup_virtual(&hierarchy[i], parent);
			break;
		case GPOWERCAP_NODE_TEST_VIRTUAL:
			gpowercap = gpc_test_setup(&hierarchy[i], parent);
			break;
		default:
			gpowercap = __gpowercap_setup_leaf(&hierarchy[i], parent);
			break;
		}

		/*
		 * A NULL pointer means there is no children, hence we
		 * continue without going deeper in the recursivity.
		 */
		if (!gpowercap)
			continue;

		if (IS_ERR(gpowercap)) {
			pr_warn("Failed to create '%s' in the hierarchy\n",
				hierarchy[i].name);
			continue;
		}

		ret = __for_each_powercap_child(hierarchy, &hierarchy[i], gpowercap);
		if (ret)
			return ret;
	}

	return 0;
}

int __gpowercap_create_hierarchy(struct of_device_id *gpowercap_match_table)
{
	const struct of_device_id *match;
	const struct gpowercap_node *hierarchy;
	struct device_node *np;
	int ret = 0;

	mutex_lock(&gpowercap_lock);

	if (pct) {
		ret = -EBUSY;
		goto out_unlock;
	}

	pct = register_control_type(NULL, GPOWERCAP_CONTROL_TYPE, NULL);
	if (IS_ERR(pct)) {
		pr_err("Failed to register control type\n");
		ret = PTR_ERR(pct);
		goto out_pct;
	}

	ret = -ENODEV;
	np = of_find_node_by_path("/");
	if (!np)
		goto out_err;

	match = match_of_node(gpowercap_match_table, np);

	of_node_put(np);

	if (!match)
		goto out_err;

	hierarchy = match->data;
	if (!hierarchy) {
		ret = -EFAULT;
		goto out_err;
	}

	ret = __for_each_powercap_child(hierarchy, NULL, NULL);
	if (ret)
		goto out_err;

	mutex_unlock(&gpowercap_lock);

	return 0;

out_err:
	unregister_control_type(pct);
out_pct:
	pct = NULL;
out_unlock:
	mutex_unlock(&gpowercap_lock);

	return ret;
}

void __gpowercap_destroy_tree_recursive(struct gpowercap *gpowercap)
{
	struct gpowercap *child, *aux;

	list_for_each_entry_safe(child, aux, &gpowercap->children, siblings)
		__gpowercap_destroy_tree_recursive(child);

	/*
	 * At this point, we know all children were removed from the
	 * recursive call before
	 */
	gpowercap_unregister(gpowercap);
}

void __gpowercap_destroy_hierarchy(void)
{
	int i;

	mutex_lock(&gpowercap_lock);
	if (!pct)
		goto out_unlock;
	if (root)
		__gpowercap_destroy_tree_recursive(root);

	for (i = 0; i < ARRAY_SIZE(gpc_device_ops); i++) {

		if (!gpc_device_ops[i]->exit)
			continue;

		gpc_device_ops[i]->exit();
	}
	unregister_control_type(pct);
	pct = NULL;
	root = NULL;
out_unlock:
	mutex_unlock(&gpowercap_lock);
}

/**
 * gpowercap_register - Register a gpowercap node in the hierarchy tree
 * @name: a string specifying the name of the node
 * @gpowercap: a pointer to a gpowercap structure corresponding to the new node
 * @parent: a pointer to a gpowercap structure corresponding to the parent node
 *
 * Create a gpowercap node in the tree. If no parent is specified, the node
 * is the root node of the hierarchy. If the root node already exists,
 * then the registration will fail. The powercap controller must be
 * initialized before calling this function.
 *
 * The gpowercap structure must be initialized with the power numbers
 * before calling this function.
 *
 * Return: zero on success, a negative value in case of error:
 *  -EAGAIN: the function is called before the framework is initialized.
 *  -EBUSY: the root node is already inserted
 *  -EINVAL: * there is no root node yet and @parent is specified
 *           * no all ops are defined
 *           * parent have ops which are reserved for leaves
 *   Other negative values are reported back from the powercap framework
 */
int gpowercap_register(const char *name, struct gpowercap *gpowercap, struct gpowercap *parent)
{
	return __gpowercap_register(name, gpowercap, parent);
}

/**
 * gpowercap_unregister - Unregister a gpowercap node from the hierarchy tree
 * @gpowercap: a pointer to a gpowercap structure corresponding to the node to be removed
 *
 * Call the underlying powercap unregister function. That will call
 * the release callback of the powercap zone.
 */
void gpowercap_unregister(struct gpowercap *gpowercap)
{
	pr_debug("Unregistered gpowercap node '%s'\n", gpowercap->zone.name);
	cancel_delayed_work_sync(&gpowercap->bypass_work);
	unregister_zone(pct, &gpowercap->zone);
}

/**
 * gpowercap_create_hierarchy - Create the gpowercap hierarchy
 * @hierarchy: An array of struct gpowercap_node describing the hierarchy
 *
 * The function is called by the platform specific code with the
 * description of the different node in the hierarchy. It creates the
 * tree in the sysfs filesystem under the powercap gpowercap entry.
 *
 * The expected tree has the format:
 *
 * struct gpowercap_node hierarchy[] = {
 *	[0] { .name = "topmost", type =  GPOWERCAP_NODE_VIRTUAL },
 *	[1] { .name = "package", .type = GPOWERCAP_NODE_VIRTUAL, .parent = &hierarchy[0] },
 *	[2] { .name = "/cpus/cpu0", .type = GPOWERCAP_NODE_CPU, .parent = &hierarchy[1] },
 *	[3] { .name = "/cpus/cpu1", .type = GPOWERCAP_NODE_CPU, .parent = &hierarchy[1] },
 *	[4] { .name = "/cpus/cpu2", .type = GPOWERCAP_NODE_CPU, .parent = &hierarchy[1] },
 *	[5] { .name = "/cpus/cpu3", .type = GPOWERCAP_NODE_CPU, .parent = &hierarchy[1] },
 *	[6] { }
 * };
 *
 * The last element is always an empty one and marks the end of the
 * array.
 *
 * Return: zero on success, a negative value in case of error. Errors
 * are reported back from the underlying functions.
 */
int gpowercap_create_hierarchy(struct of_device_id *gpowercap_match_table)
{
	return __gpowercap_create_hierarchy(gpowercap_match_table);
}

void gpowercap_destroy_hierarchy(void)
{
	return __gpowercap_destroy_hierarchy();
}

/**
 * gpowercap_init - Allocate and initialize a gpowercap struct.
 *
 * @gpowercap: The gpowercap struct pointer to be initialized
 * @ops: The gpowercap device specific ops, NULL for a virtual node
 */
void gpowercap_init(struct gpowercap *gpowercap, struct gpowercap_ops *ops)
{
	if (gpowercap) {
		INIT_LIST_HEAD(&gpowercap->children);
		INIT_LIST_HEAD(&gpowercap->siblings);
		gpowercap->ops = ops;
		mutex_init(&gpowercap->lock);
		INIT_DEFERRABLE_WORK(&gpowercap->bypass_work,
				     __power_limit_bypass_clear_work);
	}
}
