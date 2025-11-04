// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 *
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <max77759_regs.h>
#include <max777x9_bcl.h>
#include "../ifpmic_defs.h"
#include "max77759_irq.h"


static int max77759_register_irq(struct bcl_device *bcl_dev, const char *irq_name, int idx,
				 u32 link)
{
	int irq, pin, ret, irq_config;
	u32 flag;

	switch (link) {
	case IF_VDROOP1:
		irq = gpio_to_irq(bcl_dev->vdroop1_pin);
		pin = bcl_dev->vdroop1_pin;
		irq_config = IRQ_EXIST;
		break;
	case IF_VDROOP2:
		irq = gpio_to_irq(bcl_dev->vdroop2_pin);
		pin = bcl_dev->vdroop2_pin;
		irq_config = IRQ_EXIST;
		break;
	case IF_INTB:
		irq = bcl_dev->pmic_irq;
		pin = NOT_USED;
		irq_config = IRQ_NOT_EXIST;
		break;
	default:
		return 0;
	};

	flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	ret = google_bcl_register_zone(bcl_dev, idx, irq_name, pin, irq,
				       IF_PMIC, irq_config, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: %s\n", irq_name);
		return -ENODEV;
	}
	return 0;
}

static int max77759_intf_pmic_init(struct bcl_device *bcl_dev)
{
	int ret;
	unsigned int uvlo1_lvl, uvlo2_lvl, batoilo_lvl, batoilo2_lvl, lvl;
	struct device_node *np = bcl_dev->device->of_node;
	struct device_node *p_np;
	u32 uvlo1_link, uvlo2_link, oilo1_link, link;

	batoilo_reg_read(bcl_dev->intf_pmic_dev, bcl_dev->ifpmic, BATOILO2, &lvl);
	batoilo2_lvl = BO_STEP * lvl + bcl_dev->batt_irq_conf1.batoilo_lower_limit;
	batoilo_reg_read(bcl_dev->intf_pmic_dev, bcl_dev->ifpmic, BATOILO1, &lvl);
	batoilo_lvl = BO_STEP * lvl + bcl_dev->batt_irq_conf1.batoilo_lower_limit;
	uvlo_reg_read(bcl_dev->intf_pmic_dev, bcl_dev->ifpmic, UVLO1, &uvlo1_lvl);
	uvlo_reg_read(bcl_dev->intf_pmic_dev, bcl_dev->ifpmic, UVLO2, &uvlo2_lvl);

	p_np = of_get_child_by_name(np, "ifpmic_irq_mapping");
	if (!p_np)
		return -EINVAL;
	ret = of_property_read_u32(p_np, "sys_uvlo1", &link);
	uvlo1_link = ret ? -1 : link;
	ret = of_property_read_u32(p_np, "sys_uvlo2", &link);
	uvlo2_link = ret ? -1 : link;
	ret = of_property_read_u32(p_np, "bat_oilo1", &link);
	oilo1_link = ret ? -1 : link;
	if (max77759_register_irq(bcl_dev, "uvlo1", UVLO1, uvlo1_link) != 0)
		return -EINVAL;
	if (max77759_register_irq(bcl_dev, "uvlo2", UVLO2, uvlo2_link) != 0)
		return -EINVAL;
	if (max77759_register_irq(bcl_dev, "oilo1", BATOILO1, oilo1_link) != 0)
		return -EINVAL;

	max77759_clr_irq(bcl_dev, UVLO1);
	max77759_clr_irq(bcl_dev, UVLO2);
	max77759_clr_irq(bcl_dev, BATOILO1);

	return ret;
}

static void max77759_ifpmic_parse_dt(struct bcl_device *bcl_dev)
{
	struct device_node *np = bcl_dev->device->of_node;
	int ret;
	u32 retval;

	if (!np)
		return;

	ret = of_property_read_u32(np, "batoilo_lower", &retval);
	bcl_dev->batt_irq_conf1.batoilo_lower_limit = ret ? BO_LOWER_LIMIT : retval;
	ret = of_property_read_u32(np, "batoilo_upper", &retval);
	bcl_dev->batt_irq_conf1.batoilo_upper_limit = ret ? BO_UPPER_LIMIT : retval;
	ret = of_property_read_u32(np, "batoilo2_lower", &retval);
	bcl_dev->batt_irq_conf2.batoilo_lower_limit = ret ? BO_LOWER_LIMIT : retval;
	ret = of_property_read_u32(np, "batoilo2_upper", &retval);
	bcl_dev->batt_irq_conf2.batoilo_upper_limit = ret ? BO_UPPER_LIMIT : retval;
	ret = of_property_read_u32(np, "batoilo_trig_lvl", &retval);
	retval = ret ? BO_LIMIT : retval;
	bcl_dev->batt_irq_conf1.batoilo_trig_lvl =
			(retval - bcl_dev->batt_irq_conf1.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo2_trig_lvl", &retval);
	retval = ret ? BO_LIMIT : retval;
	bcl_dev->batt_irq_conf2.batoilo_trig_lvl =
			(retval - bcl_dev->batt_irq_conf2.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo_usb_trig_lvl", &retval);
	bcl_dev->batt_irq_conf1.batoilo_usb_trig_lvl = ret ?
			bcl_dev->batt_irq_conf1.batoilo_trig_lvl :
			(retval - bcl_dev->batt_irq_conf1.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo2_usb_trig_lvl", &retval);
	bcl_dev->batt_irq_conf2.batoilo_usb_trig_lvl = ret ?
			bcl_dev->batt_irq_conf2.batoilo_trig_lvl :
			(retval - bcl_dev->batt_irq_conf2.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo_wlc_trig_lvl", &retval);
	bcl_dev->batt_irq_conf1.batoilo_wlc_trig_lvl = ret ?
			bcl_dev->batt_irq_conf1.batoilo_trig_lvl :
			(retval - bcl_dev->batt_irq_conf1.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo2_wlc_trig_lvl", &retval);
	bcl_dev->batt_irq_conf2.batoilo_wlc_trig_lvl = ret ?
			bcl_dev->batt_irq_conf2.batoilo_trig_lvl :
			(retval - bcl_dev->batt_irq_conf2.batoilo_lower_limit) / BO_STEP;
	ret = of_property_read_u32(np, "batoilo_bat_open_to", &retval);
	bcl_dev->batt_irq_conf1.batoilo_bat_open_to = ret ? BO_BAT_OPEN_TO_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo2_bat_open_to", &retval);
	bcl_dev->batt_irq_conf2.batoilo_bat_open_to = ret ? BO_BAT_OPEN_TO_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo_rel", &retval);
	bcl_dev->batt_irq_conf1.batoilo_rel = ret ? BO_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo2_rel", &retval);
	bcl_dev->batt_irq_conf2.batoilo_rel = ret ? BO_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo_int_rel", &retval);
	bcl_dev->batt_irq_conf1.batoilo_int_rel = ret ? BO_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo2_int_rel", &retval);
	bcl_dev->batt_irq_conf2.batoilo_int_rel = ret ? BO_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo_det", &retval);
	bcl_dev->batt_irq_conf1.batoilo_det = ret ? BO_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo2_det", &retval);
	bcl_dev->batt_irq_conf2.batoilo_det = ret ? BO_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo_int_det", &retval);
	bcl_dev->batt_irq_conf1.batoilo_int_det = ret ? BO_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "batoilo2_int_det", &retval);
	bcl_dev->batt_irq_conf2.batoilo_int_det = ret ? BO_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "uvlo1_det", &retval);
	bcl_dev->batt_irq_conf1.uvlo_det = ret ? UV_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "uvlo2_det", &retval);
	bcl_dev->batt_irq_conf2.uvlo_det = ret ? UV_INT_REL_DEFAULT : retval;
	ret = of_property_read_u32(np, "uvlo1_rel", &retval);
	bcl_dev->batt_irq_conf1.uvlo_rel = ret ? UV_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "uvlo2_rel", &retval);
	bcl_dev->batt_irq_conf2.uvlo_rel = ret ? UV_INT_DET_DEFAULT : retval;
	ret = of_property_read_u32(np, "evt_cnt_enable", &retval);
	bcl_dev->evt_cnt.enable = ret ? EVT_CNT_ENABLE_DEFAULT : retval;
	ret = of_property_read_u32(np, "evt_cnt_rate", &retval);
	bcl_dev->evt_cnt.rate = ret ? EVT_CNT_RATE_DEFAULT : retval;
	bcl_dev->uvlo1_vdrp1_en = of_property_read_bool(np, "uvlo1_vdrp1_en");
	bcl_dev->uvlo1_vdrp2_en = of_property_read_bool(np, "uvlo1_vdrp2_en");
	bcl_dev->uvlo2_vdrp1_en = of_property_read_bool(np, "uvlo2_vdrp1_en");
	bcl_dev->uvlo2_vdrp2_en = of_property_read_bool(np, "uvlo2_vdrp2_en");
	bcl_dev->oilo1_vdrp1_en = of_property_read_bool(np, "oilo1_vdrp1_en");
	bcl_dev->oilo1_vdrp2_en = of_property_read_bool(np, "oilo1_vdrp2_en");
	bcl_dev->oilo2_vdrp1_en = of_property_read_bool(np, "oilo2_vdrp1_en");
	bcl_dev->oilo2_vdrp2_en = of_property_read_bool(np, "oilo2_vdrp2_en");
	ret = of_property_read_u32(np, "uvlo1_lvl", &retval);
	bcl_dev->uvlo1_lvl = ret ? DEFAULT_SYS_UVLO1_LVL : retval;
	ret = of_property_read_u32(np, "uvlo2_lvl", &retval);
	bcl_dev->uvlo2_lvl = ret ? DEFAULT_SYS_UVLO2_LVL : retval;
	ret = of_property_read_u32(np, "vdroop_int_mask", &retval);
	bcl_dev->vdroop_int_mask = ret ? DEFAULT_VDROOP_INT_MASK : retval;
	ret = of_property_read_u32(np, "intb_int_mask", &retval);
	bcl_dev->intb_int_mask = ret ? DEFAULT_INTB_MASK : retval;
}

int max77759_ifpmic_setup(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	int ret = 0;

	max77759_ifpmic_parse_dt(bcl_dev);

	ret = max77759_intf_pmic_init(bcl_dev);
	if (ret < 0) {
		dev_err(bcl_dev->device, "Interface PMIC initialization err:%d\n", ret);
		return ret;
	}

	return 0;
}

int max77759_get_irq(struct bcl_device *bcl_dev, u8 *irq_val)
{
	u8 chg_int;
	u8 ret;
	u8 clr_bcl_irq_mask;

	clr_bcl_irq_mask = (MAX77759_CHG_INT2_BAT_OILO_I | MAX77759_CHG_INT2_SYS_UVLO1_I |
			    MAX77759_CHG_INT2_SYS_UVLO2_I);
	ret = max77759_external_reg_read(bcl_dev->intf_pmic_dev, MAX77759_CHG_INT2, &chg_int);
	if (ret < 0)
		return IRQ_NONE;
	if ((chg_int & clr_bcl_irq_mask) == 0)
		return IRQ_NONE;

	/* UVLO2 has the highest priority and then BATOILO, then UVLO1 */
	if (chg_int & MAX77759_CHG_INT2_SYS_UVLO2_I)
		*irq_val = UVLO2;
	else if (chg_int & MAX77759_CHG_INT2_BAT_OILO_I)
		*irq_val = BATOILO;
	else if (chg_int & MAX77759_CHG_INT2_SYS_UVLO1_I)
		*irq_val = UVLO1;

	return ret;
}

int max77759_clr_irq(struct bcl_device *bcl_dev, int idx)
{
	u8 irq_val = 0;
	u8 chg_int = 0;
	int ret;

	if (idx == NOT_USED) {
		irq_val = idx;
	} else {
		if (max77759_get_irq(bcl_dev, &irq_val) != 0)
			return IRQ_NONE;
	}
	if (irq_val == UVLO2)
		chg_int = MAX77759_CHG_INT2_SYS_UVLO2_I | MAX77759_CHG_INT2_BAT_OILO_I;
	else if (irq_val == UVLO1)
		chg_int = MAX77759_CHG_INT2_SYS_UVLO1_I;
	else if (irq_val == BATOILO)
		chg_int = MAX77759_CHG_INT2_SYS_UVLO2_I | MAX77759_CHG_INT2_BAT_OILO_I;

	ret = max77759_external_reg_write(bcl_dev->intf_pmic_dev, MAX77759_CHG_INT2, chg_int);
	if (ret < 0)
		return IRQ_NONE;
	return ret;

}

int max77759_vimon_read(struct bcl_device *bcl_dev) { return 0; }
