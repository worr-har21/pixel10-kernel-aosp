// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 *
 */

#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/mfd/samsung/s2mpg10.h>
#include <linux/mfd/samsung/s2mpg10-meter.h>
#include <linux/mfd/samsung/s2mpg10-register.h>
#include <linux/mfd/samsung/s2mpg11.h>
#include <linux/mfd/samsung/s2mpg11-meter.h>
#include <linux/mfd/samsung/s2mpg11-register.h>

#include "core_pmic_defs.h"

#define MAIN_OFFSRC1 S2MPG10_PM_OFFSRC
#define MAIN_OFFSRC2 S2MPG10_PM_OFFSRC
#define MAIN_PWRONSRC S2MPG10_PM_PWRONSRC

int core_pmic_main_write_register(struct bcl_device *bcl_dev, u16 reg, u8 value, bool is_meter)
{
	if (is_meter)
		return s2mpg10_write_reg(bcl_dev->main_meter_i2c, (u8)reg, value);
	return s2mpg10_write_reg(bcl_dev->main_pmic_i2c, (u8)reg, value);
}

int core_pmic_main_read_register(struct bcl_device *bcl_dev, u16 reg, u8 *value, bool is_meter)
{
	if (is_meter)
		return s2mpg10_read_reg(bcl_dev->main_meter_i2c, (u8)reg, value);
	return s2mpg10_read_reg(bcl_dev->main_pmic_i2c, (u8)reg, value);
}

void core_pmic_main_meter_read_lpf_data(struct bcl_device *bcl_dev, struct brownout_stats *br_stats)
{
}

int core_pmic_main_get_ocp_lvl(struct bcl_device *bcl_dev, u64 *val, u8 addr, u16 limit, u16 step)
{
	u8 value = 0;
	unsigned int ocp_warn_lvl;

	if (pmic_read(CORE_PMIC_MAIN, bcl_dev, addr, &value)) {
		dev_err(bcl_dev->device, "S2MPG1011 read %#x failed.", addr);
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
		dev_err(bcl_dev->device, "S2MPG1011 read %#x failed.", addr);
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
		dev_err(bcl_dev->device, "SMPL_WARN LEVEL %d outside of range %d - %d mV.", val,
			SMPL_LOWER_LIMIT, SMPL_UPPER_LIMIT);
		return -EINVAL;
	}
	if (!bcl_dev->main_pmic_i2c) {
		dev_err(bcl_dev->device, "MAIN I2C not found\n");
		return -EIO;
	}
	if (pmic_read(CORE_PMIC_MAIN, bcl_dev, SMPL_WARN_CTRL, &value)) {
		dev_err(bcl_dev->device, "S2MPG1011 read %#x failed.", SMPL_WARN_CTRL);
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

void core_pmic_parse_dtree(struct bcl_device *bcl_dev) {}

int core_pmic_main_setup(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	struct s2mpg10_platform_data *pdata;
	struct s2mpg10_dev *main_dev = NULL;
	struct device_node *p_np;
	struct device_node *np = bcl_dev->device->of_node;
	struct i2c_client *i2c;
	int ret;
	u8 val;
	unsigned int ocp_cpu1_pin;
	unsigned int ocp_cpu2_pin;
	unsigned int ocp_tpu_pin;

	unsigned int soft_ocp_cpu2_pin;
	unsigned int soft_ocp_cpu1_pin;
	unsigned int soft_ocp_tpu_pin;
	u32 flag;

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

	bcl_dev->main_pmic_i2c = main_dev->pmic;
	bcl_dev->main_meter_i2c = main_dev->meter;
	bcl_dev->main_dev = main_dev->dev;
	/* clear MAIN information every boot */
	/* see b/213371339 */
	pmic_read(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC1, &val);
	dev_info(bcl_dev->device, "MAIN OFFSRC1 : %#x\n", val);
	bcl_dev->main_offsrc1 = val;
	pmic_read(CORE_PMIC_MAIN, bcl_dev, MAIN_PWRONSRC, &val);
	dev_info(bcl_dev->device, "MAIN PWRONSRC: %#x\n", val);
	bcl_dev->pwronsrc = val;
	pmic_write(CORE_PMIC_MAIN, bcl_dev, MAIN_OFFSRC1, 0);
	pmic_write(CORE_PMIC_MAIN, bcl_dev, MAIN_PWRONSRC, 0);

	ocp_cpu2_pin = pdata->b2_ocp_warn_pin;
	ocp_cpu1_pin = pdata->b3_ocp_warn_pin;
	ocp_tpu_pin = pdata->b10_ocp_warn_pin;

	soft_ocp_cpu2_pin = pdata->b2_soft_ocp_warn_pin;
	soft_ocp_cpu1_pin = pdata->b3_soft_ocp_warn_pin;
	soft_ocp_tpu_pin = pdata->b10_soft_ocp_warn_pin;

	flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	ret = google_bcl_register_zone(bcl_dev, PRE_UVLO, "smpl_warn",
				       pdata->smpl_warn_pin, gpio_to_irq(pdata->smpl_warn_pin),
				       CORE_MAIN_PMIC, IRQ_EXIST, POLARITY_LOW, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: SMPL_WARN\n");
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

	ret = google_bcl_register_zone(bcl_dev, SOFT_PRE_OCP_CPU1, "soft_ocp_cpu1",
				       soft_ocp_cpu1_pin, gpio_to_irq(soft_ocp_cpu1_pin),
				       CORE_MAIN_PMIC, IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: SOFT_CPUCL1\n");
		return -ENODEV;
	}

	ret = google_bcl_register_zone(bcl_dev, SOFT_PRE_OCP_CPU2, "soft_ocp_cpu2",
				       soft_ocp_cpu2_pin, gpio_to_irq(soft_ocp_cpu2_pin),
				       CORE_MAIN_PMIC, IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: SOFT_CPUCL2\n");
		return -ENODEV;
	}

	ret = google_bcl_register_zone(bcl_dev, SOFT_PRE_OCP_TPU, "soft_ocp_tpu",
				       soft_ocp_tpu_pin, gpio_to_irq(soft_ocp_tpu_pin),
				       CORE_MAIN_PMIC, IRQ_EXIST, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: SOFT_TPU\n");
		return -ENODEV;
	}

	return 0;
}
