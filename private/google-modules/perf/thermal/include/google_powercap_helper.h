/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * google_powercap_helper.h Google powercap related helper functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#ifndef _GOOGLE_POWERCAP_HELPER_H_
#define _GOOGLE_POWERCAP_HELPER_H_

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/of.h>

#include "google_powercap.h"

#define GPOWERCAP_POWER_LIMIT_FLAG			0
#define GPOWERCAP_POWER_LIMIT_BYPASS_FLAG		1
#define GPOWERCAP_POWER_LIMIT_BYPASS_TIME_MSEC_MAX	5000
#define GPOWERCAP_CONTROL_TYPE "gpc"

int __get_power_uw(struct gpowercap *gpowercap, u64 *power_uw);
void __gpowercap_sub_power(struct gpowercap *gpowercap);
void __gpowercap_add_power(struct gpowercap *gpowercap);
int __gpowercap_update_power(struct gpowercap *gpowercap);
int __gpowercap_release_zone(struct powercap_zone *pcz);
int __set_power_limit_uw(struct gpowercap *gpowercap, u64 power_limit);
ssize_t __power_levels_uw_show(struct gpowercap *gpowercap, char *buf);
int __gpowercap_register(const char *name, struct gpowercap *gpowercap, struct gpowercap *parent);
struct gpowercap *__gpowercap_setup_virtual(const struct gpowercap_node *hierarchy,
					    struct gpowercap *parent);
struct gpowercap *__gpowercap_setup_leaf(const struct gpowercap_node *hierarchy,
					 struct gpowercap *parent);
int __for_each_powercap_child(const struct gpowercap_node *hierarchy,
			      const struct gpowercap_node *it, struct gpowercap *parent);
int __gpowercap_create_hierarchy(struct of_device_id *gpowercap_match_table);
void __gpowercap_destroy_tree_recursive(struct gpowercap *gpowercap);
void __gpowercap_destroy_hierarchy(void);
int __power_limit_bypass(struct gpowercap *gpowercap, int time_ms);

#endif  // _GOOGLE_POWERCAP_HELPER_H_
