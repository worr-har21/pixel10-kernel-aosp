/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * google_powercap_devfreq.h Google devfreq powercap related functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#ifndef _GOOGLE_POWERCAP_DEVFREQ_H_
#define _GOOGLE_POWERCAP_DEVFREQ_H_

#include <linux/devfreq.h>
#include <linux/energy_model.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#include "cdev_devfreq_helper.h"
#include "google_powercap.h"

struct gpowercap_devfreq {
	struct gpowercap gpowercap;
	struct cdev_devfreq_data cdev;
};

static inline struct gpowercap_devfreq *to_gpowercap_devfreq(struct gpowercap *gpowercap)
{
	return container_of(gpowercap, struct gpowercap_devfreq, gpowercap);
}

int gpc_devfreq_update_pd_power_uw(struct gpowercap *gpowercap);
u64 gpc_devfreq_set_pd_power_limit(struct gpowercap *gpowercap, u64 power_limit);
u64 gpc_devfreq_get_pd_power_uw(struct gpowercap *gpowercap);
void gpc_devfreq_pd_release(struct gpowercap *gpowercap);
int __gpc_devfreq_setup(struct gpowercap *parent, struct device_node *np, enum hw_dev_type cdev_id);
#endif  // _GOOGLE_POWERCAP_DEVFREQ_H_
