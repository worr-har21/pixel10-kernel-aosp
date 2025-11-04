// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC.
 */

#include <linux/io.h>

#include "sequence.h"

static struct power_desc aon_power_desc[] = {
	{
		.default_on = true,
		.power_on = NULL,
		.power_off = NULL,
	},
};

const struct sswrp_power_desc aon_power_desc_table = {
	.reg_names = NULL,
	.region_count = 0,
	.descriptors = aon_power_desc,
	.desc_count = ARRAY_SIZE(aon_power_desc),
};
