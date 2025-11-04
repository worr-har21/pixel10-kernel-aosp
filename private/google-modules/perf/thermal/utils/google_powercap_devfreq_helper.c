// SPDX-License-Identifier: GPL-2.0-only
/*
 * google_powercap_devfreq_helper.c driver to register the devfreq nodes.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/units.h>

#include "google_powercap_devfreq.h"
#include "google_powercap_helper_mock.h"
#include "perf/core/google_pm_qos.h"

static char devfreq_names[HW_CDEV_MAX][20] = {
	[HW_CDEV_GPU] "gpu",
	[HW_CDEV_TPU] "tpu",
	[HW_CDEV_AUR] "aurora",
};

int gpc_devfreq_update_pd_power_uw(struct gpowercap *gpowercap)
{
	struct gpowercap_devfreq *gpowercap_devfreq = to_gpowercap_devfreq(gpowercap);
	struct cdev_devfreq_data *cdev = &gpowercap_devfreq->cdev;

	if (!cdev->devfreq)
		return -ENODEV;

	gpowercap->power_min = cdev->opp_table[0].power;
	gpowercap->power_max = cdev->opp_table[cdev->num_opps - 1].power;
	gpowercap->power_limit = gpowercap->power_max;
	gpowercap->num_opps = cdev->num_opps;
	gpowercap->opp_table = cdev->opp_table;
	pr_debug("Devfreq:%s powercap min power:%llu max power:%llu\n",
		 dev_name(&cdev->devfreq->dev),
		 gpowercap->power_min, gpowercap->power_max);

	return 0;
}

u64 gpc_devfreq_set_pd_power_limit(struct gpowercap *gpowercap, u64 power_limit)
{
	struct gpowercap_devfreq *gpowercap_devfreq = to_gpowercap_devfreq(gpowercap);
	unsigned long freq;
	int i;
	struct devfreq *devfreq = gpowercap_devfreq->cdev.devfreq;

	if (!devfreq)
		return 0;

	/* The power limit value here is clamped already using the min and max power.
	 * So they will not be out of bounds and no need to check that case. The index will
	 * start from 1 to avoid redundant check for idx = 0. */
	for (i = 1; i < gpowercap->num_opps; i++) {
		if (gpowercap->opp_table[i].power > power_limit)
			break;
	}

	freq = gpowercap->opp_table[i - 1].freq;
	power_limit = gpowercap->opp_table[i - 1].power;

	pr_debug("Devfreq:%s freq limit:%lu power limit:%llu",
		 dev_name(&devfreq->dev), freq, power_limit);
	cdev_pm_qos_update_request(&gpowercap_devfreq->cdev, freq);

	return power_limit;
}

u64 gpc_devfreq_get_pd_power_uw(struct gpowercap *gpowercap)
{
	struct gpowercap_devfreq *gpowercap_devfreq = to_gpowercap_devfreq(gpowercap);
	struct devfreq *devfreq = gpowercap_devfreq->cdev.devfreq;
	struct devfreq_dev_status status;
	unsigned long freq;
	int i;

	if (!devfreq)
		return 0;

	mutex_lock(&devfreq->lock);
	status = devfreq->last_status;
	mutex_unlock(&devfreq->lock);

	freq = DIV_ROUND_UP(status.current_frequency, HZ_PER_KHZ);

	for (i = 0; i < gpowercap->num_opps; i++) {

		if (gpowercap->opp_table[i].freq < freq)
			continue;

		pr_debug("Devfreq:%s freq:%u power:%uuw\n",
			dev_name(&devfreq->dev),
			gpowercap->opp_table[i].freq,
			gpowercap->opp_table[i].power);
		return gpowercap->opp_table[i].power;
	}

	return 0;
}

void gpc_devfreq_pd_release(struct gpowercap *gpowercap)
{
	struct gpowercap_devfreq *gpowercap_devfreq = to_gpowercap_devfreq(gpowercap);

	cdev_devfreq_exit(&gpowercap_devfreq->cdev);
	kfree(gpowercap_devfreq);
}

static struct gpowercap_ops gpc_devfreq_ops = {
	.set_power_uw = gpc_devfreq_set_pd_power_limit,
	.get_power_uw = gpc_devfreq_get_pd_power_uw,
	.update_power_uw = gpc_devfreq_update_pd_power_uw,
	.release = gpc_devfreq_pd_release,
};

void __gpc_devfreq_cdev_success(struct cdev_devfreq_data *cdev)
{
	struct gpowercap_devfreq *gpowercap_devfreq = container_of(cdev,
								   struct gpowercap_devfreq,
								   cdev);

	gpc_devfreq_update_pd_power_uw(&gpowercap_devfreq->gpowercap);
}

void __gpc_devfreq_cdev_exit(struct cdev_devfreq_data *cdev)
{
	struct gpowercap_devfreq *gpowercap_devfreq = container_of(cdev,
								   struct gpowercap_devfreq,
								   cdev);

	gpowercap_unregister(&gpowercap_devfreq->gpowercap);
	gpc_fatal_error();
}

int __gpc_devfreq_setup(struct gpowercap *parent, struct device_node *np, enum hw_dev_type cdev_id)
{
	struct gpowercap_devfreq *gpowercap_devfreq;
	int ret = 0;

	gpowercap_devfreq = kzalloc(sizeof(*gpowercap_devfreq), GFP_KERNEL);
	if (!gpowercap_devfreq)
		return -ENOMEM;

	ret = cdev_devfreq_init(&gpowercap_devfreq->cdev, np, cdev_id, __gpc_devfreq_cdev_success,
				__gpc_devfreq_cdev_exit);
	if (ret) {
		pr_err("Setup error:%s. ret:%d.\n", of_node_full_name(np), ret);
		kfree(gpowercap_devfreq);
		return ret;
	}
	gpowercap_init(&gpowercap_devfreq->gpowercap, &gpc_devfreq_ops);
	ret = gpowercap_register(devfreq_names[cdev_id], &gpowercap_devfreq->gpowercap, parent);
	if (ret) {
		pr_err("Failed to register '%s': %d\n", of_node_full_name(np), ret);
		cdev_devfreq_exit(&gpowercap_devfreq->cdev);
		kfree(gpowercap_devfreq);
		return ret;
	}

	return 0;
}
