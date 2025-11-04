// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */


#include <linux/mfd/samsung/s2mpg1415.h>
#include <linux/mfd/samsung/s2mpg1415-register.h>
#include <soc/google/odpm.h>
#include <linux/pinctrl/consumer.h>

#include "core_pmic_defs.h"

#define MAIN_METER_PWR_WARN0	S2MPG14_METER_PWR_WARN0
#define SUB_METER_PWR_WARN0	S2MPG15_METER_PWR_WARN0

int meter_write(int pmic, struct bcl_device *bcl_dev, int idx, u8 value)
{
	switch (pmic) {
	case CORE_PMIC_SUB:
		return core_pmic_sub_write_register(bcl_dev, SUB_METER_PWR_WARN0 + idx,
						    value, true);
	case CORE_PMIC_MAIN:
		return core_pmic_main_write_register(bcl_dev, MAIN_METER_PWR_WARN0 + idx,
						     value, true);
	}
	return 0;
}

int meter_read(int pmic, struct bcl_device *bcl_dev, int idx, u8 *value)
{
	switch (pmic) {
	case CORE_PMIC_SUB:
		return core_pmic_sub_read_register(bcl_dev, SUB_METER_PWR_WARN0 + idx,
						   value, true);
	case CORE_PMIC_MAIN:
		return core_pmic_main_read_register(bcl_dev, MAIN_METER_PWR_WARN0 + idx,
						    value, true);
	}
	return 0;
}

int google_bcl_configure_modem(struct bcl_device *bcl_dev)
{
	struct pinctrl *modem_pinctrl;
	struct pinctrl_state *batoilo_pinctrl_state, *rffe_pinctrl_state;
	int ret;

	modem_pinctrl = devm_pinctrl_get(bcl_dev->device);
	if (IS_ERR_OR_NULL(modem_pinctrl)) {
		dev_err(bcl_dev->device, "Cannot find modem_pinctrl!\n");
		return -EINVAL;
	}
	batoilo_pinctrl_state = pinctrl_lookup_state(modem_pinctrl, "bcl-batoilo-modem");
	if (IS_ERR_OR_NULL(batoilo_pinctrl_state)) {
		dev_err(bcl_dev->device, "batoilo: pinctrl lookup state failed!\n");
		return -EINVAL;
	}
	rffe_pinctrl_state = pinctrl_lookup_state(modem_pinctrl, "bcl-rffe-modem");
	if (IS_ERR_OR_NULL(rffe_pinctrl_state)) {
		dev_err(bcl_dev->device, "rffe: pinctrl lookup state failed!\n");
		return -EINVAL;
	}
	ret = pinctrl_select_state(modem_pinctrl, batoilo_pinctrl_state);
	if (ret < 0) {
		dev_err(bcl_dev->device, "batoilo: pinctrl select state failed!\n");
		return -EINVAL;
	}
	ret = pinctrl_select_state(modem_pinctrl, rffe_pinctrl_state);
	if (ret < 0) {
		dev_err(bcl_dev->device, "rffe: pinctrl select state failed!\n");
		return -EINVAL;
	}
	bcl_dev->config_modem = true;
	return 0;
}

u64 settings_to_current(struct bcl_device *bcl_dev, int pmic, int idx, u32 setting)
{
	int rail_i;
	enum s2mpg1415_meter_muxsel muxsel;
	struct odpm_info *info;
	u64 raw_unit;
	u32 resolution;

	setting = setting << LPF_CURRENT_SHIFT;

	if (pmic == CORE_PMIC_MAIN)
		info = bcl_dev->main_odpm;
	else
		info = bcl_dev->sub_odpm;

	if (!info)
		return 0;

	rail_i = info->channels[idx].rail_i;
	muxsel = info->chip.rails[rail_i].mux_select;
	if (pmic == CORE_PMIC_MAIN) {
		if (strstr(bcl_dev->main_rail_names[idx], "VSYS") != NULL)
			resolution = (u32) VSHUNT_MULTIPLIER * ((u64)EXTERNAL_RESOLUTION_VSHUNT) /
					info->chip.rails[rail_i].shunt_uohms;
		else
			resolution = s2mpg14_muxsel_to_current_resolution(muxsel);
	} else {
		if (strstr(bcl_dev->sub_rail_names[idx], "VSYS") != NULL)
			resolution = (u32) VSHUNT_MULTIPLIER * ((u64)EXTERNAL_RESOLUTION_VSHUNT) /
					info->chip.rails[rail_i].shunt_uohms;
		else
			resolution = s2mpg15_muxsel_to_current_resolution(muxsel);
	}
	raw_unit = (u64)setting * resolution;
	raw_unit = raw_unit * MILLI_TO_MICRO;
	return (u32)_IQ30_to_int(raw_unit);
}

void compute_mitigation_modules(struct bcl_device *bcl_dev,
				struct bcl_mitigation_conf *mitigation_conf, u32 *odpm_lpf_value)
{
	int i;

	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		if (odpm_lpf_value[i] >= mitigation_conf[i].threshold) {
			atomic_or(BIT(mitigation_conf[i].module_id),
					  &bcl_dev->mitigation_module_ids);
		}
	}
}

void s2mpg1x_setup_irq_handling(struct bcl_device *bcl_dev)
{
	int i;
	int ret;

	for (i = 0; i < S2MPG1415_METER_CHANNEL_MAX; i++) {
		bcl_dev->main_pwr_warn_irq[i] = bcl_dev->main_irq_base
				+ S2MPG14_IRQ_PWR_WARN_CH0_INT6 + i;
		ret = devm_request_threaded_irq(bcl_dev->device, bcl_dev->main_pwr_warn_irq[i],
						NULL, main_pwr_warn_irq_handler, 0,
						bcl_dev->main_rail_names[i], bcl_dev);
		if (ret < 0) {
			dev_err(bcl_dev->device, "Failed to request PWR_WARN_CH%d IRQ: %d: %d\n",
				i, bcl_dev->main_pwr_warn_irq[i], ret);
		}
		bcl_dev->sub_pwr_warn_irq[i] =
				bcl_dev->sub_irq_base + S2MPG15_IRQ_PWR_WARN_CH0_INT5 + i;
		ret = devm_request_threaded_irq(bcl_dev->device, bcl_dev->sub_pwr_warn_irq[i],
						NULL, sub_pwr_warn_irq_handler, 0,
						bcl_dev->sub_rail_names[i], bcl_dev);
		if (ret < 0) {
			dev_err(bcl_dev->device, "Failed to request PWR_WARN_CH%d IRQ: %d: %d\n",
				i, bcl_dev->sub_pwr_warn_irq[i], ret);
		}
	}
}

