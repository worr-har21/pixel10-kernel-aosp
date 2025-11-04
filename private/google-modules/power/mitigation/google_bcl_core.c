// SPDX-License-Identifier: GPL-2.0 only
/*
 * google_bcl_core.c Google bcl core driver
 *
 * Copyright (c) 2022 Google LLC.
 *
 */
#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/debugfs.h>
#include "bcl.h"
#include "core_pmic/core_pmic_defs.h"
#include "ifpmic/ifpmic_defs.h"
#include "ifpmic/max77759/max77759_irq.h"
#include "ifpmic/max77779/max77779_irq.h"
#include "soc/soc_defs.h"

static const struct platform_device_id google_id_table[] = {
	{.name = "google_mitigation",},
	{},
};

void update_irq_start_times(struct bcl_device *bcl_dev, int id);
void update_irq_end_times(struct bcl_device *bcl_dev, int id);
void trace_bcl_zone_stats(struct bcl_zone *zone, int value);

static struct power_supply *google_get_power_supply(struct bcl_device *bcl_dev)
{
	static struct power_supply *psy[2];
	static struct power_supply *batt_psy;
	int err = 0;

	batt_psy = NULL;
	err = power_supply_get_by_phandle_array(bcl_dev->device->of_node, "google,power-supply",
						psy, ARRAY_SIZE(psy));
	if (err > 0)
		batt_psy = psy[0];
	return batt_psy;
}

static void ocpsmpl_read_stats(struct bcl_device *bcl_dev,
			       struct ocpsmpl_stats *dst, struct power_supply *psy)
{
	union power_supply_propval ret = {0};
	int err = 0;

	if (!psy)
		return;
	dst->_time = ktime_to_ms(ktime_get());
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
	if (err < 0)
		dst->capacity = -1;
	else {
		dst->capacity = ret.intval;
		bcl_dev->batt_psy_initialized = true;
	}
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &ret);
	if (err < 0)
		dst->voltage = -1;
	else {
		dst->voltage = ret.intval;
		bcl_dev->batt_psy_initialized = true;
	}

}

static int google_bcl_wait_for_response_locked(struct bcl_zone *zone, int timeout_ms)
{
	struct bcl_device *bcl_dev = zone->parent;
	if (bcl_dev->ifpmic == MAX77759)
		return 0;
	reinit_completion(&zone->deassert);
	return wait_for_completion_timeout(&zone->deassert, msecs_to_jiffies(timeout_ms));
}

static irqreturn_t latched_irq_handler(int irq, void *data)
{
	struct bcl_zone *zone = data;
	struct bcl_device *bcl_dev;
	u8 idx;

	if (!zone || !zone->parent)
		return IRQ_HANDLED;

	idx = zone->idx;
	bcl_dev = zone->parent;

	/* Ensure sw mitigation enabled is read correctly */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled)) {
		if (zone->irq_type == IF_PMIC)
			bcl_cb_clr_irq(bcl_dev, idx);
		return IRQ_HANDLED;
	}
	queue_work(system_unbound_wq, &zone->irq_triggered_work);
	return IRQ_HANDLED;
}

static bool google_warn_check(struct bcl_zone *zone)
{
	struct bcl_device *bcl_dev;
	int gpio_level;

	bcl_dev = zone->parent;
	if (zone->bcl_pin != NOT_USED) {
		gpio_level = gpio_get_value(zone->bcl_pin);
		return (gpio_level == zone->polarity);
	}
	return ifpmic_retrieve_batoilo_asserted(bcl_dev->intf_pmic_dev, bcl_dev->ifpmic);
}

static void google_bcl_release_throttling(struct bcl_zone *zone)
{
	struct bcl_device *bcl_dev;

	bcl_dev = zone->parent;
	zone->bcl_cur_lvl = 0;
	if (zone->bcl_qos)
		google_bcl_qos_update(zone, QOS_NONE);
	else if (zone->idx == BATOILO2 && bcl_dev->zone[BATOILO])
		google_bcl_qos_update(bcl_dev->zone[BATOILO], QOS_NONE);
	complete(&zone->deassert);
	trace_bcl_zone_stats(zone, 0);
	if (zone->irq_type == IF_PMIC) {
		update_irq_end_times(bcl_dev, zone->idx);
		if ((zone->idx == UVLO1 || zone->idx == BATOILO2 ||
		    zone->idx == UVLO2 || zone->idx == BATOILO1) &&
		    bcl_dev->ifpmic == MAX77779)
			evt_cnt_rd_and_clr(bcl_dev, zone->idx, false);
	}
	if (zone->idx == BATOILO)
		google_bcl_cancel_batfet_timer(bcl_dev);
}

static void google_warn_work(struct work_struct *work)
{
	struct bcl_zone *zone = container_of(work, struct bcl_zone, warn_work.work);
	struct bcl_device *bcl_dev;

	bcl_dev = zone->parent;
	if (!google_warn_check(zone)) {
		google_bcl_upstream_state(zone, DISABLED);
		google_bcl_release_throttling(zone);
	} else {
		zone->bcl_cur_lvl = zone->bcl_lvl + THERMAL_HYST_LEVEL;
		/* ODPM Read to kick off LIGHT module throttling */
		mod_delayed_work(bcl_dev->qos_update_wq, &zone->warn_work,
				 msecs_to_jiffies(TIMEOUT_5MS));
	}
}

static int google_bcl_set_soc(struct bcl_device *bcl_dev, int low, int high)
{
	if (IS_ERR_OR_NULL(bcl_dev) || IS_ERR_OR_NULL(bcl_dev->device))
		return 0;
	if (high == bcl_dev->trip_high_temp)
		return 0;

	bcl_dev->trip_low_temp = low;
	bcl_dev->trip_high_temp = high;
	schedule_delayed_work(&bcl_dev->soc_work, 0);

	return 0;
}

static int tz_bcl_set_soc(struct thermal_zone_device *tz, int low, int high)
{
	return google_bcl_set_soc(tz->devdata, low, high);
}

static int google_bcl_read_soc(struct bcl_device *bcl_dev, int *val)
{
	union power_supply_propval ret = {0};
	int err = 0;
	*val = 100;

	if (IS_ERR_OR_NULL(bcl_dev) || IS_ERR_OR_NULL(bcl_dev->device))
		return 0;
	/* Ensure bcl driver is initialized to avoid receiving external calls */
	if (!smp_load_acquire(&bcl_dev->initialized))
		return 0;
	if (!bcl_dev->batt_psy)
		bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	if (bcl_dev->batt_psy) {
		err = power_supply_get_property(bcl_dev->batt_psy,
						POWER_SUPPLY_PROP_CAPACITY, &ret);
		if (err < 0) {
			dev_err(bcl_dev->device, "battery percentage read error:%d\n", err);
			return err;
		}
		bcl_dev->batt_psy_initialized = true;
		*val = 100 - ret.intval;
	}
	dev_dbg(bcl_dev->device, "soc:%d\n", *val);

	return err;
}

static int tz_bcl_read_soc(struct thermal_zone_device *tz, int *val)
{
	return google_bcl_read_soc(tz->devdata, val);
}

static void google_bcl_evaluate_soc(struct work_struct *work)
{
	int battery_percentage_reverse;
	struct bcl_device *bcl_dev = container_of(work, struct bcl_device,
						  soc_work.work);

	if (google_bcl_read_soc(bcl_dev, &battery_percentage_reverse))
		return;

	if ((battery_percentage_reverse < bcl_dev->trip_high_temp) &&
		(battery_percentage_reverse > bcl_dev->trip_low_temp))
		return;

	bcl_dev->trip_val = battery_percentage_reverse;
	if (!bcl_dev->soc_tz) {
		bcl_dev->soc_tz = devm_thermal_of_zone_register(bcl_dev->device,
								PMIC_SOC, bcl_dev,
								&bcl_dev->soc_tz_ops);
		if (IS_ERR(bcl_dev->soc_tz)) {
			dev_err(bcl_dev->device, "soc TZ register failed. err:%ld\n",
				PTR_ERR(bcl_dev->soc_tz));
			return;
		}
	}
	if (!IS_ERR(bcl_dev->soc_tz))
		thermal_zone_device_update(bcl_dev->soc_tz, THERMAL_EVENT_UNSPECIFIED);
	return;
}

static int battery_supply_callback(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct power_supply *psy = data;
	struct bcl_device *bcl_dev = container_of(nb, struct bcl_device, psy_nb);
	struct power_supply *bcl_psy;

	if (IS_ERR_OR_NULL(bcl_dev))
		return NOTIFY_OK;

	bcl_psy = bcl_dev->batt_psy;

	if (!bcl_psy || event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (!strcmp(psy->desc->name, bcl_psy->desc->name))
		schedule_delayed_work(&bcl_dev->soc_work, 0);

	return NOTIFY_OK;
}

static int google_bcl_remove_thermal(struct bcl_device *bcl_dev)
{
	int i = 0;
	struct bcl_zone *zone;

	if (IS_ERR_OR_NULL(bcl_dev))
		return 0;
        if (bcl_dev->batt_psy_initialized)
		power_supply_unreg_notifier(&bcl_dev->psy_nb);
	for (i = 0; i < TRIGGERED_SOURCE_MAX; i++) {
		if (!bcl_dev->zone[i])
			continue;
		zone = bcl_dev->zone[i];
		if (zone->irq_reg) {
			if ((bcl_dev->ifpmic == MAX77779) && (i == BATOILO))
				devm_free_irq(bcl_dev->device, bcl_dev->pmic_irq, bcl_dev);
			else
				devm_free_irq(bcl_dev->device, zone->bcl_irq, zone);
		}
		zone->irq_reg = false;
		if (zone->irq_triggered_work.func != NULL)
			cancel_work_sync(&zone->irq_triggered_work);
		if (zone->warn_work.work.func != NULL)
			cancel_delayed_work_sync(&zone->warn_work);
		devm_kfree(bcl_dev->device, zone);
	}
	if (bcl_dev->main_pwr_irq_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->main_pwr_irq_work);
	if (bcl_dev->sub_pwr_irq_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->sub_pwr_irq_work);
	if (bcl_dev->setup_core_pmic_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_core_pmic_work);
	if (bcl_dev->setup_main_odpm_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_main_odpm_work);
	if (bcl_dev->setup_sub_odpm_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_sub_odpm_work);
	google_bcl_remove_qos(bcl_dev);
	google_bcl_remove_data_logging(bcl_dev);
	if (bcl_dev->qos_update_wq) {
		flush_workqueue(bcl_dev->qos_update_wq);
		destroy_workqueue(bcl_dev->qos_update_wq);
	}
	if (bcl_dev->soc_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->soc_work);
	if (bcl_dev->non_monitored_module_ids != NULL)
		kfree(bcl_dev->non_monitored_module_ids);
	cpu_pm_unregister_notifier(&bcl_dev->cpu_nb);
	google_bcl_remove_votable(bcl_dev);
	mutex_destroy(&bcl_dev->cpu_ratio_lock);
	mutex_destroy(&bcl_dev->sysreg_lock);
	google_bcl_teardown_mailbox(bcl_dev);
	core_pmic_teardown(bcl_dev);
	ifpmic_teardown(bcl_dev);

	return 0;
}

struct bcl_device *google_retrieve_bcl_handle(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct bcl_device *bcl_dev;

	np = of_find_node_by_name(NULL, "google-mitigation");
	if (!np)
		np = of_find_node_by_name(NULL, "google,mitigation");

	if (!np || !virt_addr_valid(np) || !of_device_is_available(np))
		return NULL;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return NULL;
	bcl_dev = platform_get_drvdata(pdev);
	if (IS_ERR_OR_NULL(bcl_dev))
		return NULL;

	return bcl_dev;
}
EXPORT_SYMBOL_GPL(google_retrieve_bcl_handle);

int google_init_tpu_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, TPU);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_tpu_ratio);

int google_init_gpu_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, GPU);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_gpu_ratio);

int google_init_aur_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, AUR);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_aur_ratio);

int google_pwr_loop_trigger_mitigation(struct bcl_device *bcl_dev)
{
	/* TODO: b/356694140 - implement power reduction */
	core_pmic_main_meter_read_lpf_data(bcl_dev, &bcl_dev->vimon_odpm_stats);
	return 0;
}

static void google_irq_triggered_work(struct work_struct *work)
{
	struct bcl_zone *zone = container_of(work, struct bcl_zone, irq_triggered_work);
	struct bcl_device *bcl_dev;
	u8 irq_val = 0;
	int idx;

	idx = zone->idx;
	bcl_dev = zone->parent;

	google_bcl_upstream_state(zone, START);

	if (zone->bcl_pin != NOT_USED) {
		if (bcl_dev->ifpmic == MAX77759 && idx >= UVLO2 && idx <= BATOILO2) {
			bcl_cb_get_irq(bcl_dev, &irq_val);
			if (irq_val == 0)
				return;
			idx = irq_val;
			zone = bcl_dev->zone[idx];
		}

		if (zone->irq_type == IF_PMIC) {
			bcl_cb_get_irq(bcl_dev, &irq_val);
			bcl_cb_clr_irq(bcl_dev, idx);
		}

		if (gpio_get_value(zone->bcl_pin) == zone->polarity) {
			if (idx >= UVLO1 && idx <= BATOILO2) {
				atomic_inc(&zone->last_triggered.triggered_cnt[START]);
				zone->last_triggered.triggered_time[START] =
						ktime_to_ms(ktime_get());
			}
		} else {
			google_bcl_release_throttling(zone);
			return;
		}
	}
	if (zone->bcl_qos) {
		google_bcl_qos_update(zone, QOS_LIGHT);
		mod_delayed_work(bcl_dev->qos_update_wq, &zone->warn_work,
				 msecs_to_jiffies(TIMEOUT_5MS));
	}

	google_bcl_start_data_logging(bcl_dev, idx);

	/* LIGHT phase */
	google_bcl_upstream_state(zone, LIGHT);

	if (bcl_dev->batt_psy_initialized) {
		if (idx == BATOILO2 || idx == UVLO2) {
			if (irq_val != 0) {
				atomic_inc(&bcl_dev->zone[irq_val]->bcl_cnt);
				ocpsmpl_read_stats(bcl_dev, &bcl_dev->zone[irq_val]->bcl_stats,
						   bcl_dev->batt_psy);
			}
		} else {
			atomic_inc(&zone->bcl_cnt);
			ocpsmpl_read_stats(bcl_dev, &zone->bcl_stats, bcl_dev->batt_psy);
		}
	}

	idx = zone->idx;
	bcl_dev = zone->parent;
	trace_bcl_zone_stats(zone, 1);

	if (zone->irq_type == IF_PMIC) {
		update_irq_start_times(bcl_dev, idx);
		if (idx == BATOILO)
			google_bcl_set_batfet_timer(bcl_dev);
	}

	if (google_bcl_wait_for_response_locked(zone, TIMEOUT_5MS) > 0)
		return;
	google_bcl_upstream_state(zone, MEDIUM);

	/* MEDIUM phase: b/300504518 */
	if (google_bcl_wait_for_response_locked(zone, TIMEOUT_5MS) > 0)
		return;
	google_bcl_upstream_state(zone, HEAVY);
	/* We most likely have to shutdown after this */

	/* Reset Mitigation module if we are still alive */
	atomic_set(&bcl_dev->mitigation_module_ids, 0);

	/* HEAVY phase */
	/* IRQ deasserted */
}

static irqreturn_t vdroop_irq_thread_fn(int irq, void *data)
{
	struct bcl_device *bcl_dev = data;
	struct bcl_zone *zone = NULL;

	if (IS_ERR_OR_NULL(bcl_dev))
		return IRQ_HANDLED;
	bcl_cb_clr_irq(bcl_dev, BATOILO);

	/* Ensure sw mitigation enabled is read correctly */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled))
		return IRQ_HANDLED;

	/* This is only BATOILO */
	zone = bcl_dev->zone[BATOILO];
	if (zone) {
		atomic_inc(&zone->last_triggered.triggered_cnt[START]);
		zone->last_triggered.triggered_time[START] = ktime_to_ms(ktime_get());
		queue_work(system_unbound_wq, &zone->irq_triggered_work);
	}

	return IRQ_HANDLED;
}

int google_bcl_register_zone(struct bcl_device *bcl_dev, int idx, const char *devname,
				    int pin, int irq, int type, int irq_config, int polarity,
				    u32 flag)
{
	int ret = 0;
	struct bcl_zone *zone;

	if ((irq_config == IRQ_EXIST) && (pin < 0 || irq < 0)) {
		dev_err(bcl_dev->device, "Failed to register zone %s, pin error, pin:%d, irq:%d\n",
			devname, pin, irq);
		return -EINVAL;
	}
	zone = devm_kzalloc(bcl_dev->device, sizeof(struct bcl_zone), GFP_KERNEL);

	if (!zone)
		return -ENOMEM;

	init_completion(&zone->deassert);
	zone->idx = idx;
	zone->bcl_pin = pin;
	zone->bcl_irq = irq;
	zone->has_irq = irq_config;
	zone->bcl_cur_lvl = 0;
	zone->bcl_prev_lvl = 0;
	zone->bcl_lvl = 0;
	zone->parent = bcl_dev;
	zone->irq_type = type;
	zone->devname = devname;
	zone->disabled = true;
	zone->device = bcl_dev->device;
	zone->polarity = polarity;
	atomic_set(&zone->bcl_cnt, 0);
	atomic_set(&zone->last_triggered.triggered_cnt[START], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[LIGHT], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[MEDIUM], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[HEAVY], 0);

	INIT_WORK(&zone->irq_triggered_work, google_irq_triggered_work);
	INIT_DELAYED_WORK(&zone->warn_work, google_warn_work);

	if ((irq_config == IRQ_EXIST) && (zone->bcl_pin == NOT_USED) && !zone->irq_reg) {
		ret = devm_request_threaded_irq(bcl_dev->device, bcl_dev->pmic_irq, NULL,
						vdroop_irq_thread_fn,
						IRQF_TRIGGER_FALLING | IRQF_SHARED |
						IRQF_ONESHOT | IRQF_NO_THREAD, devname,
						bcl_dev);
		if (ret < 0) {
			dev_err(zone->device, "Failed to request l-IRQ: %d: %d\n",
				bcl_dev->pmic_irq, ret);
			devm_kfree(bcl_dev->device, zone);
			return ret;
		}
		zone->bcl_irq = bcl_dev->pmic_irq;
		zone->irq_reg = true;
		zone->disabled = false;
	}
	if ((irq_config == IRQ_EXIST) && (zone->bcl_pin != NOT_USED) && !zone->irq_reg) {
		ret = devm_request_threaded_irq(bcl_dev->device, zone->bcl_irq, NULL,
						latched_irq_handler, flag,
						devname, zone);

		if (ret < 0) {
			dev_err(zone->device, "Failed to request IRQ: %d: %d\n", zone->bcl_irq,
				ret);
			devm_kfree(bcl_dev->device, zone);
			return ret;
		}
		zone->irq_reg = true;
	}
	bcl_dev->zone[idx] = zone;
	return ret;
}

static int google_set_intf_pmic(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	return ifpmic_setup(bcl_dev, pdev);
}

static void irq_config(struct bcl_zone *zone, bool enabled)
{
	if (!zone)
		return;
	if (!enabled)
		zone->disabled = true;
	else if (enabled && zone->disabled && zone->irq_reg) {
		zone->disabled = false;
		if (zone->bcl_pin != NOT_USED)
			enable_irq(zone->bcl_irq);
	}

}

static void google_bcl_parse_irq_config(struct bcl_device *bcl_dev)
{
	struct device_node *np = bcl_dev->device->of_node;
	struct device_node *child;
        /* irq config */
	child = of_get_child_by_name(np, "irq_config");
	if (!child)
		return;
	irq_config(bcl_dev->zone[UVLO1], of_property_read_bool(child, "irq,uvlo1"));
	irq_config(bcl_dev->zone[UVLO2], of_property_read_bool(child, "irq,uvlo2"));
	/* This enables BATOILO2 as well */
	if (bcl_dev->ifpmic == MAX77779)
		irq_config(bcl_dev->zone[BATOILO2], of_property_read_bool(child, "irq,batoilo2"));
	if (IS_ENABLED(CONFIG_GOOGLE_MFD_DA9188))
		irq_config(bcl_dev->zone[PRE_UVLO], of_property_read_bool(child, "irq,pre_uvlo"));
	else
		irq_config(bcl_dev->zone[PRE_UVLO], of_property_read_bool(child, "irq,smpl_warn"));
	if (bcl_dev->ifpmic == MAX77779)
		return;
	irq_config(bcl_dev->zone[BATOILO], of_property_read_bool(child, "irq,batoilo"));
	irq_config(bcl_dev->zone[PRE_OCP_CPU1], of_property_read_bool(child, "irq,ocp_cpu1"));
	irq_config(bcl_dev->zone[PRE_OCP_CPU2], of_property_read_bool(child, "irq,ocp_cpu2"));
	irq_config(bcl_dev->zone[PRE_OCP_TPU], of_property_read_bool(child, "irq,ocp_tpu"));
	irq_config(bcl_dev->zone[PRE_OCP_GPU], of_property_read_bool(child, "irq,ocp_gpu"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_CPU1],
		   of_property_read_bool(child, "irq,soft_ocp_cpu1"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_CPU2],
		   of_property_read_bool(child, "irq,soft_ocp_cpu2"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_TPU],
		   of_property_read_bool(child, "irq,soft_ocp_tpu"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_GPU],
		   of_property_read_bool(child, "irq,soft_ocp_gpu"));
}

static void google_bcl_init_power_supply(struct bcl_device *bcl_dev)
{
	int ret;

	INIT_DELAYED_WORK(&bcl_dev->soc_work, google_bcl_evaluate_soc);
	bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	bcl_dev->batt_psy_initialized = false;
	bcl_dev->psy_nb.notifier_call = battery_supply_callback;
	ret = power_supply_reg_notifier(&bcl_dev->psy_nb);
	if (ret < 0)
		dev_err(bcl_dev->device, "soc notifier registration error. defer. err:%d\n", ret);
	else
		bcl_dev->batt_psy_initialized = true;
	bcl_dev->soc_tz_ops.get_temp = tz_bcl_read_soc;
	bcl_dev->soc_tz_ops.set_trips = tz_bcl_set_soc;
	bcl_dev->soc_tz = devm_thermal_of_zone_register(bcl_dev->device, PMIC_SOC, bcl_dev,
							&bcl_dev->soc_tz_ops);
	if (IS_ERR(bcl_dev->soc_tz)) {
		dev_err(bcl_dev->device, "soc TZ register failed. err:%ld\n",
			PTR_ERR(bcl_dev->soc_tz));
		ret = PTR_ERR(bcl_dev->soc_tz);
		bcl_dev->soc_tz = NULL;
	} else
		thermal_zone_device_update(bcl_dev->soc_tz, THERMAL_DEVICE_UP);
}

static int google_bcl_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct bcl_device *bcl_dev;

	bcl_dev = devm_kzalloc(&pdev->dev, sizeof(*bcl_dev), GFP_KERNEL);
	if (IS_ERR_OR_NULL(bcl_dev))
		return -ENOMEM;

	mutex_init(&bcl_dev->sysreg_lock);
	bcl_dev->device = &pdev->dev;

	ret = ifpmic_setup_dev(bcl_dev);
	if (ret == -EPROBE_DEFER) {
		dev_err(bcl_dev->device, "Setting up IFPMIC again\n");
		return ret;
	} else if (ret == -ENODEV) {
		dev_err(bcl_dev->device, "IFPMIC charger not found\n");
		goto bcl_soc_probe_exit;
	}
	platform_set_drvdata(pdev, bcl_dev);
	google_bcl_init_power_supply(bcl_dev);

	google_bcl_parse_clk_div_dtree(bcl_dev);
	ret = google_bcl_init_instruction(bcl_dev);
	if (ret < 0)
		goto bcl_soc_probe_exit;

	if (google_bcl_setup_mailbox(bcl_dev) < 0)
		goto bcl_soc_probe_exit;

	core_pmic_parse_dtree(bcl_dev);
	if (core_pmic_main_setup(bcl_dev, pdev) < 0)
		goto bcl_soc_probe_exit;
	if (core_pmic_sub_setup(bcl_dev) < 0)
		goto bcl_soc_probe_exit;
	google_bcl_configure_modem(bcl_dev);

	if (google_set_intf_pmic(bcl_dev, pdev) < 0)
		goto bcl_soc_probe_exit;

	if (google_bcl_parse_qos(bcl_dev) != 0) {
		dev_err(bcl_dev->device, "Cannot parse QOS\n");
		goto bcl_soc_probe_exit;
	}

	if (google_bcl_setup_qos(bcl_dev) != 0) {
		dev_err(bcl_dev->device, "Cannot Initiate QOS\n");
		goto bcl_soc_probe_exit;
	}
	google_init_debugfs(bcl_dev);
	ret = google_bcl_init_data_logging(bcl_dev);
	if (ret < 0)
		goto bcl_soc_probe_exit;
	/* br_stats no need to run without mitigation app */
	bcl_dev->enabled_br_stats = false;
	bcl_dev->triggered_idx = TRIGGERED_SOURCE_MAX;
	ret = ifpmic_init_fs(bcl_dev);
	if (ret < 0)
		goto debug_fs_removal;
	ret = google_bcl_init_notifier(bcl_dev);
	if (ret < 0)
		goto debug_init_fs;
	google_bcl_setup_votable(bcl_dev);
	google_bcl_clk_div(bcl_dev);
	google_bcl_parse_irq_config(bcl_dev);

	/* Ensure sw mitigation enabled is correctly set */
	smp_store_release(&bcl_dev->sw_mitigation_enabled, true);

	/* Ensure hw mitigation enabled is correctly set */
	smp_store_release(&bcl_dev->hw_mitigation_enabled, true);

	/* Ensure bcl driver is initialized to avoid receiving external calls */
	smp_store_release(&bcl_dev->initialized, true);

	core_pmic_get_cpm_cached_sys_evt(bcl_dev);
	dev_info(bcl_dev->device, "BCL done\n");

	return 0;

debug_init_fs:
	ifpmic_destroy_fs(bcl_dev);
debug_fs_removal:
	debugfs_remove_recursive(bcl_dev->debug_entry);
bcl_soc_probe_exit:
	google_bcl_remove_thermal(bcl_dev);
	dev_err(bcl_dev->device, "BCL SW disabled.  Revert to HW mitigation\n");
	return 0;
}

static int google_bcl_remove(struct platform_device *pdev)
{
	struct bcl_device *bcl_dev = platform_get_drvdata(pdev);

	ifpmic_destroy_fs(bcl_dev);
	debugfs_remove_recursive(bcl_dev->debug_entry);
	cpu_pm_unregister_notifier(&bcl_dev->cpu_nb);
	google_bcl_remove_thermal(bcl_dev);

	return 0;
}

static void google_bcl_shutdown(struct platform_device *pdev)
{
	struct bcl_device *bcl_dev = platform_get_drvdata(pdev);

	if (bcl_dev)
		power_supply_unreg_notifier(&bcl_dev->psy_nb);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "google,google-bcl"},
	{},
};

static struct platform_driver google_bcl_driver = {
	.probe  = google_bcl_probe,
	.remove = google_bcl_remove,
	.shutdown = google_bcl_shutdown,
	.id_table = google_id_table,
	.driver = {
		.name           = "google_mitigation",
		.owner          = THIS_MODULE,
		.of_match_table = match_table,
	},
};

module_platform_driver(google_bcl_driver);

MODULE_SOFTDEP("pre: i2c-acpm");
MODULE_DESCRIPTION("Google Battery Current Limiter");
MODULE_AUTHOR("George Lee <geolee@google.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION(BCL_VERSION);
