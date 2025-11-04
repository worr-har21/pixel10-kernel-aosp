/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * Google wdt header file.
 *
 * Copyright (C) 2024 Google LLC.
 */

#ifndef _GOOGLE_WDT_H
#define _GOOGLE_WDT_H

#include <linux/watchdog.h>
#include <dt-bindings/soc/google/google-wdt-def.h>

struct watchdog_device *google_wdt_wdd_get(struct device *dev);

#endif // _GOOGLE_WDT_H
