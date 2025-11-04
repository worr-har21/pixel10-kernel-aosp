/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Copyright (c) 2024 Google LLC.
 *
 * Device Tree binding constants for GOOGLE Thermal
 */
#ifndef _GOOGLE_THERMAL_DEF_H
#define _GOOGLE_THERMAL_DEF_H

/* HW thermal zones definition. Align with thermal zone definition in CPM mbox header file */
#define HW_TZ_BIG	(0)
#define HW_TZ_BIG_MID	(1)
#define HW_TZ_MID	(2)
#define HW_TZ_LIT	(3)
#define HW_TZ_GPU	(4)
#define HW_TZ_TPU	(5)
#define HW_TZ_AUR	(6)
#define HW_TZ_ISP	(7)
#define HW_TZ_MEM	(8)
#define HW_TZ_AOC	(9)
#define HW_TZ_MAX	(10)

/* HW Cdev definitions. Align with hardware cdev definition in CPM mbox header. */
#define HW_DT_CDEV_BIG		(0)
#define HW_DT_CDEV_BIG_MID	(1)
#define HW_DT_CDEV_MID		(2)
#define HW_DT_CDEV_LIT		(3)
#define HW_DT_CDEV_GPU		(4)

#endif // _GOOGLE_THERMAL_DEF_H
