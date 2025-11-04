/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2023 Google LLC
 * Abstract interface for EBU devices used with USB FIFO endpoints
 */

#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/usb.h>

struct ebu_controller {
	int (*add_mapping)(struct ebu_controller *ebu, u8 channel, u8 epaddr);
	int (*release_mapping)(struct ebu_controller *ebu, u8 channel);
	dma_addr_t (*get_fifo)(struct ebu_controller *ebu, u8 channel);
	int (*enable_data)(struct ebu_controller *ebu, enum usb_device_speed speed);
};
