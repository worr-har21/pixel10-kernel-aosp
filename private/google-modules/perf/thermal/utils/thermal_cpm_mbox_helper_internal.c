// SPDX-License-Identifier: GPL-2.0-only
/*
 * thermal_cpm_mbox_impl Implement for thermal_cpm_mbox functionality.
 *
 * Copyright (c) 2024, Google LLC. All rights reserved.
 */
#include <linux/devm-helpers.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "thermal_cpm_mbox_helper_internal.h"

static const struct thermal_cpm_mbox_platform_data lga_data = {
	.tz_cdev_table = {
		[0] = {.tz_id = HW_THERMAL_ZONE_BIG, .cdev_id = HW_CDEV_BIG, .tz_name = "BIG"},
		[1] = {.tz_id = HW_THERMAL_ZONE_BIG_MID, .cdev_id = HW_CDEV_BIG_MID,
		       .tz_name = "BIG_MID"},
		[2] = {.tz_id = HW_THERMAL_ZONE_MID, .cdev_id = HW_CDEV_MID, .tz_name = "MID"},
		[3] = {.tz_id = HW_THERMAL_ZONE_LIT, .cdev_id = HW_CDEV_LIT, .tz_name = "LITTLE"},
		[4] = {.tz_id = HW_THERMAL_ZONE_GPU, .cdev_id = HW_CDEV_GPU, .tz_name = "GPU"},
		[5] = {.tz_id = HW_THERMAL_ZONE_TPU, .cdev_id = HW_CDEV_TPU, .tz_name = "TPU"},
		[6] = {.tz_id = HW_THERMAL_ZONE_AUR, .cdev_id = HW_CDEV_AUR, .tz_name = "AUR"},
		[7] = {.tz_id = HW_THERMAL_ZONE_ISP, .cdev_id = HW_CDEV_ISP, .tz_name = "ISP"},
		[8] = {.tz_id = HW_THERMAL_ZONE_MEM, .cdev_id = HW_CDEV_MEM, .tz_name = "MEM"},
		[9] = {.tz_id = HW_THERMAL_ZONE_AOC, .cdev_id = HW_CDEV_AOC, .tz_name = "AOC"},
	},
};

static const struct of_device_id thermal_cpm_mbox_platform_table[] = {
	{
		.compatible = "google,lga",
		.data = &lga_data,
	},
	{}
};

int cpm_mbox_parse_soc_data(struct thermal_cpm_mbox_driver_data *drv_data)
{
	struct device_node *np;
	const struct of_device_id *match;

	np = of_find_node_by_path("/");
	if (!np)
		return -ENODEV;

	match = of_match_node(thermal_cpm_mbox_platform_table, np);
	of_node_put(np);

	if (!match)
		return -EINVAL;

	drv_data->soc_data = match->data;

	if (!drv_data->soc_data)
		return -EINVAL;

	return 0;
}

int tzid_to_rx_cb_type(struct thermal_cpm_mbox_driver_data *drv_data, enum hw_thermal_zone_id tz_id,
		       enum hw_dev_type *cdev_id)
{
	if (!drv_data || !cdev_id || tz_id >= HW_THERMAL_ZONE_MAX || tz_id < 0)
		return -EINVAL;

	for (int i = 0; i < ARRAY_SIZE(drv_data->soc_data->tz_cdev_table); ++i) {
		if (drv_data->soc_data->tz_cdev_table[i].tz_id == tz_id) {
			*cdev_id = drv_data->soc_data->tz_cdev_table[i].cdev_id;
			return 0;
		}
	}

	return -EINVAL;
}

int hw_cdev_id_to_tzid(struct thermal_cpm_mbox_driver_data *drv_data, enum hw_dev_type cdev_id,
		       enum hw_thermal_zone_id *tz_id)
{
	if (!drv_data || !tz_id || cdev_id > HW_CDEV_MAX || cdev_id < 0)
		return -EINVAL;

	for (int i = 0; i < ARRAY_SIZE(drv_data->soc_data->tz_cdev_table); ++i) {
		if (drv_data->soc_data->tz_cdev_table[i].cdev_id == cdev_id) {
			*tz_id = drv_data->soc_data->tz_cdev_table[i].tz_id;
			return 0;
		}
	}

	return -EINVAL;
}

int cpm_mbox_send_message(struct thermal_cpm_mbox_driver_data *drv_data,
			  struct cpm_iface_req *cpm_req)
{
	return cpm_send_message(drv_data->client, cpm_req);
}

static void cpm_mbox_rx_worker(struct work_struct *work)
{
	struct thermal_cpm_mbox_rx_work *rx = container_of(work, struct thermal_cpm_mbox_rx_work,
							   work);
	u32 payload[GOOG_MBA_PAYLOAD_SIZE];
	struct thermal_cpm_mbox_request *req;
	unsigned long flags;

	spin_lock_irqsave(&rx->rx_lock, flags);
	req = (struct thermal_cpm_mbox_request *)rx->data;
	switch (req->type) {
	case THERMAL_NTC_REQUEST:
		payload[0] = req->tzid;
		payload[1] = req->req_rsvd0;
		break;
	case THERMAL_REQUEST_THROTTLE:
	default:
		memcpy(payload, rx->data, sizeof(payload));
		break;
	}
	spin_unlock_irqrestore(&rx->rx_lock, flags);

	blocking_notifier_call_chain(&rx->notifier, 0, payload);
}

/* This func will be execute in irq context */
void cpm_mbox_rx_callback(u32 context, void *msg, void *priv_data)
{
	struct cpm_iface_payload *cpm_msg = msg;
	struct thermal_cpm_mbox_response *resp = (struct thermal_cpm_mbox_response *)
						cpm_msg->payload;
	struct thermal_cpm_mbox_driver_data *drv_data = priv_data;
	struct thermal_cpm_mbox_rx_work *rx;
	enum hw_dev_type hw_rx_cb_type;
	int ret;
	unsigned long flags;

	switch (resp->type) {
	case THERMAL_REQUEST_THROTTLE:
		ret = tzid_to_rx_cb_type(drv_data, resp->tzid, &hw_rx_cb_type);
		if (ret) {
			dev_err(drv_data->dev, "Invalid tzid: %u from CPM: ret=%d\n",
				resp->tzid, ret);
			return;
		}
		break;
	case THERMAL_NTC_REQUEST:
		hw_rx_cb_type = HW_RX_CB_NTC;
		dev_err(drv_data->dev, "Received NTC IRQ.\n");
		break;
	default:
		dev_err(drv_data->dev, "Invalid type:%d.\n", resp->type);
		return;
	}

	rx = &drv_data->rx_work[hw_rx_cb_type];

	spin_lock_irqsave(&rx->rx_lock, flags);
	memcpy(rx->data, cpm_msg->payload, sizeof(rx->data));
	spin_unlock_irqrestore(&rx->rx_lock, flags);
	queue_work(system_highpri_wq, &rx->work);
}

int cpm_mbox_init(struct thermal_cpm_mbox_driver_data *drv_data)
{
	int ret;

	drv_data->client = cpm_iface_request_client(drv_data->dev,
						    CPM_SERVICE_ID_THERMAL,
						    cpm_mbox_rx_callback,
						    drv_data);
	if (IS_ERR(drv_data->client)) {
		ret = PTR_ERR(drv_data->client);
		if (ret == -EPROBE_DEFER)
			dev_dbg(drv_data->dev, "cpm interface not ready. Try again later\n");
		else
			dev_err(drv_data->dev, "Failed to request cpm client err: %d\n",
				ret);
		return ret;
	}

	return 0;
}

void thermal_cpm_mbox_init_notifier(struct thermal_cpm_mbox_driver_data *drv_data)
{
	int i;

	for (i = 0; i < HW_DEV_MAX; ++i) {
		BLOCKING_INIT_NOTIFIER_HEAD(&drv_data->rx_work[i].notifier);
		spin_lock_init(&drv_data->rx_work[i].rx_lock);
		devm_work_autocancel(drv_data->dev, &drv_data->rx_work[i].work,
				     cpm_mbox_rx_worker);
	}
}

void thermal_cpm_mbox_free(struct thermal_cpm_mbox_driver_data *drv_data)
{
	if (!IS_ERR_OR_NULL(drv_data) && !IS_ERR_OR_NULL(drv_data->client))
		cpm_iface_free_client(drv_data->client);
}

int cpm_mbox_parse_device_tree(struct thermal_cpm_mbox_driver_data *drv_data)
{
	struct device *dev = drv_data->dev;
	struct device_node *np = dev->of_node;

	if (of_property_read_u32(np, "mba-dest-channel", &drv_data->remote_ch)) {
		dev_err(dev, "Failed to read mba-dest-channel.\n");
		thermal_cpm_mbox_free(drv_data);
		return -EINVAL;
	}

	return 0;
}

const char *get_tz_name(struct thermal_cpm_mbox_driver_data *drv_data,
			enum hw_thermal_zone_id tz_id)
{
	if (tz_id >= HW_THERMAL_ZONE_MAX)
		return NULL;

	for (int i = 0; i < ARRAY_SIZE(drv_data->soc_data->tz_cdev_table); ++i)
		if (drv_data->soc_data->tz_cdev_table[i].tz_id == tz_id)
			return drv_data->soc_data->tz_cdev_table[i].tz_name;

	return NULL;
}
