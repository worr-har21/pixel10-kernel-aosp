/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cdev_cpufreq_helper.h Thermal cpufreq cooling device helper.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */
#ifndef _CDEV_CPUFREQ_HELPER_H_
#define _CDEV_CPUFREQ_HELPER_H_

#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "cdev_helper.h"
#include "thermal_cpm_mbox.h"

int cdev_cpufreq_get_opp_count(unsigned int cpu);
int cdev_cpufreq_update_opp_table(unsigned int cpu,
				  enum hw_dev_type cdev_id,
				  struct cdev_opp_table *cdev_table,
				  unsigned int num_opp);
#endif  // _CDEV_CPUFREQ_HELPER_H_
