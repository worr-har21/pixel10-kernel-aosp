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

#define SUB_OFFSRC1 S2MPG15_PM_OFFSRC1
#define SUB_OFFSRC2 S2MPG15_PM_OFFSRC2

int core_pmic_sub_write_register(struct bcl_device *bcl_dev, u16 reg, u8 value, bool is_meter)
{
	if (is_meter)
		return s2mpg14_write_reg(bcl_dev->sub_meter_i2c, reg, value);
	return s2mpg15_write_reg(bcl_dev->sub_pmic_i2c, reg, value);
}

int core_pmic_sub_read_register(struct bcl_device *bcl_dev, u16 reg, u8 *value, bool is_meter)
{
	if (is_meter)
		return s2mpg14_read_reg(bcl_dev->sub_meter_i2c, reg, value);
	return s2mpg15_read_reg(bcl_dev->sub_pmic_i2c, reg, value);
}

void core_pmic_sub_meter_read_lpf_data(struct bcl_device *bcl_dev, struct brownout_stats *br_stats)
{
	struct odpm_info *info = bcl_dev->sub_odpm;
	u32 *val = br_stats->main_odpm_lpf.value;
	/* select lpf power mode */
	s2mpg1415_meter_set_lpf_mode(info->chip.hw_id, info->i2c, S2MPG1415_METER_POWER);
	/* the acquisition time of lpf_data is around 1ms */
	s2mpg1415_meter_read_lpf_data_reg(info->chip.hw_id, info->i2c, val);
	ktime_get_real_ts64((struct timespec64 *)&br_stats->main_odpm_lpf.time);
}

int core_pmic_sub_get_ocp_lvl(struct bcl_device *bcl_dev, u64 *val, u8 addr, u8 pmic, u8 mask,
			       u16 limit, u16 step)
{
	u8 value = 0;
	unsigned int ocp_warn_lvl;

	if (pmic_read(pmic, bcl_dev, addr, &value)) {
		dev_err(bcl_dev->device, "S2MPG1415 read %#x failed.", addr);
		return -EBUSY;
	}
	value &= mask;
	ocp_warn_lvl = limit - value * step;
	*val = ocp_warn_lvl;
	return 0;
}

int core_pmic_sub_set_ocp_lvl(struct bcl_device *bcl_dev, u64 val, u8 addr,
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
	if (pmic_read(CORE_PMIC_SUB, bcl_dev, addr, &value)) {
		dev_err(bcl_dev->device, "S2MPG1415 read %#x failed.", addr);
		return -EBUSY;
	}
	disable_irq(bcl_dev->zone[id]->bcl_irq);
	value &= ~(OCP_WARN_MASK) << OCP_WARN_LVL_SHIFT;
	value |= ((ulimit - val) / step) << OCP_WARN_LVL_SHIFT;
	ret = pmic_write(CORE_PMIC_SUB, bcl_dev, addr, value);
	if (!ret)
		bcl_dev->zone[id]->bcl_lvl = val - THERMAL_HYST_LEVEL;
	enable_irq(bcl_dev->zone[id]->bcl_irq);

	return ret;
}


void sub_pwrwarn_irq_work(struct work_struct *work)
{
	struct bcl_device *bcl_dev = container_of(work, struct bcl_device,
						  sub_pwr_irq_work.work);
	bool revisit_needed = false;
	int i;
	u32 micro_unit[ODPM_CHANNEL_MAX];
	u32 measurement;

	mutex_lock(&bcl_dev->sub_odpm->lock);

	odpm_get_raw_lpf_values(bcl_dev->sub_odpm, S2MPG1415_METER_CURRENT, micro_unit);
	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		measurement = micro_unit[i] >> LPF_CURRENT_SHIFT;
		bcl_dev->sub_pwr_warn_triggered[i] = (measurement > bcl_dev->sub_setting[i]);
		if (!revisit_needed)
			revisit_needed = bcl_dev->sub_pwr_warn_triggered[i];
		if (!bcl_dev->sub_pwr_warn_triggered[i])
			pwrwarn_update_end_time(bcl_dev, i, bcl_dev->pwrwarn_sub_irq_bins,
						MMWAVE_BCL_BIN);
		else
			pwrwarn_update_start_time(bcl_dev, i, bcl_dev->pwrwarn_sub_irq_bins,
							bcl_dev->sub_pwr_warn_triggered,
							MMWAVE_BCL_BIN);
	}

	mutex_unlock(&bcl_dev->sub_odpm->lock);

	if (revisit_needed)
		mod_delayed_work(system_unbound_wq, &bcl_dev->sub_pwr_irq_work,
				 msecs_to_jiffies(PWRWARN_DELAY_MS));
}

irqreturn_t sub_pwr_warn_irq_handler(int irq, void *data)
{
	struct bcl_device *bcl_dev = data;
	int i;

	/* Ensure sw mitigation enabled is read correctly */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled))
		return IRQ_HANDLED;

	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		if (bcl_dev->sub_pwr_warn_irq[i] == irq) {
			bcl_dev->sub_pwr_warn_triggered[i] = 1;
			/* Setup Timer to clear the triggered */
			mod_delayed_work(system_unbound_wq, &bcl_dev->sub_pwr_irq_work,
					 msecs_to_jiffies(PWRWARN_DELAY_MS));
			pwrwarn_update_start_time(bcl_dev, i, bcl_dev->pwrwarn_sub_irq_bins,
							bcl_dev->sub_pwr_warn_triggered,
							MMWAVE_BCL_BIN);
			break;
		}
	}

	return IRQ_HANDLED;
}

int core_pmic_sub_setup(struct bcl_device *bcl_dev)
{
	struct s2mpg15_platform_data *pdata;
	struct s2mpg15_dev *sub_dev = NULL;
	struct device_node *p_np;
	struct device_node *np = bcl_dev->device->of_node;
	struct i2c_client *i2c;
	int i;
	int read;
	int ret;
	int rail_i;
	u8 val;
	u32 flag;

	INIT_DELAYED_WORK(&bcl_dev->sub_pwr_irq_work, sub_pwrwarn_irq_work);
	p_np = of_parse_phandle(np, "google,sub-power", 0);
	if (p_np) {
		i2c = of_find_i2c_device_by_node(p_np);
		if (!i2c) {
			dev_err(bcl_dev->device, "Cannot find main-power I2C\n");
			return -ENODEV;
		}
		sub_dev = i2c_get_clientdata(i2c);
	}
	of_node_put(p_np);
	if (!sub_dev) {
		dev_err(bcl_dev->device, "Main PMIC device not found\n");
		return -ENODEV;
	}
	pdata = dev_get_platdata(sub_dev->dev);

	bcl_dev->sub_odpm = pdata->meter;
	if (!bcl_dev->sub_odpm) {
		dev_err(bcl_dev->device, "SUB PMIC meter device not found\n");
		return -ENODEV;
	}
	/* Ensure odpm_ready is correctly set */
	if (!smp_load_acquire(&bcl_dev->sub_odpm->ready)) {
		dev_err(bcl_dev->device, "SUB PMIC meter not initialized\n");
		return -ENODEV;
	}
	for (i = 0; i < METER_CHANNEL_MAX; i++) {
		rail_i = bcl_dev->sub_odpm->channels[i].rail_i;
		if (bcl_dev->sub_odpm->chip.rails == NULL) {
			dev_err(bcl_dev->device, "SUB PMIC Rail:%d not initialized\n", rail_i);
			return -ENODEV;
		}
		bcl_dev->sub_rail_names[i] = bcl_dev->sub_odpm->chip.rails[rail_i].schematic_name;
	}
	bcl_dev->sub_irq_base = pdata->irq_base;
	bcl_dev->sub_pmic_i2c = sub_dev->pmic;
	bcl_dev->sub_meter_i2c = sub_dev->meter;
	bcl_dev->sub_dev = sub_dev->dev;
	if (pmic_read(CORE_PMIC_SUB, bcl_dev, SUB_CHIPID, &val)) {
		dev_err(bcl_dev->device, "Failed to read PMIC chipid.\n");
		return -ENODEV;
	}
	pmic_read(CORE_PMIC_SUB, bcl_dev, SUB_OFFSRC1, &val);
	dev_info(bcl_dev->device, "SUB OFFSRC1 : %#x\n", val);
	bcl_dev->sub_offsrc1 = val;
	pmic_write(CORE_PMIC_SUB, bcl_dev, SUB_OFFSRC1, 0);
	pmic_read(CORE_PMIC_SUB, bcl_dev, SUB_OFFSRC2, &val);
	dev_info(bcl_dev->device, "SUB OFFSRC2 : %#x\n", val);
	bcl_dev->sub_offsrc2 = val;
	pmic_write(CORE_PMIC_SUB, bcl_dev, SUB_OFFSRC2, 0);

	/* parse ODPM sub limit */
	p_np = of_get_child_by_name(np, "sub_limit");
	if (p_np) {
		i = 0;
		for_each_child_of_node_scoped(p_np, child) {
			of_property_read_u32(child, "setting", &read);
			if (i < METER_CHANNEL_MAX) {
				bcl_dev->sub_setting[i] = read;
				meter_write(CORE_PMIC_SUB, bcl_dev, i, read);
				bcl_dev->sub_limit[i] =
						settings_to_current(bcl_dev, CORE_PMIC_SUB, i,
								    read << LPF_CURRENT_SHIFT);
				i++;
			}
		}
		of_node_put(p_np);
	}

	flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	ret = google_bcl_register_zone(bcl_dev, PRE_OCP_GPU, "ocp_gpu",
				       pdata->b2_ocp_warn_pin,
				       gpio_to_irq(pdata->b2_ocp_warn_pin),
				       CORE_SUB_PMIC, IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: GPU\n");
		return -ENODEV;
	}

	for (i = 0; i < S2MPG1415_METER_CHANNEL_MAX; i++) {
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

	return 0;
}
