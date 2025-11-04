// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Codec3P video accelerator
 *
 * Copyright 2023 Google LLC.
 *
 * Author: Vinay Kalia <vinaykalia@google.com>
 * Author: Ernie Hsu <erniehsu@google.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/of_platform.h>
#include <linux/types.h>
#include <perf/core/google_pm_qos.h>

#include "vpu_of.h"
#include "vpu_priv.h"

static int vpu_of_get_resource(struct vpu_core *core)
{
	struct platform_device *pdev = to_platform_device(core->dev);
	struct resource *res;
	struct devfreq *df;
	int rc = 0;

	if (!pdev) {
		pr_err("No platform device");
		return -1;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vpu");
	if (IS_ERR_OR_NULL(res)) {
		rc = PTR_ERR(res);
		pr_err("Failed to find vpu register base: %d\n", rc);
		goto err;
	}
	core->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(core->base)) {
		rc = PTR_ERR(core->base);
		if (rc == 0)
			rc = -EIO;
		pr_err("Failed to map vpu register base: %d\n", rc);
		core->base = NULL;
		goto err;
	}
	core->regs_size = res->end - res->start + 1;
	core->paddr = (phys_addr_t)res->start;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "qchctl_vpu");
	if (IS_ERR_OR_NULL(res)) {
		rc = PTR_ERR(res);
		pr_err("Failed to find qchctl_vpu register base: %d\n", rc);
		goto err;
	}
	core->qch_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(core->qch_base)) {
		rc = PTR_ERR(core->qch_base);
		if (rc == 0)
			rc = -EIO;
		pr_err("Failed to map qchctl_vpu register base: %d\n", rc);
		core->qch_base = NULL;
		goto err;
	}
	core->irq = platform_get_irq(pdev, 0);
	if (core->irq < 0) {
		rc = core->irq;
		pr_err("platform_get_irq failed: %d\n", rc);
		goto err;
	}

	df = devfreq_get_devfreq_by_phandle(&pdev->dev, "devfreq", 0);
	if (IS_ERR(df)) {
		rc = PTR_ERR(df);
		pr_err("failed to get devfreq %d\n", rc);
		goto err;
	}

	rc = google_pm_qos_add_devfreq_request(df, &core->dev_freq.qos_req,
				     DEV_PM_QOS_MIN_FREQUENCY, PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE);
	if (rc) {
		pr_err("failed to add devfreq request: %d\n", rc);
		goto err;
	}
	core->dev_freq.df = df;

	/* resource got from google_devm_of_icc_get will be auto released when device is removed */
	core->icc_path.path_gmc = google_devm_of_icc_get(&pdev->dev, "sswrp-codec-3p");
	if (IS_ERR_OR_NULL(core->icc_path.path_gmc)) {
		pr_err("get icc_pach for gmc failed %ld\n", PTR_ERR(core->icc_path.path_gmc));
		rc = -ENODEV;
		goto err;
	}

	return rc;

err:
	vpu_of_dt_release(core);
	return rc;
}

int vpu_of_dt_parse(struct vpu_core *core)
{
	int rc = 0;

	rc = vpu_of_get_resource(core);
	if (rc)
		pr_err("failed to get respource: %d\n", rc);

	return rc;
}

void vpu_of_dt_release(struct vpu_core *core)
{
	if (core->dev_freq.df) {
		google_pm_qos_remove_devfreq_request(core->dev_freq.df,
						     &core->dev_freq.qos_req);
		core->dev_freq.df = NULL;
	}
}
