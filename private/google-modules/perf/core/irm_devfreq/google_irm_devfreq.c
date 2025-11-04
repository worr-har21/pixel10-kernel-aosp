// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC
 */

#include <linux/devfreq.h>
#include <linux/mailbox_client.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/units.h>

#include <dt-bindings/lpcm/pf_state.h>
#include <dvfs-helper/google_dvfs_helper.h>
#include <interconnect/google_irm_api.h>
#include <perf/core/google_pm_qos.h>
#include <perf/core/google_vote_manager.h>
#include <trace/events/power.h>

#define DVFS_TRIG_EN BIT(0)
#define IRM_GMC_BITFIELD GENMASK(3, 0)
#define IRM_MEMSS_BITFIELD GENMASK(7, 4)
#define IRM_FABRIC_1_BITFIELD GENMASK(11, 8)
#define IRM_FABRIC_2_BITFIELD GENMASK(14, 12)
#define IRM_FREQ_VOTE_VALID BIT(15)
#define IRM_FREQ_VOTE_GMC_MEMSS(gmc_pf_level, memss_pf_level)		\
		(FIELD_PREP(IRM_FREQ_VOTE_VALID, 1)			\
		| FIELD_PREP(IRM_FABRIC_2_BITFIELD, 0)			\
		| FIELD_PREP(IRM_FABRIC_1_BITFIELD, 0)			\
		| FIELD_PREP(IRM_GMC_BITFIELD, gmc_pf_level)		\
		| FIELD_PREP(IRM_MEMSS_BITFIELD, memss_pf_level))

#define LOWEST_FREQ_OP_INDEX 31

struct irm_devfreq_data {
	struct device *dev;
	struct devfreq *devfreq;
	struct devfreq_dev_profile devfreq_profile;
	u32 *gmc_opp_to_memss_opp;
	u32 client_idx;
	u32 min_clamp_reg_offset;
	u32 trigger_reg_offset;
	u64 cur_freq_Hz;
	struct dvfs_domain_info *memss_domain_info;
	struct dvfs_domain_info *gmc_domain_info;
};

/* vote for memss and gmc freq through CPU IRM register ltv_gmc */
static int irm_vote_freq(struct irm_devfreq_data *data,
				u32 gmc_pf_level, u32 memss_pf_level)
{
	u32 val = 0;
	int ret = 0;

	val = IRM_FREQ_VOTE_GMC_MEMSS(gmc_pf_level, memss_pf_level);

	ret = irm_register_write(data->dev, data->client_idx,
			data->min_clamp_reg_offset, val);
	if (ret) {
		dev_err(data->dev, "Failed to write ltv_gmc register\n");
		return ret;
	}

	// write DVFS_TRIG_EN = 1, trigger re-evaluate dvfs in mipm
	ret = irm_register_write(data->dev, data->client_idx,
					data->trigger_reg_offset, DVFS_TRIG_EN);
	if (ret) {
		dev_err(data->dev, "Failed to write trig_en register\n");
		return ret;
	}
	return ret;
}

static int irm_devfreq_target(struct device *parent,
				  unsigned long *target_freq, u32 flags)
{
	struct platform_device *pdev = container_of(parent, struct platform_device, dev);
	struct irm_devfreq_data *data = platform_get_drvdata(pdev);
	u32 gmc_pf_level, memss_pf_level;
	int map_len;
	int ret = 0;

	if (!data || !data->gmc_opp_to_memss_opp || !data->gmc_domain_info)
		return -EINVAL;


	map_len = dev_pm_opp_get_opp_count(data->dev);
	if (map_len <= 0)
		return -EINVAL;

	gmc_pf_level = dvfs_helper_freq_to_lvl_floor(data->gmc_domain_info, *target_freq);
	if (gmc_pf_level < 0 || gmc_pf_level >= map_len) {
		dev_err(data->dev, "Could not find level for gmc freq %lu", *target_freq);
		return (gmc_pf_level < 0) ? gmc_pf_level : -EINVAL;
	}

	memss_pf_level = data->gmc_opp_to_memss_opp[gmc_pf_level];

	ret = irm_vote_freq(data, gmc_pf_level, memss_pf_level);
	if (ret) {
		dev_err(data->dev, "Failed to vote gmc and memss freq\n");
		goto out;
	}
	data->cur_freq_Hz = dvfs_helper_lvl_to_freq_exact(data->gmc_domain_info,
			gmc_pf_level);
	*target_freq = data->cur_freq_Hz;

out:
	return ret;
}

static int irm_devfreq_get_cur_freq(struct device *parent,
				       unsigned long *freq)
{
	struct platform_device *pdev = container_of(parent, struct platform_device, dev);
	struct irm_devfreq_data *data = platform_get_drvdata(pdev);

	*freq = data->cur_freq_Hz;

	return 0;
}

static int parse_dtsi_info(struct irm_devfreq_data *data)
{
	int err;
	struct dvfs_domain_info *domain_info;
	struct device_node *domain_node;
	struct device *dev = data->dev;

	domain_node = of_get_child_by_name(dev->of_node, "gmc_dvfs");
	if (!domain_node)
		return -ENODEV;

	err = dvfs_helper_add_opps_to_device(dev, domain_node);
	if (err)
		return err;

	err = dvfs_helper_get_domain_info(dev, domain_node, &domain_info);
	if (err)
		return err;

	data->gmc_domain_info = domain_info;

	domain_node = of_get_child_by_name(dev->of_node, "memss_dvfs");
	if (!domain_node)
		return -ENODEV;

	err = dvfs_helper_get_domain_info(dev, domain_node, &domain_info);
	if (err)
		return err;

	data->memss_domain_info = domain_info;

	err = of_property_read_u32(dev->of_node, "irm-client-idx",
				   &data->client_idx);
	if (err) {
		dev_err(dev, "%pOF is missing irm-client-idx property\n",
			dev->of_node);
		return err;
	}

	err = of_property_read_u32(dev->of_node, "irm-min-clamp-reg",
				   &data->min_clamp_reg_offset);
	if (err) {
		dev_err(dev, "%pOF is missing irm-min-clamp-reg property\n",
			dev->of_node);
		return err;
	}

	err = of_property_read_u32(dev->of_node, "irm-trigger-reg",
				   &data->trigger_reg_offset);
	if (err) {
		dev_err(dev, "%pOF is missing irm-trigger-reg property\n",
			dev->of_node);
		return err;
	}
	return 0;
}

#define NUM_COLS	2
static int init_pf_level_map(struct irm_devfreq_data *data)
{
	int map_len, temp_len, nf, i, j;
	u32 gmc_pf_level, memss_pf_level, freq;
	u32 *gmc_opp_to_memss_opp;
	int ret = 0;

	if (!data->gmc_domain_info)
		return -EINVAL;

	map_len = dev_pm_opp_get_opp_count(data->dev);
	if (map_len <= 0)
		return -EINVAL;

	gmc_opp_to_memss_opp = devm_kzalloc(data->dev, map_len * sizeof(u32), GFP_KERNEL);
	if (!gmc_opp_to_memss_opp)
		return -ENOMEM;

	memset(gmc_opp_to_memss_opp, LOWEST_FREQ_OP_INDEX, map_len * sizeof(u32));

	if (!of_find_property(data->dev->of_node, "gmc-memss-table", &temp_len))
		return -EINVAL;

	temp_len /= sizeof(freq);

	if (temp_len % NUM_COLS || temp_len == 0)
		return -EINVAL;
	nf = temp_len / NUM_COLS;

	for (i = 0, j = 0; i < nf; i++, j += NUM_COLS) {
		ret = of_property_read_u32_index(data->dev->of_node, "gmc-memss-table", j,
				&freq);
		if (ret)
			return -EINVAL;

		gmc_pf_level = dvfs_helper_freq_to_lvl_floor(data->gmc_domain_info, freq);
		if (gmc_pf_level < 0 || gmc_pf_level >= map_len) {
			dev_err(data->dev, "Could not find level for gmc freq %u", freq);
			return (gmc_pf_level < 0) ? gmc_pf_level : -EINVAL;
		}

		ret = of_property_read_u32_index(data->dev->of_node, "gmc-memss-table", j + 1,
				&freq);
		if (ret)
			return -EINVAL;

		memss_pf_level = dvfs_helper_freq_to_lvl_floor(data->memss_domain_info, freq);
		if (memss_pf_level < 0) {
			dev_err(data->dev, "Could not find level for memss freq %u", freq);
			return memss_pf_level;
		}
		gmc_opp_to_memss_opp[gmc_pf_level] = memss_pf_level;
	}

	for (int i = map_len - 2; i >= 0; i--) {
		if (gmc_opp_to_memss_opp[i] > gmc_opp_to_memss_opp[i + 1])
			gmc_opp_to_memss_opp[i] = gmc_opp_to_memss_opp[i + 1];
	}

	data->gmc_opp_to_memss_opp = gmc_opp_to_memss_opp;

	return ret;
}

static int init_devfreq_data(struct irm_devfreq_data *data)
{
	int ret = 0;

	ret = parse_dtsi_info(data);
	if (ret)
		goto out;

	ret = init_pf_level_map(data);
	if (ret)
		goto out;

	data->devfreq = devm_devfreq_add_device(data->dev,
		&data->devfreq_profile, DEVFREQ_GOV_POWERSAVE, NULL);

	if (IS_ERR(data->devfreq)) {
		dev_err(data->dev, "failed devfreq device added\n");
		ret = PTR_ERR(data->devfreq);
	}
out:
	return ret;
}

static int irm_devfreq_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct irm_devfreq_data *data;

	if (!irm_probing_completed())
		return -EPROBE_DEFER;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto err_data;
	}

	data->dev = dev;
	data->devfreq_profile.target = irm_devfreq_target;
	data->devfreq_profile.get_cur_freq = irm_devfreq_get_cur_freq;

	platform_set_drvdata(pdev, data);

	ret = init_devfreq_data(data);
	if (ret)
		goto err_devfreq;

	ret = google_register_devfreq(data->devfreq);
	if (ret)
		goto err_freq_tracker;

	ret = vote_manager_init_devfreq(data->devfreq);
	if (ret)
		goto err_vote_manager;

	dev_info(dev, "devfreq is initialized!!\n");

	return 0;

err_vote_manager:
	google_unregister_devfreq(data->devfreq);
err_freq_tracker:
	devm_devfreq_remove_device(data->dev, data->devfreq);
err_devfreq:
	platform_set_drvdata(pdev, NULL);
err_data:
	return ret;
}

static int irm_devfreq_remove(struct platform_device *pdev)
{
	struct irm_devfreq_data *data = platform_get_drvdata(pdev);

	if (!IS_ERR_OR_NULL(data->devfreq)) {
		vote_manager_remove_devfreq(data->devfreq);
		google_unregister_devfreq(data->devfreq);
		devm_devfreq_remove_device(data->dev, data->devfreq);
	}
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id irm_devfreq_of_match_table[] = {
	{
		.compatible = "google,irm-devfreq",
	},
	{},
};
MODULE_DEVICE_TABLE(of, irm_devfreq_of_match_table);

struct platform_driver irm_devfreq_driver = {
	.driver = {
		.name = "devfreq-irm",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(irm_devfreq_of_match_table),
	},
	.probe  = irm_devfreq_probe,
	.remove = irm_devfreq_remove,
};

module_platform_driver(irm_devfreq_driver);

MODULE_AUTHOR("Ziyi Cui <ziyic@google.com>");
MODULE_DESCRIPTION("Google irm devfreq driver");
MODULE_LICENSE("GPL");
