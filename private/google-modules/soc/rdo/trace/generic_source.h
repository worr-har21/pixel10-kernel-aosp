/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Google LLC
 */

#ifndef GENERIC_SOURCE_H_
#define GENERIC_SOURCE_H_

#include <asm/local.h>
#include <linux/coresight.h>

struct soc_source_drvdata {
	struct coresight_device *csdev;
	local_t mode;
};

#endif  // GENERIC_SOURCE_H_
