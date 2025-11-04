/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cdev_helper.h Thermal cooling device helper.
 *
 * Copyright (c) 2025, Google LLC. All rights reserved.
 */
#ifndef _CDEV_HELPER_H_
#define _CDEV_HELPER_H_

struct cdev_opp_table {
	unsigned int power;
	unsigned int freq;
	unsigned int voltage;
};

#endif  // _CDEV_HELPER_H_
