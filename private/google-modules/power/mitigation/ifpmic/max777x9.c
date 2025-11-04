// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */

#include "ifpmic_defs.h"
#include "ifpmic_class.h"
#include "max77759/max77759_irq.h"
#include "max77779/max77779_irq.h"
#include <max77759_regs.h>
#include <max77779_regs.h>
#include <max77779.h>
#include <max777x9_bcl.h>

int ifpmic_setup_dev(struct bcl_device *bcl_dev)
{
	int ret = 0;
	u32 retval;
	u8 regval;
	struct device_node *np = bcl_dev->device->of_node;

	ret = of_property_read_u32(np, "google,ifpmic", &retval);
	bcl_dev->ifpmic = (retval == M77759) ? MAX77759 : MAX77779;

	bcl_dev->intf_pmic_dev = max77779_get_dev(bcl_dev->device, "google,charger");
	if (!bcl_dev->intf_pmic_dev) {
		dev_err(bcl_dev->device, "Cannot find Charger I2C\n");
		return -ENODEV;
	}
	if (bcl_dev->ifpmic == MAX77779) {
		if (max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev,
						   MAX77779_BAT_OILO1_CNFG_0, &regval) != 0)
			return -EPROBE_DEFER;
	} else {
		if (max77759_external_reg_read(bcl_dev->intf_pmic_dev,
					       MAX77759_CHG_CNFG_14, &regval) != 0)
			return -EPROBE_DEFER;
	}
	return 0;
}

int ifpmic_setup(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	if (bcl_dev->ifpmic == MAX77779) {
		if (max77779_ifpmic_setup(bcl_dev, pdev) < 0)
			return -ENODEV;
	} else {
		if (max77759_ifpmic_setup(bcl_dev, pdev) < 0)
			return -ENODEV;
	}
	return 0;
}

int ifpmic_init_fs(struct bcl_device *bcl_dev)
{
	int ret;

	ret = ifpmic_class_create();
	if (ret) {
		dev_err(bcl_dev->device, "Failed to create class(pmic) %d\n", ret);
		return ret;
	}

	if (bcl_dev->ifpmic == MAX77759)
		bcl_dev->mitigation_dev = ifpmic_device_create_with_groups(NULL, bcl_dev,
									   mitigation_mw_groups,
									   "mitigation");
	else
		bcl_dev->mitigation_dev = ifpmic_device_create_with_groups(NULL, bcl_dev,
									   mitigation_sq_groups,
									   "mitigation");
	if (IS_ERR(bcl_dev->mitigation_dev)) {
		ifpmic_class_destroy();
		return -ENODEV;
	}

	return 0;
}

void ifpmic_destroy_fs(struct bcl_device *bcl_dev)
{
	if (!IS_ERR_OR_NULL(bcl_dev->mitigation_dev))
		ifpmic_device_destroy(bcl_dev->mitigation_dev->devt);

	ifpmic_class_destroy();
}

bool ifpmic_retrieve_batoilo_asserted(struct device *dev, enum IFPMIC ifpmic)
{
	int ret, assert = 0;
	u8 regval;

	if (ifpmic != MAX77779)
		return true;

	ret = max77779_external_chg_reg_read(dev, MAX77779_CHG_DETAILS_01, &regval);
	if (ret < 0) {
		dev_err(dev, "IRQ read: %d, fail\n", regval);
		return false;
	}
	assert = _max77779_chg_details_01_bat_dtls_get(regval);
	if (assert == BAT_DTLS_OILO_ASSERTED)
		return true;
	return false;
}

void ifpmic_teardown(struct bcl_device *bcl_dev)
{
	if (bcl_dev->ifpmic == MAX77759) {
		max77759_clr_irq(bcl_dev, UVLO1);
		max77759_clr_irq(bcl_dev, UVLO2);
		max77759_clr_irq(bcl_dev, BATOILO1);
	} else if (bcl_dev->ifpmic == MAX77779) {
		max77779_clr_irq(bcl_dev, UVLO1);
		max77779_clr_irq(bcl_dev, UVLO2);
		max77779_clr_irq(bcl_dev, BATOILO1);
		max77779_clr_irq(bcl_dev, BATOILO2);
	}
	if (bcl_dev->rd_last_curr_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->rd_last_curr_work);
}

int batoilo_reg_read(struct device *dev, enum IFPMIC ifpmic, int oilo, unsigned int *val)
{
	int ret;
	uint8_t reg, regval;

	if (ifpmic == MAX77779) {
		reg = (oilo == BATOILO1) ? MAX77779_BAT_OILO1_CNFG_0 : MAX77779_BAT_OILO2_CNFG_0;
		ret = max77779_external_chg_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (oilo == BATOILO1)
			*val = _max77779_bat_oilo1_cnfg_0_bat_oilo1_get(regval);
		else
			*val = _max77779_bat_oilo2_cnfg_0_bat_oilo2_get(regval);
	} else {
		reg = MAX77759_CHG_CNFG_14;
		ret = max77759_external_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		*val = _chg_cnfg_14_bat_oilo_get(regval);
	}
	return ret;
}

int batoilo_reg_write(struct device *dev, uint8_t val, enum IFPMIC ifpmic, int oilo)
{
	int ret;
	uint8_t reg, regval;

	if (ifpmic == MAX77779) {
		reg = (oilo == BATOILO1) ? MAX77779_BAT_OILO1_CNFG_0 : MAX77779_BAT_OILO2_CNFG_0;
		ret = max77779_external_chg_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (oilo == BATOILO1)
			regval = _max77779_bat_oilo1_cnfg_0_bat_oilo1_set(regval, val);
		else
			regval = _max77779_bat_oilo2_cnfg_0_bat_oilo2_set(regval, val);
		ret = max77779_external_chg_reg_write(dev, reg, regval);
		if (ret < 0)
			return -EINVAL;
	} else {
		reg = MAX77759_CHG_CNFG_14;
		ret = max77759_external_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		regval = _chg_cnfg_14_bat_oilo_set(regval, val);
		ret = max77759_external_reg_write(dev, reg, regval);
		if (ret < 0)
			return -EINVAL;
	}
	return ret;
}

int uvlo_reg_read(struct device *dev, enum IFPMIC ifpmic, int triggered, unsigned int *val)
{
	int ret;
	uint8_t reg, regval;

	if (ifpmic == MAX77779) {
		reg = (triggered == UVLO1) ? MAX77779_SYS_UVLO1_CNFG_0 : MAX77779_SYS_UVLO2_CNFG_0;
		ret = max77779_external_chg_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (triggered == UVLO1)
			*val = _max77779_sys_uvlo1_cnfg_0_sys_uvlo1_get(regval);
		else
			*val = _max77779_sys_uvlo2_cnfg_0_sys_uvlo2_get(regval);
	} else {
		reg = (triggered == UVLO1) ? MAX77759_CHG_CNFG_15 : MAX77759_CHG_CNFG_16;
		ret = max77759_external_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (triggered == UVLO1)
			*val = _chg_cnfg_15_sys_uvlo1_get(regval);
		else
			*val = _chg_cnfg_16_sys_uvlo2_get(regval);
	}
	return ret;
}

int uvlo_reg_write(struct device *dev, uint8_t val, enum IFPMIC ifpmic, int triggered)
{
	int ret;
	uint8_t reg, regval;

	if (ifpmic == MAX77779) {
		reg = (triggered == UVLO1) ? MAX77779_SYS_UVLO1_CNFG_0 : MAX77779_SYS_UVLO2_CNFG_0;
		ret = max77779_external_chg_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (triggered == UVLO1)
			regval = _max77779_sys_uvlo1_cnfg_0_sys_uvlo1_set(regval, val);
		else
			regval = _max77779_sys_uvlo2_cnfg_0_sys_uvlo2_set(regval, val);
		ret = max77779_external_chg_reg_write(dev, reg, regval);
		if (ret < 0)
			return -EINVAL;
	} else {
		reg = (triggered == UVLO1) ? MAX77759_CHG_CNFG_15 : MAX77759_CHG_CNFG_16;
		ret = max77759_external_reg_read(dev, reg, &regval);
		if (ret < 0)
			return -EINVAL;
		if (triggered == UVLO1)
			regval = _chg_cnfg_15_sys_uvlo1_set(regval, val);
		else
			regval = _chg_cnfg_16_sys_uvlo2_set(regval, val);
		ret = max77759_external_reg_write(dev, reg, regval);
		if (ret < 0)
			return -EINVAL;
	}
	return ret;
}
