/* SPDX-License-Identifier: GPL-2.0 */
/*
 * thermal_pressure.h: supports apis to apply thermal pressure
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */

#ifndef _THERMAL_PRESSURE_H_
#define _THERMAL_PRESSURE_H_

#include <linux/cpumask.h>

enum thermal_pressure_type {
	THERMAL_PRESSURE_TYPE_TJ,
	THERMAL_PRESSURE_TYPE_TSKIN,
	THERMAL_PRESSURE_TYPE_MAX
};

/* Helper function to apply thermal pressure for a given
 *  set of CPUs. This function should not be called from an
 *  interrupt context
 */
int apply_thermal_pressure(const cpumask_t cpus, const unsigned long frequency,
			   enum thermal_pressure_type type);
#endif //_THERMAL_PRESSURE_H_
