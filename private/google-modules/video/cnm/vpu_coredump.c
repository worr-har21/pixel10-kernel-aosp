// SPDX-License-Identifier: GPL-2.0-only
/*
 * SLC operations for Codec3P
 *
 * Copyright 2024 Google LLC.
 *
 * Author: Jerry Huang <huangjerry@google.com>
 */


#include <linux/module.h>

#include "vpu_priv.h"
#include "vpu_coredump.h"


static void vpu_core_sscd_release(struct device *dev) {}

int vpu_sscd_dev_register(struct vpu_core *core)
{
	int res;

	struct platform_device *pdev = &core->sscd_pdev;

	pdev->name = VPU_CORE_NAME;
	pdev->id = PLATFORM_DEVID_NONE;
	pdev->driver_override = SSCD_NAME;
	pdev->dev.platform_data = &core->vpu_core_sscd_platdata;
	pdev->dev.release = vpu_core_sscd_release;

	res = platform_device_register(&core->sscd_pdev);

	return res;
}

void vpu_sscd_dev_unregister(struct vpu_core *core)
{
	platform_device_unregister(&core->sscd_pdev);
}

int vpu_do_sscoredump(struct vpu_core *core, const struct vpu_dump_info *dbg_info)
{
	struct sscd_platform_data *sscd_platdata;
	struct sscd_segment seg;

	dev_dbg(core->dev, "VPU core dump\n");
	sscd_platdata = dev_get_platdata(&core->sscd_pdev.dev);
	if (!sscd_platdata->sscd_report) {
		dev_dbg(core->dev, "No sscd_report\n");
		return -EINVAL;
	}

	memset(&seg, 0, sizeof(seg));
	seg.addr = dbg_info->addr;
	seg.size = dbg_info->size;

	return sscd_platdata->sscd_report(&core->sscd_pdev, &seg, 1,
		SSCD_FLAGS_ELFARM64HDR, "VPU crash\n");
}
