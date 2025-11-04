/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Google LWIS Regulator Interface
 *
 * Copyright (c) 2018 Google, LLC
 */

#ifndef LWIS_REGULATOR_H_
#define LWIS_REGULATOR_H_

#include <linux/device.h>
#include <linux/list.h>
#include <linux/regulator/consumer.h>
#include "lwis_commands.h"

struct lwis_regulator_info {
	struct regulator *reg;
	char name[LWIS_MAX_NAME_STRING_LEN];
	struct list_head node;
};

/*
 *  Allocate regulators info and add to the list if the name is not found in
 *  exist list. It also register the regulator.
 */
int lwis_regulator_list_add_info(struct device *dev, struct list_head *list, const char *name);

/*
 *  Free the nodes in the list.
 */
void lwis_regulator_list_free(struct list_head *list);

/*
 *  Search the lwis device regulators_list and return the lwis_regulator_info
 *  if the name is matched
 */
struct lwis_regulator_info *lwis_regulator_get_info(struct list_head *list, const char *name);

/*
 *  lwis_regulator_put: Unregister the regulator.
 *  Returns: 0 if success, -ve if error
 */
int lwis_regulator_put(struct list_head *list, char *name);

/*
 *  lwis_regulator_put_all: Unregister the all the regulators in the list
 *  Returns: 0 if success, -ve if error
 */
int lwis_regulator_put_all(struct list_head *list);

/*
 *  lwis_regulator_enable: Turn on/enable the regulator.
 *  Returns: 0 if success, -ve if error
 */
int lwis_regulator_enable(struct list_head *list, char *name);

/*
 *  lwis_regulator_disable: Turn off/disable the regulator.
 *  Returns: 0 if success, -ve if error
 */
int lwis_regulator_disable(struct list_head *list, char *name);

/*
 *  lwis_regulator_set_mode: Set the regulator operation mode.
 *  Returns: 0 if success, -ve if error
 */
int lwis_regulator_set_mode(struct list_head *list, char *name);

/*
 *  lwis_regulator_print: Debug function to print all the regulators in the
 *  supplied list.
 */
void lwis_regulator_print(struct list_head *list);

#endif /* LWIS_REGULATOR_H_ */
