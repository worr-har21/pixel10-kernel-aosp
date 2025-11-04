/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cdev_helper.h Thermal cooling device helper.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */
#ifndef _GOOGLE_UCLAMP_CDEV_H_
#define _GOOGLE_UCLAMP_CDEV_H_

extern struct pixel_em_profile **vendor_sched_pixel_em_profile;

struct thermal_uclamp_cdev {
	unsigned int cpu;
	unsigned int cur_state;
	unsigned int max_state;
	struct thermal_cooling_device *cdev;
	struct cdev_opp_table *opp_table;
	struct list_head cdev_list;
};

static LIST_HEAD(therm_uclamp_cdev_list);
static DEFINE_MUTEX(therm_cdev_list_lock);

#endif  // _GOOGLE_UCLAMP_CDEV_H_
