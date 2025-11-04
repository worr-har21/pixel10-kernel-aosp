// SPDX-License-Identifier: GPL-2.0
/*
 * thermal_tmu_helper.c Helper for TMU functionality.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */

#include "thermal_tmu_debugfs_helper.h"
#include "thermal_tmu_helper.h"
#include "thermal_tmu_mock.h"
#include "thermal_zone_helper.h"

#define DEFAULT_POLLING_DELAY		(50)
#define DEFAULT_PID_K_PO		(60)
#define DEFAULT_PID_K_PU		(60)
#define DEFAULT_PID_K_I			(50)
#define DEFAULT_MPMM_CLR_THROTTLE_LEVEL	(2)
#define DEFAULT_MPMM_THROTTLE_LEVEL	(0)
#define DEFAULT_GRADUAL_IRQ_GAIN	(2)
#define DEFAULT_GRADUAL_TIMER_GAIN	(1)
#define DEFAULT_EARLY_THROTTLE_K_P	(50)
#define DEFAULT_EARLY_THROTTLE_OFFSET	(10)

static struct list_head *tmu_sensor_list;
static bool is_offset_enabled;
DEFINE_MUTEX(offset_lock);
DEFINE_MUTEX(trip_counter_lock);

int __thermal_tmu_read_dt_property_with_default(struct google_sensor_data *sensor,
						struct device_node *np, char *name,
						int *val, int default_value)
{
	int ret;

	if (!sensor || !np || !name || !val)
		return -EINVAL;

	ret = read_dt_property_s32(np, name, val);
	if (ret) {
		dev_dbg(sensor->dev,
			"tz: %s, property not found: %s ret: %d, use default value: %d\n",
			thermal_zone_device_type(sensor->tzd), name, ret, default_value);
		*val = default_value;
	}
	return 0;
}

int __thermal_tmu_parse_gradual_param_dt(struct google_sensor_data *sensor,
					 struct device_node *tmu_np)
{
	int ret, irq_gain, timer_gain;
	struct tmu_sensor_data *tmu_data = NULL;

	if (!sensor || !tmu_np)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "irq-gradual-gain",
							  &irq_gain, DEFAULT_GRADUAL_IRQ_GAIN);
	if (ret)
		return ret;

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "timer-gradual-gain",
							  &timer_gain, DEFAULT_GRADUAL_TIMER_GAIN);
	if (ret)
		return ret;

	tmu_data->gradual_param.irq_gain = irq_gain;
	tmu_data->gradual_param.timer_gain = timer_gain;

	return 0;
}

int __thermal_tmu_parse_pi_param_dt(struct google_sensor_data *sensor,
				    struct device_node *tmu_np)
{
	int ret, k_po, k_pu, k_i;
	struct tmu_sensor_data *tmu_data = NULL;

	if (!sensor || !tmu_np)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	if (read_dt_property_bool(tmu_np, "pi-enable"))
		set_bit(TMU_GOV_PI_SELECT, &tmu_data->gov_select);

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "k-po", &k_po,
							  DEFAULT_PID_K_PO);
	if (ret)
		return ret;

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "k-pu", &k_pu,
							  DEFAULT_PID_K_PU);
	if (ret)
		return ret;

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "k-i", &k_i,
							  DEFAULT_PID_K_I);
	if (ret)
		return ret;

	tmu_data->pi_param.k_po = k_po;
	tmu_data->pi_param.k_pu = k_pu;
	tmu_data->pi_param.k_i = k_i;

	return 0;
}

int __thermal_tmu_parse_early_throttle_param_dt(struct google_sensor_data *sensor,
						struct device_node *tmu_np)
{
	int ret, k_p, offset;
	struct tmu_sensor_data *tmu_data = NULL;

	if (!sensor || !tmu_np)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	if (read_dt_property_bool(tmu_np, "early-throttle-enable"))
		set_bit(TMU_GOV_EARLY_THROTTLE_SELECT, &tmu_data->gov_select);

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "early-throttle-k-p",
							  &k_p, DEFAULT_EARLY_THROTTLE_K_P);
	if (ret)
		return ret;

	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "early-throttle-offset",
							  &offset, DEFAULT_EARLY_THROTTLE_OFFSET);
	if (ret)
		return ret;

	tmu_data->early_throttle_param.k_p = k_p;
	tmu_data->early_throttle_param.offset = offset;

	return 0;
}

/*
 * __thermal_tmu_parse_tzd_dt - parse tzd device tree configuration
 *
 * TMU configuration is moved into thermal zone device tree. This function is
 * used to fetch the data from thermal zone of_node and configure the TMU
 * accordingly.
 *
 * @sensor: TMU data structure
 *
 * Return: 0 on success.
 *	- EINVAL if ther input structure is not valid.
 */
int __thermal_tmu_parse_tzd_dt(struct google_sensor_data *sensor)
{
	struct device *dev = sensor->dev;
	struct device_node *tmu_np = NULL;
	struct tmu_sensor_data *tmu_data = NULL;
	int ret = 0, delay;
	char *tmu_dt_path;

	if (!sensor)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	tmu_dt_path = kasprintf(GFP_KERNEL, "/thermal-zones/%s/tmu",
				thermal_zone_device_type(sensor->tzd));
	if (!tmu_dt_path)
		return -ENOMEM;

	tmu_np = of_find_node_by_path(tmu_dt_path);
	kfree(tmu_dt_path);
	if (IS_ERR_OR_NULL(tmu_np)) {
		dev_err(dev,
			"No TMU section found in device tree: tz: %s",
			thermal_zone_device_type(sensor->tzd));
		return -ENODEV;
	}

	/* polling delay */
	ret = __thermal_tmu_read_dt_property_with_default(sensor, tmu_np, "polling-delay-on",
							  &delay, DEFAULT_POLLING_DELAY);
	if (ret)
		goto exit;
	tmu_data->polling_delay_ms = delay;

	/* parse governor parameters */
	ret = __thermal_tmu_parse_gradual_param_dt(sensor, tmu_np);
	if (ret)
		goto exit;

	ret = __thermal_tmu_parse_pi_param_dt(sensor, tmu_np);
	if (ret)
		goto exit;

	ret = __thermal_tmu_parse_early_throttle_param_dt(sensor, tmu_np);
	if (ret)
		goto exit;

	/* hardlimit via PID */
	if (of_property_read_bool(tmu_np, "hardlimit-via-pid-enable"))
		set_bit(TMU_GOV_HARDLIMIT_VIA_PID_SELECT, &tmu_data->gov_select);

	/* Tj offset */
	ret = of_property_read_u32_array(tmu_np, "junction-offset",
					(u32 *)tmu_data->junction_offset, THERMAL_TMU_NR_TRIPS);

	tmu_data->skip_junction_offset = true;
	if (ret) {
		dev_warn(dev, "Invalid junction_offset table of tz: %s, ret=%d",
			 thermal_zone_device_type(sensor->tzd), ret);
		ret = 0;
	} else {
		for (int i = 0; i < THERMAL_TMU_NR_TRIPS; i++) {
			dev_dbg(dev, "junction_offset[%d] = %d\n", i, tmu_data->junction_offset[i]);
			if (tmu_data->junction_offset[i] != 0)
				tmu_data->skip_junction_offset = false;
		}
	}

	/* temp LUT */
	if (of_property_read_bool(tmu_np, "temp-lut-enable"))
		set_bit(TMU_GOV_TEMP_LUT_SELECT, &tmu_data->gov_select);

exit:
	of_node_put(tmu_np);
	return ret;
}

/*
 * __thermal_tmu_gov_gradual_init - initialize gradual governor
 *
 * This function sends messages to initialize TMU gradual governor parameters.
 *
 * @dev: pointer to the thermal sensor device which the TMU belongs to
 * @tz_id: id of the TMU tz to init
 * @data: data structure of the TMU configuration
 *
 * Return: 0 on success
 */
int __thermal_tmu_gov_gradual_init(struct device *dev, u8 tz_id, struct tmu_sensor_data *data)
{
	int ret;

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_IRQ_GAIN, data->gradual_param.irq_gain);
	if (ret) {
		dev_err(dev,
			"Failed to message gradual irq_gain to %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_TIMER_GAIN, data->gradual_param.timer_gain);
	if (ret) {
		dev_err(dev,
			"Failed to message gradual timer_gain to %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	return 0;
}

/*
 * __thermal_tmu_gov_pi_init - initialize pi governor
 *
 * This function sends messages to initialize TMU pi governor parameters.
 *
 * @dev: pointer to the thermal sensor device which the TMU belongs to
 * @tz_id: id of the TMU tz to init
 * @data: data structure of the TMU configuration
 *
 * Return: 0 on success
 */
int __thermal_tmu_gov_pi_init(struct device *dev, u8 tz_id, struct tmu_sensor_data *data)
{
	int ret;

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_K_PO, data->pi_param.k_po);
	if (ret) {
		dev_err(dev,
			"Failed to message PI k_po to tz %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_K_PU, data->pi_param.k_pu);
	if (ret) {
		dev_err(dev,
			"Failed to message PI k_pu to tz %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_K_I, data->pi_param.k_i);
	if (ret) {
		dev_err(dev,
			"Failed to message PI k_i to tz %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	return 0;
}

/*
 * __thermal_tmu_gov_early_throttle_init - initialize early throttle governor
 *
 * This function sends messages to initialize TMU early throttle governor parameters.
 *
 * @dev: pointer to the thermal sensor device which the TMU belongs to
 * @data: data structure of the TMU configuration
 *
 * Return: 0 on success
 */
int __thermal_tmu_gov_early_throttle_init(struct device *dev, u8 tz_id,
					  struct tmu_sensor_data *data)
{
	int ret;

	ret = tmu_set_gov_param(tz_id, TMU_GOV_PARAM_EARLY_THROTTLE_K_P,
				data->early_throttle_param.k_p);
	if (ret) {
		dev_err(dev,
			"Failed to message early throttle k_p to tz %u: ret=%d",
			tz_id, ret);
		return ret;
	}

	return 0;
}

/*
 * __thermal_tmu_trip_points_init - initialize TMU trip points with tzd dt config
 *
 * @sensor: pointer to the TMU sensor data structure
 *
 * Return: 0 on success
 *	   -EINVAL: NULL sensor
 *	   other error code from message helper APIs
 */
int __thermal_tmu_trip_points_init(struct google_sensor_data *sensor)
{
	struct thermal_zone_device *tzd = NULL;
	struct device *dev = NULL;
	u8 tz_id;
	u8 trip_temps[THERMAL_TMU_NR_TRIPS] = {0},
	   trip_hysts[THERMAL_TMU_NR_TRIPS] = {0},
	   trip_types[THERMAL_TMU_NR_TRIPS] = {0};
	struct thermal_trip trip;
	int tz_trip_count, ret;

	if (!sensor)
		return -EINVAL;

	tzd = sensor->tzd;
	dev = sensor->dev;
	tz_id = (u8)sensor->sensor_id;

	tz_trip_count = thermal_zone_get_num_trips(tzd);
	if (tz_trip_count != THERMAL_TMU_NR_TRIPS) {
		dev_err(dev,
			"%s: %d trips found in device tree but %d are expected",
			thermal_zone_device_type(tzd), tz_trip_count, THERMAL_TMU_NR_TRIPS);
		return -EINVAL;
	}
	for (int i = 0; i < tz_trip_count; ++i) {
		ret = get_thermal_zone_trip(tzd, i, &trip);
		if (ret) {
			dev_err(dev, "Failed to set trip %d from tz: %s: ret=%d", i,
				thermal_zone_device_type(tzd), ret);
			return ret;
		}
		trip_temps[i] = (u8)(trip.temperature / MILLI);
		trip_hysts[i] = (u8)(trip.hysteresis / MILLI);
		switch (trip.type) {
		case THERMAL_TRIP_ACTIVE:
			trip_types[i] = (u8)TMU_TRIP_TYPE_ACTIVE;
			break;
		case THERMAL_TRIP_PASSIVE:
			trip_types[i] = (u8)TMU_TRIP_TYPE_PASSIVE;
			break;
		case THERMAL_TRIP_HOT:
			trip_types[i] = (u8)TMU_TRIP_TYPE_HOT;
			break;
		case THERMAL_TRIP_CRITICAL:
			trip_types[i] = (u8)TMU_TRIP_TYPE_CRITICAL;
			break;
		default:
			dev_err(dev, "tz %s: unknown thermal trip for tmu: %d",
				thermal_zone_device_type(tzd), trip.type);
			break;
		}
	}
	ret = tmu_set_trip_temp(tz_id, trip_temps, THERMAL_TMU_NR_TRIPS);
	if (ret) {
		dev_err(dev, "tz %s: Failed to set trip temperature. ret:%d\n",
			thermal_zone_device_type(tzd), ret);
		return ret;
	}
	ret = tmu_set_trip_hyst(tz_id, trip_hysts, THERMAL_TMU_NR_TRIPS);
	if (ret) {
		dev_err(dev, "tz %s: Failed to set trip hysteresis. ret:%d\n",
			thermal_zone_device_type(tzd), ret);
		return ret;
	}
	ret = tmu_set_trip_type(tz_id, trip_types, THERMAL_TMU_NR_TRIPS);
	if (ret) {
		dev_err(dev, "tz %s: Failed to set trip type. ret:%d\n",
			thermal_zone_device_type(tzd), ret);
		return ret;
	}
	return 0;
}

/*
 * thermal_tmu_set_polling_delay_ms - set polling delay (ms) to tmu
 *
 * @sensor: pointer to google sensor data structure of the TMU
 * @val: polling delay (ms) to set
 *
 * Return: 0 on success.
 *	   -EINVAL: NULL sensor
 *	   other error code from message API
 */
int thermal_tmu_set_polling_delay_ms(struct google_sensor_data *sensor, u32 val)
{
	u8 tz_id;
	struct tmu_sensor_data *tmu_data = NULL;
	struct device *dev = NULL;
	struct thermal_zone_device *tzd = NULL;
	int ret, delay_ms = val;

	if (!sensor)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	tz_id = (u8)sensor->sensor_id;
	dev = sensor->dev;
	tzd = sensor->tzd;

	if (val > U16_MAX) {
		dev_warn(dev, "Polling delay set to tz: %s: clamped to %d",
			 thermal_zone_device_type(tzd), U16_MAX);
		delay_ms = clamp_val(delay_ms, 0, U16_MAX);
	}

	ret = tmu_set_polling_delay_ms(tz_id, delay_ms);
	if (ret) {
		dev_err(dev, "Failed to set polling delay to tz: %s: ret=%d",
			thermal_zone_device_type(tzd), ret);
		return ret;
	}
	tmu_data->polling_delay_ms = delay_ms;
	return 0;
}

/*
 * thermal_tmu_get_polling_delay_ms - get polling delay (ms) from TMU sensor data structure
 *
 * @sensor: pointer to google sensor data structure of the TMU
 * @val: polling delay (ms) output
 *
 * Return: 0 on success.
 *	   -EINVAL: NULL sensor
 */
int thermal_tmu_get_polling_delay_ms(struct google_sensor_data *sensor, u32 *val)
{
	struct tmu_sensor_data *tmu_data = NULL;

	if (!sensor || !val)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	*val = tmu_data->polling_delay_ms;
	return 0;
}

/*
 * __thermal_tmu_send_init_msg - send messages to initialize TMU parameters
 *
 * This function sends messages to initialize TMU parameters.
 *
 * @sensor: pointer to google sensor data structure of the TMU
 *
 * Return: 0 on success
 */
int __thermal_tmu_send_init_msg(struct google_sensor_data *sensor)
{
	u8 tz_id;
	int ret = 0;
	struct device *dev = NULL;
	struct tmu_sensor_data *tmu_data = NULL;

	if (!sensor)
		return -EINVAL;

	tmu_data = sensor->sens_priv_data;
	if (!tmu_data)
		return -EINVAL;

	tz_id = (u8)sensor->sensor_id;
	dev = sensor->dev;

	ret = thermal_tmu_set_polling_delay_ms(sensor, tmu_data->polling_delay_ms);
	if (ret) {
		dev_err(dev,
			"Failed to message polling_delay_ms to tz: %s: ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}

	/* send gradual parameters */
	ret = __thermal_tmu_gov_gradual_init(dev, tz_id, tmu_data);
	if (ret)
		return ret;

	/* send PI loop parameters */
	ret = __thermal_tmu_gov_pi_init(dev, tz_id, tmu_data);
	if (ret)
		return ret;

	/* send early throttle parameters */
	ret = __thermal_tmu_gov_early_throttle_init(dev, tz_id, tmu_data);
	if (ret)
		return ret;

	/* send governor select */
	ret = tmu_set_gov_select(tz_id, (u8)tmu_data->gov_select);
	if (ret) {
		dev_err(dev,
			"Failed to message gov_select to tz: %s: ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}

	return 0;
}

/*
 * Sysfs Node
 */

/*
 * offset_enable: enable/disable trip temperature offset of a TMU
 */
static ssize_t offset_enable_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int ret;

	mutex_lock(&offset_lock);
	ret = sysfs_emit(buf, "%d\n", is_offset_enabled);
	mutex_unlock(&offset_lock);

	return ret;
}

static ssize_t offset_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t count)
{
	bool offset_enable;
	int ret;

	if (kstrtobool(buf, &offset_enable))
		return -EINVAL;

	ret = __thermal_tmu_offset_enable(offset_enable);
	if (ret)
		dev_err(dev, "Failed to update offset enable: ret=%d", ret);

	return count;
}

DEVICE_ATTR_RW(offset_enable);

/*
 * trip_counter
 */
static ssize_t trip_counter_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct thermal_sm_trip_counter_data trip_counter_data;
	int ret, len = 0;

	ret = __thermal_tmu_get_trip_counter(attr, &trip_counter_data);
	if (ret)
		return ret;

	for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i)
		len += sysfs_emit_at(buf, len, "%llu ", trip_counter_data.counters[i]);

	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

/*
 * thermal_tmu_configure - Initialize global configuration of TMU
 *
 * @pdev: The platform device
 * @sensor_ct: Maximum number of potentially supported sensor
 *
 * Return: 0 on success;
 *	-EINVAL: invalid input;
 *	error returned from tmu_debugfs_init
 */
int thermal_tmu_configure(struct platform_device *pdev, int sensor_ct)
{
	struct device *dev;
	int ret;

	if (!pdev)
		return -EINVAL;

	dev = &pdev->dev;
	ret = thermal_tmu_debugfs_init(dev);
	if (ret)
		dev_dbg(dev, "Failed to initialize debugfs: ret=%d", ret);

	ret = device_create_file(dev, &dev_attr_offset_enable);
	if (ret) {
		dev_err(dev, "Failed to create file node for offset_enable: ret=%d", ret);
		return ret;
	}

	return 0;
}

/*
 * thermal_tmu_post_configure
 *
 * Post configuration tasks:
 * - acquire google thermal sensor list from framework
 *
 * @sensor_list: list head of google thermal sensor list
 *
 * Return: 0 on success;
 *	-EINVAL: invalid argument
 */
int thermal_tmu_post_configure(struct list_head *sensor_list)
{
	if (!sensor_list)
		return -EINVAL;

	tmu_sensor_list = sensor_list;
	return 0;
}



/*
 * thermal_tmu_cleanup - cleanup global configuration of TMU
 *
 * @pdev: the platform device
 */
void thermal_tmu_cleanup(struct platform_device *pdev)
{
	thermal_tmu_debugfs_cleanup();
	device_remove_file(&pdev->dev, &dev_attr_offset_enable);
}

/*
 * thermal_tmu_configure_sensor - configure tmu after sensor registered
 *
 * This function initializes the tmu after being registered as a sensor to a
 * thermal zone with thermal zone device tree configurations.
 *
 * @sensor: registered google sensor data
 */
int thermal_tmu_configure_sensor(struct google_sensor_data *sensor)
{
	struct device *dev = NULL;
	struct tmu_sensor_data *tmu_data = NULL;
	int ret = 0;

	if (!sensor)
		return -EINVAL;

	dev = sensor->dev;
	tmu_data = devm_kzalloc(dev, sizeof(*tmu_data), GFP_KERNEL);
	if (!tmu_data) {
		dev_err(dev, "tz:%s : failed to allocate tmu\n",
			thermal_zone_device_type(sensor->tzd));
		return -ENOMEM;
	}
	sensor->sens_priv_data = (void *)tmu_data;

	/* read from device tree and update sensor->sens_data.tmu_data*/
	ret = thermal_tmu_parse_tzd_dt(sensor);
	if (ret) {
		dev_err(dev, "Fail to parse device tree for tz: %s ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}

	/* if no governor is selected, then select gradual as default */
	if (!test_bit(TMU_GOV_GRADUAL_SELECT, &tmu_data->gov_select) &&
	    !test_bit(TMU_GOV_PI_SELECT, &tmu_data->gov_select) &&
	    !test_bit(TMU_GOV_TEMP_LUT_SELECT, &tmu_data->gov_select)) {
		set_bit(TMU_GOV_GRADUAL_SELECT, &tmu_data->gov_select);
		dev_dbg(dev, "No TMU governor selected in config of tz: %s",
			thermal_zone_device_type(sensor->tzd));
	}

	/* set data to cpm */
	ret = __thermal_tmu_send_init_msg(sensor);
	if (ret) {
		dev_err(dev, "Fail to send init messages for tz: %s ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}

	/* init trip points*/
	ret = __thermal_tmu_trip_points_init(sensor);
	if (ret) {
		dev_err(dev, "Fail to init trip points for tz: %s ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}
	ret = __thermal_tmu_trip_counter_init(dev, sensor);
	if (ret)
		dev_warn(dev, "Unable to init trip point counters for tz: %s ret=%d, continue...",
			 thermal_zone_device_type(sensor->tzd), ret);

	/* init debugfs */
	ret = thermal_tmu_debugfs_sensor_init(sensor);
	if (ret)
		dev_dbg(dev, "Unable to create debugfs for tz: %s ret=%d", sensor->tzd->type, ret);

	return 0;
}

/*
 * thermal_tmu_cleanup_sensor - cleanup tmu before unregistering the sensor
 *
 * @sensor: registered google sensor data
 *
 * Return: 0 on success
 */
int thermal_tmu_cleanup_sensor(struct google_sensor_data *sensor)
{
	struct device *dev = sensor->dev;
	struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;

	device_remove_file(dev, &tmu_data->dev_attr_trip_counter);
	return 0;
}

/*
 * thermal_tmu_get_temp - thermal zone device ops callback
 *
 * This function is registered to a thermal zone for temperature query.
 * When the thermal zone SSWRP is off, temperature sensor will not be available.
 * This function will output "THERMAL_TEMP_INVALID" (-274000) to indicate this
 * condition.
 *
 * @tzd: thermal zone device pointer
 * @temp: tmu thermal zone temperature
 *
 * Return: 0 on success.
 */
int thermal_tmu_get_temp(struct thermal_zone_device *tzd, int *temp)
{
	struct google_sensor_data *sens = thermal_zone_device_priv(tzd);
	u8 tz_id = (u8)sens->sensor_id, temp_c;
	int ret = 0;

	ret = tmu_get_temp(tz_id, &temp_c);
	if (ret) {
		if (ret == -ENODATA) {
			ret = 0;
			*temp = THERMAL_TEMP_INVALID;
		}
		return ret;
	}

	*temp = (int)temp_c * MILLI;
	return 0;
}

/*
 * __thermal_tmu_set_trip_temp - thermal zone device ops callback
 *
 * This function is registered to a thermal zone for updating trip temperature
 *
 * @tzd: thermal zone device pointer
 * @trip_id: the trip point to set
 * @temp: new trip point temperature
 *
 * Return: 0 on success.
 */
int __thermal_tmu_set_trip_temp(struct thermal_zone_device *tzd, int trip_id, int temp)
{
	struct google_sensor_data *sens = thermal_zone_device_priv(tzd);
	struct device *dev = sens->dev;
	struct thermal_trip trip;
	int ret;
	u8 tz_id = (u8)sens->sensor_id,
	   trip_temps[THERMAL_TMU_NR_TRIPS] = {0};

	if (trip_id < 0 || trip_id >= THERMAL_TMU_NR_TRIPS)
		return -EINVAL;

	for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i) {
		if (i == trip_id)
			trip_temps[i] = (u8)(temp / MILLI);
		else {
			ret = get_thermal_zone_trip(tzd, i, &trip);
			if (ret) {
				dev_err(dev, "TZ: %s: Failed to get trip temp: %d, ret=%d",
					thermal_zone_device_type(tzd), i, ret);
				return ret;
			}
			trip_temps[i] = (u8)(trip.temperature / MILLI);
		}
	}

	return tmu_set_trip_temp(tz_id, trip_temps, THERMAL_TMU_NR_TRIPS);
}

int thermal_tmu_set_trip_temp(struct thermal_zone_device *tzd, int trip_id, int temp)
{
	/* Enable changing the trip temperature only for userdebug build. */
#if IS_ENABLED(CONFIG_DEBUG_FS)
	return __thermal_tmu_set_trip_temp(tzd, trip_id, temp);
#else
	return 0;
#endif
}

/*
 * __thermal_tmu_set_trip_hyst - thermal zone device ops callback
 *
 * This function is registered to a thermal zone for updating trip hysteresis
 *
 * @tzd: thermal zone device pointer
 * @trip_id: the trip point to set
 * @hyst: new trip point hysteresis
 *
 * Return: 0 on success.
 */
int __thermal_tmu_set_trip_hyst(struct thermal_zone_device *tzd, int trip_id, int hyst)
{
	struct google_sensor_data *sens = thermal_zone_device_priv(tzd);
	struct device *dev = sens->dev;
	struct thermal_trip trip;
	int ret;
	u8 tz_id = (u8)sens->sensor_id,
	   trip_hysts[THERMAL_TMU_NR_TRIPS] = {0};

	if (trip_id < 0 || trip_id >= THERMAL_TMU_NR_TRIPS)
		return -EINVAL;

	for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i) {
		if (i == trip_id)
			trip_hysts[i] = (u8)(hyst / MILLI);
		else {
			ret = get_thermal_zone_trip(tzd, i, &trip);
			if (ret) {
				dev_err(dev, "TZ: %s: Failed to get trip hyst: %d, ret=%d",
					thermal_zone_device_type(tzd), i, ret);
				return ret;
			}
			trip_hysts[i] = (u8)(trip.hysteresis / MILLI);
		}
	}

	return tmu_set_trip_hyst(tz_id, trip_hysts, THERMAL_TMU_NR_TRIPS);
}

int thermal_tmu_set_trip_hyst(struct thermal_zone_device *tzd, int trip_id, int hyst)
{
	/* Enable changing the trip hysteresis only for userdebug build. */
#if IS_ENABLED(CONFIG_DEBUG_FS)
	return __thermal_tmu_set_trip_hyst(tzd, trip_id, hyst);
#else
	return 0;
#endif
}

/*
 * __thermal_tmu_offset_enable - apply and remove TMU trip temp offsets
 *
 * @sensor: google sensor instance of the TMU to control
 * @enable: true to apply the offsets while false to remove
 *
 * Return: 0 on success
 *	-EINVAL: invalid tmu_sensor_list
 *	other error code returned from thermal zone trip and set trip functions
 */
int __thermal_tmu_offset_enable(bool enable)
{
	struct google_sensor_data *sensor;
	int ret = 0;

	if (!tmu_sensor_list)
		return -EINVAL;

	mutex_lock(&offset_lock);
	if (is_offset_enabled == enable)
		goto unlock;

	list_for_each_entry(sensor, tmu_sensor_list, node) {
		u8 trip_temps[8] = {0};
		struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;

		if (!tmu_data) {
			dev_err(sensor->dev, "TZ: %s: Invalid sensor data",
				thermal_zone_device_type(sensor->tzd));
			continue;
		}
		// If all junction_offsets are 0, skip this sensor
		if (tmu_data->skip_junction_offset)
			continue;

		for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i) {
			struct thermal_trip trip;
			int new_trip_temp;

			ret = get_thermal_zone_trip(sensor->tzd, i, &trip);
			if (ret) {
				dev_err(sensor->dev, "TZ: %s: Failed to get trip temp: %d, ret=%d",
					thermal_zone_device_type(sensor->tzd), i, ret);
				goto unlock;
			}
			if (enable)
				new_trip_temp = trip.temperature + tmu_data->junction_offset[i];
			else
				new_trip_temp = trip.temperature;

			dev_dbg(sensor->dev, "TZ %s: trip_temp_%d change from %d to %d",
				thermal_zone_device_type(sensor->tzd), i, trip.temperature,
				new_trip_temp);

			trip_temps[i] = (u8)(new_trip_temp / MILLI);
		}
		ret = tmu_set_trip_temp((u8)sensor->sensor_id, trip_temps, THERMAL_TMU_NR_TRIPS);
		if (ret) {
			dev_err(sensor->dev, "TZ: %s: Failed to set trip temp offset: ret=%d",
				thermal_zone_device_type(sensor->tzd), ret);
			goto unlock;
		}
		pr_info("TMU TZ %s: Trip offset applied: trip_temp(C): %d %d %d %d %d %d %d %d",
			thermal_zone_device_type(sensor->tzd),
			trip_temps[0], trip_temps[1], trip_temps[2], trip_temps[3],
			trip_temps[4], trip_temps[5], trip_temps[6], trip_temps[7]);
	}
	is_offset_enabled = enable;
unlock:
	mutex_unlock(&offset_lock);
	return ret;
}

/*
 * thermal_tmu_set_junction_offset - set junction offset to TMU data from debugfs node
 *
 * @sensor: google sensor data structure
 * @offsets: junction offset string array input
 * @num_offset: size of junction offset array
 *
 * Return: 0 on success
 *	-EINVAL: invalid argument
 */
int thermal_tmu_set_junction_offset(struct google_sensor_data *sensor,
				    int offsets[], int num_offset)
{
	/* Enable setting junction offset only for userdebug build. */
#if IS_ENABLED(CONFIG_DEBUG_FS)
	struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;

	if (!sensor || !tmu_data || !offsets || num_offset != THERMAL_TMU_NR_TRIPS)
		return -EINVAL;

	mutex_lock(&offset_lock);
	tmu_data->skip_junction_offset = true;
	for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i) {
		tmu_data->junction_offset[i] = offsets[i];
		if (offsets[i] != 0)
			tmu_data->skip_junction_offset = false;
	}
	mutex_unlock(&offset_lock);
	return 0;
#else
	return 0;
#endif
}

/*
 * thermal_tmu_get_junction_offset - get junction offset to TMU data for debugfs
 *
 * @sensor: google sensor data structure
 * @buf: debugfs output buffer
 *
 * Return: 0 on success
 *	-EINVAL: invalid argument
 */
int thermal_tmu_get_junction_offset(struct google_sensor_data *sensor, int offsets[])
{
	struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;

	if (!sensor || !tmu_data || !offsets)
		return -EINVAL;

	mutex_lock(&offset_lock);
	for (int i = 0; i < THERMAL_TMU_NR_TRIPS; ++i)
		offsets[i] = tmu_data->junction_offset[i];
	mutex_unlock(&offset_lock);

	return 0;
}

/*
 * __thermal_tmu_trip_counter_init - initialize trip counter shared memory and sysfs node
 *
 * @dev: pointer to the thermal sensor device which the TMU belongs to
 * @sensor: pointer to the google sensor data of the TMU
 *
 * Return: 0 on success
 *	other error code returned from sm init functions or create device attr node function
 */
int __thermal_tmu_trip_counter_init(struct device *dev, struct google_sensor_data *sensor)
{
	struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;
	int ret;

	/* init shared memory */
	ret = sm_initialize_section(dev, THERMAL_SM_TRIP_COUNT);
	if (ret) {
		dev_err(dev, "failed to initialize shared memory trip counter section: ret=%d",
			ret);
		return ret;
	}

	/* create sysfs node */
	sysfs_attr_init(&tmu_data->dev_attr_trip_counter.attr);
	tmu_data->dev_attr_trip_counter.attr.name =
		devm_kasprintf(dev, GFP_KERNEL, "trip_counter_%s",
			       thermal_zone_device_type(sensor->tzd));
	tmu_data->dev_attr_trip_counter.attr.mode = 0444;
	tmu_data->dev_attr_trip_counter.show = trip_counter_show;

	/* create device attr sysfs node */
	ret = device_create_file(dev, &tmu_data->dev_attr_trip_counter);
	if (ret) {
		dev_err(dev, "failed to create file node for trip counter: tz=%s, ret=%d",
			thermal_zone_device_type(sensor->tzd), ret);
		return ret;
	}

	return 0;
}

/*
 * __thermal_tmu_get_trip_counter - get trip counter of a thermal zone
 *
 * @attr: pointer to the trip counter attr structure of the TMU sensor
 * @data: pointer to the shared memory trip counter data structure
 *
 * Return: 0 on success
 *	-EINVAL: invalid argument
 *	other error code returned from thermal msg or sm functions
 */
int __thermal_tmu_get_trip_counter(struct device_attribute *attr,
				   struct thermal_sm_trip_counter_data *data)
{
	struct google_sensor_data *sensor;
	int ret;

	mutex_lock(&trip_counter_lock);
	list_for_each_entry(sensor, tmu_sensor_list, node) {
		struct tmu_sensor_data *tmu_data = sensor->sens_priv_data;
		u8 tz_id = sensor->sensor_id;

		if (attr == &tmu_data->dev_attr_trip_counter) {
			ret = tmu_get_trip_counter_snapshot(tz_id);
			if (ret) {
				pr_err("failed to get trip counter of tz(%s): ret=%d",
				       thermal_zone_device_type(sensor->tzd), ret);
				/* thermal msg ret may >0, return -EIO to avoid conflict */
				ret = -EIO;
				goto unlock_exit;
			}
			ret = sm_get_tmu_trip_counter(data);
			if (ret)
				pr_err("failed to read trip counters from sm for tz(%s); ret=%d",
				       thermal_zone_device_type(sensor->tzd), ret);
			goto unlock_exit;
		}
	}
	ret = -ENODEV;

unlock_exit:
	mutex_unlock(&trip_counter_lock);
	return ret;
}
