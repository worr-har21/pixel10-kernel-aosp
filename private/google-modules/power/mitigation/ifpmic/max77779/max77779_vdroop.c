// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 *
 */

#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include "ifpmic_defs.h"
#include "max77779_irq.h"
#include <max77779.h>
#include <max77779_vimon.h>
#include <max777x9_bcl.h>

#define MAX77779_VIMON_BCL_CLIENT 0
#define MAX77779_VIMON_BCL_SAMPLE_COUNT 16
#define MAX77779_VIMON_CLIENT_TRIG_FOREVER -1

struct max77779_sample_data {
	uint16_t v_val;
	int16_t i_val;
} __packed;

int evt_cnt_rd_and_clr(struct bcl_device *bcl_dev, int idx, bool update_evt_cnt)
{
	int ret;
	u8 reg, val;

	switch (idx) {
	case UVLO1:
		reg = MAX77779_PMIC_EVENT_CNT_UVLO0;
		break;
	case UVLO2:
		reg = MAX77779_PMIC_EVENT_CNT_UVLO1;
		break;
	case BATOILO1:
		reg = MAX77779_PMIC_EVENT_CNT_OILO0;
		break;
	case BATOILO2:
		reg = MAX77779_PMIC_EVENT_CNT_OILO1;
		break;
	}

	/* Read to clear register */
	ret = max77779_external_pmic_reg_read(bcl_dev->irq_pmic_dev, reg, &val);
	if (ret < 0) {
		dev_err(bcl_dev->device, "%s: %d, fail\n", __func__, reg);
		return -ENODEV;
	}

	switch (idx) {
	case UVLO1:
		bcl_dev->evt_cnt_latest.uvlo1 = val;
		if (update_evt_cnt)
			bcl_dev->evt_cnt.uvlo1 = val;
		break;
	case UVLO2:
		bcl_dev->evt_cnt_latest.uvlo2 = val;
		if (update_evt_cnt)
			bcl_dev->evt_cnt.uvlo2 = val;
		break;
	case BATOILO1:
		bcl_dev->evt_cnt_latest.batoilo1 = val;
		if (update_evt_cnt)
			bcl_dev->evt_cnt.batoilo1 = val;
		break;
	case BATOILO2:
		bcl_dev->evt_cnt_latest.batoilo2 = val;
		if (update_evt_cnt)
			bcl_dev->evt_cnt.batoilo2 = val;
		break;
	}
	return 0;
}

static int max77779_register_irq(struct bcl_device *bcl_dev, const char *irq_name, int idx,
				 u32 link)
{
	int irq, pin, ret, irq_config;
	u32 flag;

	switch (link) {
	case IF_VDROOP1:
		irq = bcl_dev->vdroop1_irq;
		pin = bcl_dev->vdroop1_pin;
		irq_config = IRQ_EXIST;
		break;
	case IF_VDROOP2:
		irq = bcl_dev->vdroop2_irq;
		pin = bcl_dev->vdroop2_pin;
		irq_config = IRQ_EXIST;
		break;
	case IF_INTB:
		irq = bcl_dev->pmic_irq;
		pin = NOT_USED;
		irq_config = IRQ_EXIST;
		break;
	case IF_SHARED:
		irq = 0;
		irq_config = IRQ_NOT_EXIST;
		pin = NOT_USED;
		break;
	default:
		return 0;
	};

	if (IS_ENABLED(CONFIG_GOOGLE_MFD_DA9188))
		flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING;
	else
		flag = IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	ret = google_bcl_register_zone(bcl_dev, idx, irq_name, pin, irq,
				       IF_PMIC, irq_config, POLARITY_HIGH, flag);
	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl_register fail: %s\n", irq_name);
		return -ENODEV;
	}
	return 0;
}

static void max77779_bcl_on_sample_ready(void *device, const enum vimon_trigger_source reason,
					 const u16 *buf, const size_t buf_size)
{
	struct bcl_device *bcl_dev = device;
	int i_max = 0;
	const int count = buf_size / sizeof(struct max77779_sample_data);
	const struct max77779_sample_data *sample = (const struct max77779_sample_data *)buf;
	int i, i_rdback;

	if (reason == VIMON_CLIENT_REQUEST)
		goto max77779_bcl_trigger_mitigation;

	for (i = 0; i < count; i++) {
		i_rdback = ((int64_t)sample->i_val * MAX77779_VIMON_NA_PER_LSB) /
			   MILLI_UNITS_TO_NANO_UNITS;

		i_max = max(i_max, i_rdback);
		sample++;
	}

	if (i_max <= bcl_dev->vimon_pwr_loop_thresh)
		return;

max77779_bcl_trigger_mitigation:
	google_pwr_loop_trigger_mitigation(bcl_dev);
}

static void max77779_bcl_on_sample_removed(void *device)
{
}

static bool max77779_bcl_extra_trigger(void *device, const uint16_t *buf, const size_t buf_size)
{
	struct bcl_device *bcl_dev = device;
	const int count = buf_size / sizeof(struct max77779_sample_data);
	const struct max77779_sample_data *sample = (const struct max77779_sample_data *)buf;
	int i, i_rdback;

	if (!bcl_dev->vimon_pwr_loop_en)
		return false;

	for (i = 0; i < count; i++) {
		i_rdback = ((int64_t)sample->i_val * MAX77779_VIMON_NA_PER_LSB) /
			   MILLI_UNITS_TO_NANO_UNITS;

		if (i_rdback > bcl_dev->vimon_pwr_loop_thresh)
			return true;

		sample++;
	}

	return false;
}

static struct vimon_client_callbacks max77779_vimon_bcl_client = {
	.on_sample_ready = max77779_bcl_on_sample_ready,
	.on_removed = max77779_bcl_on_sample_removed,
	.extra_trigger = max77779_bcl_extra_trigger,
};

static int google_bcl_update_last_curr(struct bcl_device *bcl_dev)
{
	int ret = 0;
	u16 readout;

	bcl_dev->last_curr_rd_retry_cnt--;
	ret = max77779_external_fg_reg_read(bcl_dev->fg_pmic_dev,
					    MAX77779_FG_MaxMinCurr,
					    &readout);
	if (ret == -EAGAIN)
		return ret;

	if (ret < 0) {
		dev_err(bcl_dev->device, "bcl read of last current failed: %d\n", ret);
		return ret;
	}

	readout &= MAX77779_FG_MaxMinCurr_MAXCURR_MASK;
	readout = readout >> MAX77779_FG_MaxMinCurr_MAXCURR_SHIFT;
	bcl_dev->last_current = readout;
	dev_dbg(bcl_dev->device, "LAST CURRENT: %#x\n", bcl_dev->last_current);

	return ret;
}

static void google_bcl_rd_last_curr(struct work_struct *work)
{
	struct bcl_device *bcl_dev = container_of(work, struct bcl_device,
						  rd_last_curr_work.work);

	if (google_bcl_update_last_curr(bcl_dev) == -EAGAIN && bcl_dev->last_curr_rd_retry_cnt > 0)
		schedule_delayed_work(&bcl_dev->rd_last_curr_work,
				      msecs_to_jiffies(TIMEOUT_5S));
}

static int max77779_intf_pmic_init(struct bcl_device *bcl_dev)
{
	int ret;
	u8 val, retval;
	unsigned int uvlo1_lvl, uvlo2_lvl, batoilo_lvl, batoilo2_lvl, lvl;
	struct device_node *np = bcl_dev->device->of_node;
	struct device_node *p_np;
	u32 uvlo1_link, uvlo2_link, oilo1_link, oilo2_link, link;

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
	ret = of_property_read_u32(p_np, "bat_oilo2", &link);
	oilo2_link = ret ? -1 : link;
	if (max77779_register_irq(bcl_dev, "uvlo1", UVLO1, uvlo1_link) != 0)
		return -EINVAL;
	if (max77779_register_irq(bcl_dev, "uvlo2", UVLO2, uvlo2_link) != 0)
		return -EINVAL;
	if (max77779_register_irq(bcl_dev, "oilo1", BATOILO1, oilo1_link) != 0)
		return -EINVAL;
	if (max77779_register_irq(bcl_dev, "oilo2", BATOILO2, oilo2_link) != 0)
		return -EINVAL;

	/* Setup mitigation IRQ */
	max77779_external_pmic_reg_write(bcl_dev->irq_pmic_dev, MAX77779_PMIC_VDROOP_INT_MASK,
					 bcl_dev->vdroop_int_mask);
	max77779_external_pmic_reg_read(bcl_dev->irq_pmic_dev, MAX77779_PMIC_INTB_MASK, &retval);
	val = bcl_dev->intb_int_mask;
	retval = _max77779_pmic_intb_mask_vdroop_int_m_set(retval, val);
	max77779_external_pmic_reg_write(bcl_dev->irq_pmic_dev, MAX77779_PMIC_INTB_MASK, retval);

	/* UVLO2 no VDROOP2 */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_1, &val);
	val = _max77779_sys_uvlo2_cnfg_1_sys_uvlo2_vdrp2_en_set(val, bcl_dev->uvlo2_vdrp2_en);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_1, val);
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_0, &val);
	val = _max77779_sys_uvlo2_cnfg_0_sys_uvlo2_set(val, bcl_dev->uvlo2_lvl);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_0, val);
	/* UVLO1 = VDROOP1, 3.1V */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_1, &val);
	val = _max77779_sys_uvlo1_cnfg_1_sys_uvlo1_vdrp1_en_set(val, bcl_dev->uvlo1_vdrp1_en);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_1, val);
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_0, &val);
	val = _max77779_sys_uvlo1_cnfg_0_sys_uvlo1_set(val, bcl_dev->uvlo1_lvl);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_0, val);

	/* BATOILO1 = VDROOP2, 36ms BATOILO1 BAT_OPEN */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_3, &val);
	val = _max77779_bat_oilo1_cnfg_3_bat_oilo1_vdrp1_en_set(val, bcl_dev->oilo1_vdrp1_en);
	val = _max77779_bat_oilo1_cnfg_3_bat_oilo1_vdrp2_en_set(val, bcl_dev->oilo1_vdrp2_en);
	val = _max77779_bat_oilo1_cnfg_3_bat_open_to_1_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_bat_open_to);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_3, val);

	/* BATOILO2 = VDROOP1/2, 12ms BATOILO2 BAT_OPEN */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_3, &val);
	val = _max77779_bat_oilo2_cnfg_3_bat_oilo2_vdrp1_en_set(val,
								bcl_dev->oilo2_vdrp1_en);
	val = _max77779_bat_oilo2_cnfg_3_bat_oilo2_vdrp2_en_set(val,
								bcl_dev->oilo2_vdrp2_en);
	val = _max77779_bat_oilo2_cnfg_3_bat_open_to_2_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_bat_open_to);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_3, val);

	/* BATOILO1 5A THRESHOLD */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_0, &val);
	val = _max77779_bat_oilo1_cnfg_0_bat_oilo1_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_trig_lvl);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_0, val);

	/* BATOILO2 8A THRESHOLD */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_0, &val);
	val = _max77779_bat_oilo2_cnfg_0_bat_oilo2_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_trig_lvl);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_0, val);

	/* BATOILO INT and VDROOP1 REL and DET */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_1, &val);
	val = _max77779_bat_oilo1_cnfg_1_bat_oilo1_rel_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_rel);
	val = _max77779_bat_oilo1_cnfg_1_bat_oilo1_det_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_1, val);

	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_2, &val);
	val = _max77779_bat_oilo1_cnfg_2_bat_oilo1_int_rel_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_int_rel);
	val = _max77779_bat_oilo1_cnfg_2_bat_oilo1_int_det_set(
					 val, bcl_dev->batt_irq_conf1.batoilo_int_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_2, val);

	/* BATOILO2 INT and VDROOP2 REL and DET */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_1, &val);
	val = _max77779_bat_oilo2_cnfg_1_bat_oilo2_rel_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_rel);
	val = _max77779_bat_oilo2_cnfg_1_bat_oilo2_det_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_1, val);

	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_2, &val);
	val = _max77779_bat_oilo2_cnfg_2_bat_oilo2_int_rel_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_int_rel);
	val = _max77779_bat_oilo2_cnfg_2_bat_oilo2_int_det_set(
					 val, bcl_dev->batt_irq_conf2.batoilo_int_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_2, val);

	/* UVLO1 INT and VDROOP1 REL and DET */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_1, &val);
	val = _max77779_sys_uvlo1_cnfg_1_sys_uvlo1_rel_set(
					 val, bcl_dev->batt_irq_conf1.uvlo_rel);
	val = _max77779_sys_uvlo1_cnfg_1_sys_uvlo1_det_set(
					 val, bcl_dev->batt_irq_conf1.uvlo_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO1_CNFG_1, val);

	/* UVLO2 INT and VDROOP1 REL and DET */
	max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_1, &val);
	val = _max77779_sys_uvlo2_cnfg_1_sys_uvlo2_rel_set(
					 val, bcl_dev->batt_irq_conf2.uvlo_rel);
	val = _max77779_sys_uvlo2_cnfg_1_sys_uvlo2_det_set(
					 val, bcl_dev->batt_irq_conf2.uvlo_det);
	max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_SYS_UVLO2_CNFG_1, val);

	/* Read, save, and clear event counters */
	evt_cnt_rd_and_clr(bcl_dev, UVLO1, true);
	evt_cnt_rd_and_clr(bcl_dev, UVLO2, true);
	evt_cnt_rd_and_clr(bcl_dev, BATOILO1, true);
	evt_cnt_rd_and_clr(bcl_dev, BATOILO2, true);

	/* Enable event counter if it is not enabled */
	max77779_external_pmic_reg_read(bcl_dev->irq_pmic_dev,
					MAX77779_PMIC_EVENT_CNT_CFG, &retval);
	retval = _max77779_pmic_event_cnt_cfg_enable_set(
					 retval, bcl_dev->evt_cnt.enable);
	retval = _max77779_pmic_event_cnt_cfg_sample_rate_set(
					 retval, bcl_dev->evt_cnt.rate);
	max77779_external_pmic_reg_write(bcl_dev->irq_pmic_dev,
					 MAX77779_PMIC_EVENT_CNT_CFG, retval);

	max77779_clr_irq(bcl_dev, UVLO1);
	max77779_clr_irq(bcl_dev, UVLO2);
	max77779_clr_irq(bcl_dev, BATOILO1);
	max77779_clr_irq(bcl_dev, BATOILO2);

	if (!IS_ENABLED(CONFIG_REGULATOR_S2MPG14) && !IS_ENABLED(CONFIG_SOC_LGA))
		return 0;

	if (!bcl_dev->vimon_pwr_loop_en)
		return 0;

	ret = vimon_register_callback(bcl_dev->vimon_dev, VIMON_BATOILO1_TRIGGER,
				      MAX77779_VIMON_CLIENT_TRIG_FOREVER, bcl_dev,
				      &max77779_vimon_bcl_client);
	if (ret)
		dev_err(bcl_dev->device, "bcl_vimon_client register callback failed %d\n", ret);

	return ret;
}

static void max77779_ifpmic_parse_dt(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	struct device_node *np = bcl_dev->device->of_node;
	int ret;
	u32 retval;

	if (!np)
		return;
	if (IS_ENABLED(CONFIG_GOOGLE_MFD_DA9188)) {
		bcl_dev->vdroop1_irq = platform_get_irq_byname(pdev, "vd1");
		bcl_dev->vdroop2_irq = platform_get_irq_byname(pdev, "vd2");
		bcl_dev->pmic_irq = platform_get_irq_byname(pdev, "intb");
		bcl_dev->vdroop1_pin = of_get_named_gpio(np, "gpio,vd1", 0);
		bcl_dev->vdroop2_pin = of_get_named_gpio(np, "gpio,vd2", 0);
	} else {
		bcl_dev->pmic_irq = platform_get_irq(pdev, 0);
		bcl_dev->vdroop1_pin = of_get_named_gpio(np, "gpios", 0);
		bcl_dev->vdroop2_pin = of_get_named_gpio(np, "gpios", 1);
		bcl_dev->vdroop1_irq = gpio_to_irq(bcl_dev->vdroop1_pin);
		bcl_dev->vdroop2_irq = gpio_to_irq(bcl_dev->vdroop2_pin);
	}
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
	bcl_dev->vimon_pwr_loop_en = of_property_read_bool(np, "vimon_pwr_loop_en");
	ret = of_property_read_u32(np, "uvlo1_lvl", &retval);
	bcl_dev->uvlo1_lvl = ret ? DEFAULT_SYS_UVLO1_LVL : retval;
	ret = of_property_read_u32(np, "uvlo2_lvl", &retval);
	bcl_dev->uvlo2_lvl = ret ? DEFAULT_SYS_UVLO2_LVL : retval;
	ret = of_property_read_u32(np, "vdroop_int_mask", &retval);
	bcl_dev->vdroop_int_mask = ret ? DEFAULT_VDROOP_INT_MASK : retval;
	ret = of_property_read_u32(np, "intb_int_mask", &retval);
	bcl_dev->intb_int_mask = ret ? DEFAULT_INTB_MASK : retval;
	ret = of_property_read_u32(np, "vimon_pwr_loop_cnt", &retval);
	bcl_dev->vimon_pwr_loop_cnt = ret ? DEFAULT_VIMON_PWR_LOOP_CNT : retval;
	ret = of_property_read_u32(np, "vimon_pwr_loop_thresh", &retval);
	bcl_dev->vimon_pwr_loop_thresh = ret ? DEFAULT_VIMON_PWR_LOOP_THRESH : retval;
}

int max77779_ifpmic_setup(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	int ret = 0;
	u32 retval;
	struct device_node *np = bcl_dev->device->of_node;

	ret = of_property_read_u32(np, "google,ifpmic", &retval);
	bcl_dev->ifpmic = MAX77779;

	max77779_ifpmic_parse_dt(bcl_dev, pdev);

	bcl_dev->irq_pmic_dev = max77779_get_dev(bcl_dev->device, "google,pmic");
	if (!bcl_dev->irq_pmic_dev) {
		dev_err(bcl_dev->device, "Cannot find PMIC bus\n");
		return -ENODEV;
	}

	bcl_dev->fg_pmic_dev = max77779_get_dev(bcl_dev->device, "google,power-supply");
	if (!bcl_dev->fg_pmic_dev) {
		dev_err(bcl_dev->device, "Cannot find google,power-supply\n");
		return -ENODEV;
	}

	/* Readout last current */
	INIT_DELAYED_WORK(&bcl_dev->rd_last_curr_work, google_bcl_rd_last_curr);
	bcl_dev->last_curr_rd_retry_cnt = LAST_CURR_RD_CNT_MAX;
	if (google_bcl_update_last_curr(bcl_dev) == -EAGAIN)
		schedule_delayed_work(&bcl_dev->rd_last_curr_work,
				      msecs_to_jiffies(TIMEOUT_1000MS));

	bcl_dev->vimon_dev = max77779_get_dev(bcl_dev->device, "google,vimon");
	if (!bcl_dev->vimon_dev)
		dev_err(bcl_dev->device, "Cannot find max77779 vimon\n");

	ret = max77779_intf_pmic_init(bcl_dev);
	if (ret < 0) {
		dev_err(bcl_dev->device, "Interface PMIC initialization err:%d\n", ret);
		return ret;
	}

	return 0;
}

int max77779_adjust_batoilo_lvl(struct bcl_device *bcl_dev, u8 lower_enable, u8 set_batoilo1_lvl,
				u8 set_batoilo2_lvl)
{
	int ret;
	u8 val, batoilo1_lvl, batoilo2_lvl;

	if (lower_enable) {
		batoilo1_lvl = set_batoilo1_lvl;
		batoilo2_lvl = set_batoilo2_lvl;
	} else {
		batoilo1_lvl = bcl_dev->batt_irq_conf1.batoilo_trig_lvl;
		batoilo2_lvl = bcl_dev->batt_irq_conf2.batoilo_trig_lvl;
	}
	ret = max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_0,
					     &val);
	if (ret < 0)
		return ret;
	val = _max77779_bat_oilo1_cnfg_0_bat_oilo1_set(val, batoilo1_lvl);
	ret = max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO1_CNFG_0,
					      val);
	if (ret < 0)
		return ret;
	ret = max77779_external_chg_reg_read(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_0,
					     &val);
	if (ret < 0)
		return ret;
	val = _max77779_bat_oilo2_cnfg_0_bat_oilo2_set(val, batoilo2_lvl);
	ret = max77779_external_chg_reg_write(bcl_dev->intf_pmic_dev, MAX77779_BAT_OILO2_CNFG_0,
					      val);

	return ret;
}

int max77779_get_irq(struct bcl_device *bcl_dev, u8 *irq_val)
{
	u8 vdroop_int;
	u8 ret;
	u8 clr_bcl_irq_mask;

	clr_bcl_irq_mask = (MAX77779_PMIC_VDROOP_INT_BAT_OILO2_INT_MASK |
			    MAX77779_PMIC_VDROOP_INT_BAT_OILO1_INT_MASK |
			    MAX77779_PMIC_VDROOP_INT_SYS_UVLO1_INT_MASK |
			    MAX77779_PMIC_VDROOP_INT_SYS_UVLO2_INT_MASK);
	ret = max77779_external_pmic_reg_read(bcl_dev->irq_pmic_dev,
					      MAX77779_PMIC_VDROOP_INT, &vdroop_int);
	if (ret < 0)
		return IRQ_NONE;
	if ((vdroop_int & clr_bcl_irq_mask) == 0)
		return IRQ_NONE;

	/* UVLO2 has the highest priority and then BATOILO, then UVLO1 */
	if (vdroop_int & MAX77779_PMIC_VDROOP_INT_SYS_UVLO2_INT_MASK)
		*irq_val = UVLO2;
	else if (vdroop_int & MAX77779_PMIC_VDROOP_INT_BAT_OILO2_INT_MASK)
		*irq_val = BATOILO2;
	else if (vdroop_int & MAX77779_PMIC_VDROOP_INT_BAT_OILO1_INT_MASK)
		*irq_val = BATOILO1;
	else if (vdroop_int & MAX77779_PMIC_VDROOP_INT_SYS_UVLO1_INT_MASK)
		*irq_val = UVLO1;

	return ret;
}

int max77779_clr_irq(struct bcl_device *bcl_dev, int idx)
{
	u8 irq_val = 0;
	u8 chg_int = 0;
	int ret;

	if (idx != NOT_USED) {
		irq_val = idx;
	} else {
		if (max77779_get_irq(bcl_dev, &irq_val) != 0)
			return IRQ_NONE;
	}
	if (irq_val == UVLO2)
		chg_int = MAX77779_PMIC_VDROOP_INT_SYS_UVLO2_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO1_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO2_INT_MASK;
	else if (irq_val == UVLO1)
		chg_int = MAX77779_PMIC_VDROOP_INT_SYS_UVLO1_INT_MASK;
	else if (irq_val == BATOILO1)
		chg_int = MAX77779_PMIC_VDROOP_INT_SYS_UVLO2_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO1_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO2_INT_MASK;
	else if (irq_val == BATOILO2)
		chg_int = MAX77779_PMIC_VDROOP_INT_SYS_UVLO2_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO1_INT_MASK |
				MAX77779_PMIC_VDROOP_INT_BAT_OILO2_INT_MASK;

	ret = max77779_external_pmic_reg_write(bcl_dev->irq_pmic_dev,
					       MAX77779_PMIC_VDROOP_INT, chg_int);
	if (ret < 0)
		return IRQ_NONE;
	return ret;
}

int max77779_vimon_read(struct bcl_device *bcl_dev)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_REGULATOR_S2MPG14) && !IS_ENABLED(CONFIG_SOC_LGA))
		return ret;

	ret = max77779_external_vimon_read_buffer(bcl_dev->vimon_dev, bcl_dev->vimon_intf.data,
						  &bcl_dev->vimon_intf.count, VIMON_BUF_SIZE);
	if (ret == 0)
		return bcl_dev->vimon_intf.count;

	return ret;
}

