// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */

#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/mfd/samsung/s2mpg1415.h>
#include <linux/mfd/samsung/s2mpg1415-register.h>
#include <soc/google/odpm.h>

#include "core_pmic_defs.h"

#define MAIN_OFFSRC1 S2MPG14_PM_OFFSRC1
#define MAIN_OFFSRC2 S2MPG14_PM_OFFSRC2
#define MAIN_PWRONSRC S2MPG14_PM_PWRONSRC

int core_pmic_main_write_register(struct bcl_device *bcl_dev, u16 reg, u8 value, bool is_meter)
{
	if (is_meter)
		return s2mpg14_write_reg(bcl_dev->main_meter_i2c, (u8)reg, value);
	return s2mpg14_write_reg(bcl_dev->main_pmic_i2c, reg, value);
}

int core_pmic_main_read_register(struct bcl_device *bcl_dev, u16 reg, u8 *value, bool is_meter)
{
	if (is_meter)
		return s2mpg14_read_reg(bcl_dev->main_meter_i2c, (u8)reg, value);
	return s2mpg14_read_reg(bcl_dev->main_pmic_i2c, reg, value);
}

void core_pmic_main_meter_read_lpf_data(struct bcl_device *bcl_dev, struct brownout_stats *br_stats)
{
	struct odpm_info *info = bcl_dev->main_odpm;
	u32 *val = br_stats->main_odpm_lpf.value;
	/* select lpf power mode */
	s2mpg1415_meter_set_lpf_mode(info->chip.hw_id, info->i2c, S2MPG1415_METER_POWER);
	/* the acquisition time of lpf_data is around 1ms */
	s2mpg1415_meter_read_lpf_data_reg(info->chip.hw_id, info->i2c, val);
	ktime_get_real_ts64((struct timespec64 *)&br_stats->main_odpm_lpf.time);
}

int core_pmic_main_get_ocp_lvl(struct bcl_device *bcl_dev, u64 *val, u8 addr, u16 limit, u16 step)
{
	u8 value = 0;
	unsigned int ocp_warn_lvl;

	if (pmic_read(CORE_PMIC_MAIN, bcl_dev, addr, &value)) {
		dev_err(bcl_dev->device, "S2MPG1415 read %#x failed.", addr);
		return -EBUSY;
	}
	value &= OCP_WARN_MASK;
	ocp_warn_lvl = limit - value * step;
	*val = ocp_warn_lvl;
	return 0;
}

int core_pmic_main_set_ocp_lvl(struct bcl_device *bcl_dev, u64 val, u8 addr,
			       u16 llimit, u16 ulimit, u16 step, u8 id)
{
	u8 value;
	int ret;

	if (!bcl_dev->zone[id])
		return 0;
	if (val < llimit || val > ulimit) {
		dev_err(bcl_dev->device, "OCP_WARN LEVEL %llu outside of range %d - %d mA.", val,
		       llimit, ulimit);
		return -EBUSY;
	}
	if (pmic_read(CORE_PMIC_MAIN, bcl_dev, addr, &value)) {
		dev_err(bcl_dev->device, "S2MPG1415 read %#x failed.", addr);
		return -EBUSY;
	}
	disable_irq(bcl_dev->zone[id]->bcl_irq);
	value &= ~(OCP_WARN_MASK) << OCP_WARN_LVL_SHIFT;
	value |= ((ulimit - val) / step) << OCP_WARN_LVL_SHIFT;
	ret = pmic_write(CORE_PMIC_MAIN, bcl_dev, addr, value);
	if (!ret)
		bcl_dev->zone[id]->bcl_lvl = val - THERMAL_HYST_LEVEL;
	enable_irq(bcl_dev->zone[id]->bcl_irq);

	return ret;
}

int core_pmic_main_read_uvlo(struct bcl_device *bcl_dev, unsigned int *smpl_warn_lvl)
{
	u8 value = 0;

	if (!bcl_dev->main_pmic_i2c)
		return -EBUSY;
	pmic_read(CORE_PMIC_MAIN, bcl_dev, SMPL_WARN_CTRL, &value);
	value >>= SMPL_WARN_SHIFT;

	*smpl_warn_lvl = value * 100 + SMPL_LOWER_LIMIT;
	return 0;
}

u16 core_pmic_main_store_uvlo(struct bcl_device *bcl_dev, unsigned int val, size_t size)
{
	u8 value;
	int ret = 0;

	if (val < SMPL_LOWER_LIMIT || val > SMPL_UPPER_LIMIT) {
		dev_err(bcl_dev->device, "PRE_UVLO LEVEL %d outside of range %d - %d mV.", val,
			SMPL_LOWER_LIMIT, SMPL_UPPER_LIMIT);
		return -EINVAL;
	}
	if (!bcl_dev->main_pmic_i2c) {
		dev_err(bcl_dev->device, "MAIN I2C not found\n");
		return -EIO;
	}
	if (pmic_read(CORE_PMIC_MAIN, bcl_dev, SMPL_WARN_CTRL, &value)) {
		dev_err(bcl_dev->device, "S2MPG1415 read %#x failed.", SMPL_WARN_CTRL);
		return -EBUSY;
	}
	disable_irq(bcl_dev->zone[PRE_UVLO]->bcl_irq);
	value &= ~SMPL_WARN_MASK;
	value |= ((val - SMPL_LOWER_LIMIT) / 100) << SMPL_WARN_SHIFT;
	if (pmic_write(CORE_PMIC_MAIN, bcl_dev, SMPL_WARN_CTRL, value)) {
		dev_err(bcl_dev->device, "i2c write error setting smpl_warn\n");
		enable_irq(bcl_dev->zone[PRE_UVLO]->bcl_irq);
		return ret;
	}
	bcl_dev->zone[PRE_UVLO]->bcl_lvl = SMPL_BATTERY_VOLTAGE - val - THERMAL_HYST_LEVEL;

	enable_irq(bcl_dev->zone[PRE_UVLO]->bcl_irq);

	return size;
}

void core_pmic_parse_dtree(struct bcl_device *bcl_dev)
{
	int ret, len, i;
	struct device_node *child;
	struct device_node *p_np;
	int read;
	struct device_node *np = bcl_dev->device->of_node;

	/* parse ODPM main mitigation module */
	p_np = of_get_child_by_name(np, "main_mitigation");
	if (p_np) {
		i = 0;
		for_each_child_of_node(p_np, child) {
			if (i < METER_CHANNEL_MAX) {
				of_property_read_u32(child, "module_id", &read);
				bcl_dev->main_mitigation_conf[i].module_id = read;

				of_property_read_u32(child, "threshold", &read);
				bcl_dev->main_mitigation_conf[i].threshold = read;
				i++;
			}
		}
	}

	/* parse ODPM sub mitigation module */
	p_np = of_get_child_by_name(np, "sub_mitigation");
	if (p_np) {
		i = 0;
		for_each_child_of_node(p_np, child) {
			if (i < METER_CHANNEL_MAX) {
				of_property_read_u32(child, "module_id", &read);
				bcl_dev->sub_mitigation_conf[i].module_id = read;

				of_property_read_u32(child, "threshold", &read);
				bcl_dev->sub_mitigation_conf[i].threshold = read;
				i++;
			}
		}
	}

	/* parse and init non-monitored modules */
	bcl_dev->non_monitored_mitigation_module_ids = 0;
	len = 0;
	if (of_get_property(np, "non_monitored_module_ids", &len) && len >= sizeof(u32)) {
		bcl_dev->non_monitored_module_ids = kmalloc(len, GFP_KERNEL);
		len /= sizeof(u32);
		if (bcl_dev->non_monitored_module_ids) {
			for (i = 0; i < len; i++) {
				ret = of_property_read_u32_index(np,
						 "non_monitored_module_ids",
						 i, &bcl_dev->non_monitored_module_ids[i]);
				if (ret) {
					dev_err(bcl_dev->device,
						"failed to read non_monitored_module_id_%d\n", i);
				}
				bcl_dev->non_monitored_mitigation_module_ids |=
					BIT(bcl_dev->non_monitored_module_ids[i]);
			}
		}
	}
}

void main_pwrwarn_irq_work(struct work_struct *work)
{
	struct bcl_device *bcl_dev = container_of(work, struct bcl_device,
						  main_pwr_irq_work.work);
	bool revisit_needed = false;
	int i;
	u32 micro_unit[ODPM_CHANNEL_MAX];
	u32 measurement;

	mutex_lock(&bcl_dev->main_odpm->lock);

	odpm_get_raw_lpf_values(bcl_dev->main_odpm, S2MPG1415_METER_CURRENT, micro_unit);
	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		measurement = micro_unit[i] >> LPF_CURRENT_SHIFT;
		bcl_dev->main_pwr_warn_triggered[i] = (measurement > bcl_dev->main_setting[i]);
		if (!revisit_needed)
			revisit_needed = bcl_dev->main_pwr_warn_triggered[i];
		if (!bcl_dev->main_pwr_warn_triggered[i])
			pwrwarn_update_end_time(bcl_dev, i, bcl_dev->pwrwarn_main_irq_bins,
						RFFE_BCL_BIN);
		else
			pwrwarn_update_start_time(bcl_dev, i, bcl_dev->pwrwarn_main_irq_bins,
							bcl_dev->main_pwr_warn_triggered,
							RFFE_BCL_BIN);
	}

	mutex_unlock(&bcl_dev->main_odpm->lock);

	if (revisit_needed)
		mod_delayed_work(system_unbound_wq, &bcl_dev->main_pwr_irq_work,
				 msecs_to_jiffies(PWRWARN_DELAY_MS));
}


irqreturn_t main_pwr_warn_irq_handler(int irq, void *data)
{
	struct bcl_device *bcl_dev = data;
	int i;

	/* Ensure sw mitigation enabled is correctly read */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled))
		return IRQ_HANDLED;

	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		if (bcl_dev->main_pwr_warn_irq[i] == irq) {
			bcl_dev->main_pwr_warn_triggered[i] = 1;

			/* Setup Timer to clear the triggered */
			mod_delayed_work(system_unbound_wq, &bcl_dev->main_pwr_irq_work,
					 msecs_to_jiffies(PWRWARN_DELAY_MS));
			pwrwarn_update_start_time(bcl_dev, i, bcl_dev->pwrwarn_main_irq_bins,
							bcl_dev->main_pwr_warn_triggered,
							RFFE_BCL_BIN);
			break;
		}
	}

	return IRQ_HANDLED;
}

int core_pmic_main_setup(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	struct s2mpg14_platform_data *pdata;
	struct s2mpg14_dev *main_dev = NULL;
	struct device_node *p_np;
	struct device_node *np = bcl_dev->device->of_node;
	struct i2c_client *i2c;
	int i;
	int read;
	int ret;
	int rail_i;
	u8 val;
	unsigned int ocp_cpu1_pin;
	unsigned int ocp_cpu2_pin;
	unsigned int ocp_tpu_pin;
	u32 flag;

	INIT_DELAYED_WORK(&bcl_dev->main_pwr_irq_work, main_pwrwarn_irq_work);
	p_np = of_parse_phandle(np, "google,main-power", 0);
	if (p_np) {
		i2c = of_find_i2c_device_by_node(p_np);
		if (!i2c) {
			dev_err(bcl_dev->device, "Cannot find main-power I2C\n");
			return -ENODEV;
		}
		main_dev = i2c_get_clientdata(i2c);
	}
	of_node_put(p_np);
	if (!main_dev) {
		dev_err(bcl_dev->device, "Main PMIC device not found\n");
		return -ENODEV;
	}
	pdata = dev_get_platdata(main_dev->dev);

	bcl_dev->main_odpm = pdata->meter;
	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		rail_i = bcl_dev->main_odpm->channels[i].rail_i;
		if (bcl_dev->main_odpm->chip.rails == NULL) {
			dev_err(bcl_dev->device, "MAIN PMIC Rail:%d not initialized\n", rail_i);
			return -ENODEV;
		}
		bcl_dev->main_rail_names[i] = bcl_dev->main_odpm->chip.rails[rail_i].schematic_name;
	}
	bcl_dev->main_irq_base = pdata->irq_base;
	bcl_dev->main_pmic_i2c = main_dev->pmic;
	bcl_dev->main_meter_i2c = main_dev->meter;
	bcl_dev->main_dev = main_dev->dev;
	/* clear MAIN information every boot */
	/* see b/215371539 */
	pmic_read(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC1, &val);
	dev_info(bcl_dev->device, "MAIN OFFSRC1 : %#x\n", val);
	bcl_dev->main_offsrc1 = val;
	pmic_read(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC2, &val);
	dev_info(bcl_dev->device, "MAIN OFFSRC2 : %#x\n", val);
	bcl_dev->main_offsrc2 = val;
	pmic_read(CORE_PMIC_MAIN, bcl_dev, MAIN_PWRONSRC, &val);
	dev_info(bcl_dev->device, "MAIN PWRONSRC: %#x\n", val);
	bcl_dev->pwronsrc = val;
	pmic_write(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC1, 0);
	pmic_write(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC2, 0);
	pmic_write(CORE_PMIC_MAIN, bcl_dev, S2MPG14_PM_SMPL_WARN_CTRL, bcl_dev->smpl_ctrl);

	/* parse ODPM main limit */
	p_np = of_get_child_by_name(np, "main_limit");
	if (p_np) {
		i = 0;
		for_each_child_of_node_scoped(p_np, child) {
			of_property_read_u32(child, "setting", &read);
			if (i < METER_CHANNEL_MAX) {
				bcl_dev->main_setting[i] = read;
				meter_write(CORE_PMIC_MAIN, bcl_dev, i, read);
				bcl_dev->main_limit[i] =
						settings_to_current(bcl_dev, CORE_PMIC_MAIN, i,
								    read << LPF_CURRENT_SHIFT);
				i++;
			}
		}
		of_node_put(p_np);
	}

	ocp_cpu2_pin = pdata->b2_ocp_warn_pin;
	ocp_cpu1_pin = pdata->b3_ocp_warn_pin;
	ocp_tpu_pin = pdata->b7_ocp_warn_pin;

	flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	ret = google_bcl_register_zone(bcl_dev, PRE_UVLO, "smpl_warn",
				       pdata->smpl_warn_pin, gpio_to_irq(pdata->smpl_warn_pin),
				       CORE_MAIN_PMIC, IRQ_EXIST, POLARITY_LOW, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: PRE_UVLO\n");
		return -ENODEV;
	}

	ret = google_bcl_register_zone(bcl_dev, PRE_OCP_CPU1, "ocp_cpu1",
				       ocp_cpu1_pin, gpio_to_irq(ocp_cpu1_pin), CORE_MAIN_PMIC,
				       IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: CPUCL1\n");
		return -ENODEV;
	}

	ret = google_bcl_register_zone(bcl_dev, PRE_OCP_CPU2, "ocp_cpu2",
				       ocp_cpu2_pin, gpio_to_irq(ocp_cpu2_pin), CORE_MAIN_PMIC,
				       IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: CPUCL2\n");
		return -ENODEV;
	}

	ret = google_bcl_register_zone(bcl_dev, PRE_OCP_TPU, "ocp_tpu",
				       ocp_tpu_pin, gpio_to_irq(ocp_tpu_pin), CORE_MAIN_PMIC,
				       IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: TPU\n");
		return -ENODEV;
	}

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
	}

	return 0;
}
