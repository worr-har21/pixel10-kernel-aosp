// SPDX-License-Identifier: GPL-2.0-only
/*
 * google_powercap_tree.c driver to register the powercap tree.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "google_powercap.h"

static struct gpowercap_node lga_tree[] __initdata = {
	[0] { .name = "package",
		.type =  GPOWERCAP_NODE_VIRTUAL },
	[1] { .name = "soc",
		.type = GPOWERCAP_NODE_VIRTUAL,
		.parent = &lga_tree[0] },
	[2] { .name = "/cpus/cpu@0", // Little CPU cluster
		.type = GPOWERCAP_NODE_CPU,
		.cdev_id = HW_CDEV_LIT,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[3] { .name = "/cpus/cpu@200", // Mid CPU cluster
		.type = GPOWERCAP_NODE_CPU,
		.cdev_id = HW_CDEV_MID,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[4] { .name = "/cpus/cpu@500", // BIG-MID CPU cluster
		.type = GPOWERCAP_NODE_CPU,
		.cdev_id = HW_CDEV_BIG_MID,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[5] { .name = "/cpus/cpu@700", // BIG CPU cluster
		.type = GPOWERCAP_NODE_CPU,
		.cdev_id = HW_CDEV_BIG,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[6] { .name = "/gpu0@34800000", // GPU
		.type = GPOWERCAP_NODE_DEVFREQ,
		.cdev_id = HW_CDEV_GPU,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[7] { .name = "/sswrp_tpu@36000000/buenos@500000", // TPU
		.type = GPOWERCAP_NODE_DEVFREQ,
		.cdev_id = HW_CDEV_TPU,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[8] { .name = "/sswrp_aur@38000000/gxp@880000", // Aurora
		.type = GPOWERCAP_NODE_DEVFREQ,
		.cdev_id = HW_CDEV_AUR,
		.parent = &lga_tree[1] }, // parent = 'soc'
	[9] { },
};

static struct of_device_id gpowercap_platform_tree_data[] __initdata = {
	{
		.compatible = "google,lga",
		.data = lga_tree,
	},
	{},
};

static int __init powercap_tree_init(void)
{
	return gpowercap_create_hierarchy(gpowercap_platform_tree_data);
}
module_init(powercap_tree_init);

static void __exit powercap_tree_exit(void)
{
	return gpowercap_destroy_hierarchy();
}
module_exit(powercap_tree_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ram Chandrasekar <rchandrasekar@google.com>");
MODULE_DESCRIPTION("Google LLC powercap driver");
MODULE_ALIAS("platform:google_powercap");
