// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC.
 *
 * LPM-based power controller that provides power domain control by directly
 * talking to the LPM registers. Power domain control is usually handled by
 * the CPM (Central Power Manager), where the CPM either:
 * 1. Talks to the LPB (for handling SSWRP-level top domain), or
 * 2. Talks to the LPM (for handling subdomains).
 *
 * But:
 * 1. Depending on the environment, we might not have CPM (and thus no LPB)
 * 2. Some domains like GPU domains need genpd to directly talk to LPCM for
 *    latency reasons.
 * 3. CPM <-> LPM communication might not be modelled in the CPM FW yet.
 *
 * Due to the above reasons, this genpd provider provides power domain control
 * without having to go through the CPM.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

#include <dt-bindings/power/genpd_lga.h>

#include "sequences/sequence.h"

struct rdo_lpm_pm_domains {
	struct device *dev;
	struct power_domain *pds;
	u32 pd_count;
};

static inline struct power_domain *to_power_domain(struct generic_pm_domain *d)
{
	return container_of(d, struct power_domain, genpd);
}

static int set_always_on(struct power_domain *pd)
{
	bool active = pd->desc->default_on;

	if (!active) {
		dev_dbg(pd->dev, "%s: Powering on an always-on domain\n",
			pd->genpd.name);
		active = pd->desc->power_on(pd) == 0;
		if (!active) {
			dev_err(pd->dev, "%s: Failed to power on\n",
				pd->genpd.name);
			return -EINVAL;
		}
	}
	dev_dbg(pd->dev, "%s: Marked as always-on\n", pd->genpd.name);
	pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
	return 0;
}

static int register_power_domain(struct device_node *pwr_domain_np,
				 struct power_domain *pd)
{
	struct device *dev = pd->dev;
	bool active = pd->desc->default_on;
	int ret;

	if (!active && !pd->desc->power_on) {
		dev_err(pd->dev,
			"%s: Default off domain without a power_on callback.\n",
			pd->genpd.name);
		return -EINVAL;
	}
	/*
	 * If any of the power transition callbacks do not exist, we interpret
	 * it as an always-on power domain.
	 */
	if (!pd->desc->power_off || !pd->desc->power_on) {
		dev_dbg(dev, "%s: is an always on domain\n", pd->genpd.name);
		ret = set_always_on(pd);
		if (ret) {
			dev_err(dev, "%s: Failed to set as always-on\n",
				pd->genpd.name);
			return ret;
		}
		active = true;
	}
	ret = pm_genpd_init(&pd->genpd, /* governer */ NULL, !active);
	if (ret) {
		dev_err(dev, "Failed at pm_genpd_init\n");
		return ret;
	}
	ret = of_genpd_add_provider_simple(pwr_domain_np, &pd->genpd);
	if (ret) {
		dev_err(dev, "Failed to add a simple provider\n");
		goto remove_genpd;
	}
	dev_dbg(dev, "%s: Registered as powered %s\n", pd->genpd.name,
		active ? "on" : "off");
	return 0;
remove_genpd:
	pm_genpd_remove(&pd->genpd);
	return ret;
}

static void unregister_power_domain(struct device_node *pwr_domain_np,
				    struct power_domain *pd)
{
	struct device *dev = pd->dev;

	dev_dbg(dev, "Unregistering %s\n", pd->genpd.name);
	of_genpd_del_provider(pwr_domain_np);
	pm_genpd_remove(&pd->genpd);
}

static void unregister_power_domains(struct platform_device *pdev, int count)
{
	struct rdo_lpm_pm_domains *lpm_pm_domains = platform_get_drvdata(pdev);
	struct device_node *pwr_ctrl_np = pdev->dev.of_node;
	struct device_node *pwr_domain_np;
	struct power_domain *pd;
	int i = 0;

	dev_dbg(lpm_pm_domains->dev, "Unregistering all power domains\n");
	for_each_available_child_of_node(pwr_ctrl_np, pwr_domain_np) {
		if (i >= count)
			break;
		pd = &lpm_pm_domains->pds[i];
		unregister_power_domain(pwr_domain_np, pd);
		++i;
	}
}

static int power_on(struct generic_pm_domain *domain)
{
	struct power_domain *pd = to_power_domain(domain);
	int ret;

	dev_dbg(pd->dev, "%s: power on sequence start\n", pd->genpd.name);
	ret = pd->desc->power_on(pd);
	dev_dbg(pd->dev, "%s: power on sequence %s\n", pd->genpd.name,
		ret == 0 ? "succeeded" : "failed");
	return ret;
}

static int power_off(struct generic_pm_domain *domain)
{
	struct power_domain *pd = to_power_domain(domain);
	int ret;

	dev_dbg(pd->dev, "%s: power off sequence start\n", pd->genpd.name);
	ret = pd->desc->power_off(pd);
	dev_dbg(pd->dev, "%s: power off sequence %s\n", pd->genpd.name,
		ret == 0 ? "succeeded" : "failed");
	return ret;
}

static int register_as_subdomain(struct device_node *pwr_domain_np,
				 struct power_domain *pd)
{
	struct device *dev = pd->dev;
	struct of_phandle_args parent_args;
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(pwr_domain_np, "power-domains", NULL,
					 0, &parent_args);
	if (ret) {
		dev_dbg(dev, "%s has no parent.\n", pwr_domain_np->name);
		return 0;
	}

	args.np = pwr_domain_np;
	args.args_count = 0;
	ret = of_genpd_add_subdomain(&parent_args, &args);
	of_node_put(parent_args.np);
	if (ret) {
		dev_err(dev,
			"Failed to setup %s as a subdomain of %s (ret = %d)\n",
			pwr_domain_np->name, parent_args.np->name, ret);
		return ret;
	}
	return 0;
}

static int goog_lpm_pm_domains_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rdo_lpm_pm_domains *lpm_pm_domains;
	const struct sswrp_power_desc *power_desc_table;
	struct device_node *pwr_ctrl_np = dev->of_node;
	struct device_node *pwr_domain_np;
	struct power_domain *pd;
	void __iomem **regions;
	int pd_count;
	int ret;
	int registered_domain_count;
	int i;

	power_desc_table = device_get_match_data(dev);

	lpm_pm_domains = devm_kzalloc(dev, sizeof(*lpm_pm_domains), GFP_KERNEL);
	if (!lpm_pm_domains)
		return -ENOMEM;

	regions = devm_kcalloc(dev, power_desc_table->region_count,
			       sizeof(*regions), GFP_KERNEL);
	for (i = 0; i < power_desc_table->region_count; ++i) {
		regions[i] = devm_platform_ioremap_resource_byname(
			pdev, power_desc_table->reg_names[i]);
		if (IS_ERR(regions[i])) {
			dev_err(dev, "Could not ioremap %s.\n",
				power_desc_table->reg_names[i]);
			return PTR_ERR(regions[i]);
		}
	}

	platform_set_drvdata(pdev, lpm_pm_domains);

	lpm_pm_domains->dev = dev;
	pd_count = of_get_child_count(pwr_ctrl_np);
	if (pd_count == 0) {
		dev_warn(dev, "No power domain defined.\n");
		return 0;
	}
	dev_dbg(dev, "Found %d power domains\n", pd_count);
	lpm_pm_domains->pd_count = pd_count;
	lpm_pm_domains->pds =
		devm_kcalloc(dev, pd_count, sizeof(*pd), GFP_KERNEL);
	if (!lpm_pm_domains->pds)
		return -ENOMEM;

	registered_domain_count = 0;
	for_each_available_child_of_node(pwr_ctrl_np, pwr_domain_np) {
		pd = &lpm_pm_domains->pds[registered_domain_count];

		ret = of_property_read_u32(pwr_domain_np, "subdomain-id",
					   &pd->subdomain_id);
		if (ret) {
			dev_err(dev, "Couldn't find 'subdomain-id'.\n");
			goto cleanup;
		}
		if (pd->subdomain_id >= power_desc_table->desc_count) {
			dev_err(dev, "Invalid subdomain id %d >= %d\n",
				pd->subdomain_id, power_desc_table->desc_count);
			ret = -EINVAL;
			goto cleanup;
		}

		pd->desc = &power_desc_table->descriptors[pd->subdomain_id];
		pd->dev = dev;
		pd->regions = regions;
		pd->data = power_desc_table->data;
		pd->genpd.name = pwr_domain_np->name;
		pd->genpd.power_on = power_on;
		pd->genpd.power_off = power_off;

		ret = register_power_domain(pwr_domain_np, pd);
		if (ret)
			goto cleanup;
		++registered_domain_count;
	}
	i = 0;
	for_each_available_child_of_node(pwr_ctrl_np, pwr_domain_np) {
		pd = &lpm_pm_domains->pds[i];
		ret = register_as_subdomain(pwr_domain_np, pd);
		if (ret)
			goto cleanup;
		++i;
	}
	return 0;
cleanup:
	of_node_put(pwr_domain_np);
	unregister_power_domains(pdev, registered_domain_count);
	return ret;
}

static int goog_lpm_pm_domains_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rdo_lpm_pm_domains *lpm_pm_domains = platform_get_drvdata(pdev);

	dev_dbg(dev, "Removing stub power controller\n");
	unregister_power_domains(pdev, lpm_pm_domains->pd_count);
	return 0;
}

static const struct of_device_id goog_lpm_pm_domains_of_match_table[] = {
	{ .compatible = "google,aon-lpm-pm-domains",
	  .data = &aon_power_desc_table },
	{},
};

MODULE_DEVICE_TABLE(of, goog_lpm_pm_domains_of_match_table);

static struct platform_driver goog_lpm_pm_domains_driver = {
	.probe = goog_lpm_pm_domains_probe,
	.remove = goog_lpm_pm_domains_remove,
	.driver = {
		.name = "lpm-pm-domains",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(goog_lpm_pm_domains_of_match_table),
	},
};

module_platform_driver(goog_lpm_pm_domains_driver);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Power controller driver that directly talks to the LPM.");
MODULE_LICENSE("GPL");
