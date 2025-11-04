/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * google_powercap.h Google powercap related functions.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#ifndef __GOOGLE_POWERCAP_H__
#define __GOOGLE_POWERCAP_H__

#include <linux/powercap.h>

#include "cdev_helper.h"
#include "thermal_cpm_mbox.h"

#define MAX_GOOGLE_POWERCAP_CONSTRAINTS 1

struct gpowercap {
	struct powercap_zone zone;
	struct gpowercap *parent;
	struct list_head siblings;
	struct list_head children;
	struct gpowercap_ops *ops;
	unsigned long flags;
	u64 power_limit;
	u64 power_max;
	u64 power_min;
	unsigned int num_opps;
	struct cdev_opp_table *opp_table;
	struct mutex lock;
	struct delayed_work bypass_work;
};

struct gpowercap_ops {
	u64 (*set_power_uw)(struct gpowercap *, u64);
	u64 (*get_power_uw)(struct gpowercap *);
	int (*update_power_uw)(struct gpowercap *);
	void (*release)(struct gpowercap *);
};

struct device_node;

struct gpowercap_subsys_ops {
	const char *name;
	void (*exit)(void);
	int (*setup)(struct gpowercap *gpc, struct device_node *np,
		     enum hw_dev_type cdev_id);
};

extern struct gpowercap_subsys_ops gpc_cpu_dev_ops;
extern struct gpowercap_subsys_ops gpc_devfreq_dev_ops;
extern struct gpowercap_subsys_ops gpc_test_device_ops;

static struct gpowercap_subsys_ops *gpc_device_ops[] = {
	&gpc_cpu_dev_ops, // GPOWERCAP_NODE_CPU
	&gpc_devfreq_dev_ops, // GPOWERCAP_NODE_DEVFREQ
	&gpc_test_device_ops, // GPOWERCAP_NODE_TEST_DT
};

enum GPOWERCAP_NODE_TYPE {
	GPOWERCAP_NODE_CPU = 0,
	GPOWERCAP_NODE_DEVFREQ,
	GPOWERCAP_NODE_TEST_DT,
	// All actual nodes should be before virtual type node.
	GPOWERCAP_NODE_VIRTUAL,
	GPOWERCAP_NODE_TEST_VIRTUAL,
};

struct gpowercap_node {
	enum GPOWERCAP_NODE_TYPE type;
	const char *name;
	struct gpowercap_node *parent;
	enum hw_dev_type cdev_id;
};

static inline struct powercap_zone *to_powercap_zone(struct device *dev)
{
	return container_of(dev, struct powercap_zone, dev);
}

static inline struct gpowercap *to_gpowercap(struct powercap_zone *zone)
{
	return container_of(zone, struct gpowercap, zone);
}
void gpowercap_init(struct gpowercap *gpowercap, struct gpowercap_ops *ops);
int gpowercap_update_power(struct gpowercap *gpowercap);
int gpowercap_release_zone(struct powercap_zone *pcz);
void gpowercap_unregister(struct gpowercap *gpowercap);
int gpowercap_register(const char *name, struct gpowercap *gpowercap, struct gpowercap *parent);
int gpowercap_create_hierarchy(struct of_device_id *gpowercap_match_table);
void gpowercap_destroy_hierarchy(void);

#endif //__GOOGLE_POWERCAP_H__
