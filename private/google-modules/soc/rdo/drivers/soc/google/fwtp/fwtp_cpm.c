// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Google LLC.
 *
 * CPM firmware tracepoint driver.
 */

#include <linux/dma-mapping.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <soc/google/goog_mba_cpm_iface.h>
#include <soc/google/goog_cpm_service_ids.h>

#include "fwtp.h"

/*******************************************************************************
 * Data structures and defs.
 ******************************************************************************/

/* Mailbox request timeout time in milliseconds. */
#define FWTP_MBA_REQ_TIMEOUT_MS 3000

/**
 * struct fwtp_cpm_dev - Structure representing a CPM FWTP device.
 *
 * @base: Base FWTP kernel device record.
 * @cpm_client: Mailbox interface client to CPM.
 * @mba_channel: CPM mailbox channel.
 * @reserved_mem_initialized: If true, device reserved memory has been
 *                            initialized.
 * @dma_size: Size of DMA memory.
 * @dma_base: Base address of DMA memory.
 * @dma_base_phys: Physical base address of DMA memory.
 */
struct fwtp_cpm_dev {
	struct fwtp_dev base;
	struct cpm_iface_client *cpm_client;
	u32 mba_channel;
	bool reserved_mem_initialized;
	size_t dma_size;
	void *dma_base;
	dma_addr_t dma_base_phys;
};

/**
 * struct cpm_mba_fwtp_msg - CPM mailbox FWTP message.
 *
 * @mba_header: Mailbox header.
 * @msg_phys_addr_lo: Lower 32-bits of message physical address.
 * @msg_phys_addr_hi: Upper 32-bits of message physical address.
 * @msg_buffer_size: Size of message buffer.
 * @msg_data_size: Size of message data.
 *
 * FWTP messages are stored in shared memory. This mailbox message specifies the
 * address and size of the FWTP message.
 */
struct cpm_mba_fwtp_msg {
	u32 mba_header;
	u32 msg_phys_addr_lo;
	u32 msg_phys_addr_hi;
	u16 msg_buffer_size;
	u16 msg_data_size;
};

/*******************************************************************************
 * Internal prototypes.
 ******************************************************************************/

static int fwtp_cpm_remove(struct platform_device *pdev);

/*******************************************************************************
 * FWTP interface functions.
 ******************************************************************************/

/**
 * fwtp_cpm_send_message - Sends a message through the interface.
 *
 * @fwtp_if: The firmware tracepoint interface through which to send message.
 * @msg_buffer: Buffer containing the message.
 * @msg_buffer_size: Size of the message buffer.
 * @tx_msg_data_size: Size of the message data to transmit.
 * @rx_msg_data_size: Size of the received message data.
 *
 * Sends the message contained in the buffer @msg_buffer. The size of the
 * message data to transmit is specified by @tx_msg_data_size.
 *
 * Any received response message is placed in the message buffer @msg_buffer.
 * The size of the data in the received message is returned in
 * @rx_msg_data_size. This will not be larger than the buffer size
 * @msg_buffer_size.
 *
 * Return: kFwtpOk on success, non-zero error code on error.
 */
static fwtp_error_code_t fwtp_cpm_send_message(struct fwtp_if *fwtp_if,
					       void *msg_buffer,
					       u16 msg_buffer_size,
					       u16 tx_msg_data_size,
					       u16 *rx_msg_data_size)
{
	struct fwtp_dev *fwtp_dev;
	struct fwtp_cpm_dev *fwtp_cpm_dev;
	struct cpm_mba_fwtp_msg cpm_mba_msg;
	struct cpm_iface_req cpm_req;
	u16 local_rx_msg_data_size;
	int ret;

	/* Get the CPM FWTP device record. */
	fwtp_dev =
		container_of(fwtp_if, struct fwtp_dev, fwtp_ipc_client.fwtp_if);
	fwtp_cpm_dev = container_of(fwtp_dev, struct fwtp_cpm_dev, base);

	/* Validate the message size. */
	if (tx_msg_data_size > msg_buffer_size) {
		dev_err(fwtp_cpm_dev->base.dev,
			"FWTP message data size %u too big for message buffer size %u.\n",
			tx_msg_data_size, msg_buffer_size);
		return -EINVAL;
	}
	if (msg_buffer_size > fwtp_cpm_dev->dma_size) {
		dev_err(fwtp_cpm_dev->base.dev,
			"FWTP message of size %d too big for DMA buffer of size %zu.\n",
			(int)msg_buffer_size, fwtp_cpm_dev->dma_size);
		return -EINVAL;
	}

	/* Encapsulate the FWTP message into a CPM FWTP message. */
	memcpy(fwtp_cpm_dev->dma_base, msg_buffer, tx_msg_data_size);
	cpm_mba_msg.msg_phys_addr_lo = fwtp_cpm_dev->dma_base_phys &
				       0xFFFFFFFFUL;
	cpm_mba_msg.msg_phys_addr_hi = fwtp_cpm_dev->dma_base_phys >> 32;
	cpm_mba_msg.msg_buffer_size = msg_buffer_size;
	cpm_mba_msg.msg_data_size = tx_msg_data_size;

	/* Ensure the FWTP message is visible to CPM before sending. */
	mb();

	/* Send the message through the interface. */
	cpm_req.msg_type = REQUEST_MSG;
	cpm_req.req_msg = (struct cpm_iface_payload *)&cpm_mba_msg;
	cpm_req.resp_msg = (struct cpm_iface_payload *)&cpm_mba_msg;
	cpm_req.dst_id = fwtp_cpm_dev->mba_channel;
	cpm_req.tout_ms = FWTP_MBA_REQ_TIMEOUT_MS;
	ret = cpm_send_message(fwtp_cpm_dev->cpm_client, &cpm_req);
	if (ret < 0) {
		dev_err(fwtp_cpm_dev->base.dev,
			"Failed to send message with error %d\n", ret);
		return ret;
	}

	/* Return the received response message. */
	local_rx_msg_data_size = cpm_mba_msg.msg_data_size;
	if (local_rx_msg_data_size > msg_buffer_size) {
		dev_err(fwtp_cpm_dev->base.dev,
			"Received message of size %d too big for message buffer of size %d.\n",
			(int)local_rx_msg_data_size, (int)msg_buffer_size);
		return -EIO;
	}
	memcpy(msg_buffer, fwtp_cpm_dev->dma_base, local_rx_msg_data_size);
	*rx_msg_data_size = local_rx_msg_data_size;

	return 0;
}

/*******************************************************************************
 * Platform driver functions.
 ******************************************************************************/

/**
 * fwtp_cpm_probe - Probe CPM firmware tracepoint devices.
 *
 * @pdev: The platform device to probe.
 */
static int fwtp_cpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwtp_cpm_dev *fwtp_cpm_dev;
	struct cpm_iface_client *cpm_client;
	struct fwtp_dev *fwtp_dev;
	struct device_node *dma_reserved_mem_node = NULL;
	struct reserved_mem *dma_reserved_mem;
	int ret;

	/* Log the start of probing. */
	dev_dbg(dev, "Probing CPM FWTP device.\n");

	/* Create a CPM FWTP device record. */
	fwtp_cpm_dev = devm_kzalloc(dev, sizeof(*fwtp_cpm_dev), GFP_KERNEL);
	if (!fwtp_cpm_dev) {
		ret = -ENOMEM;
		goto out;
	}
	platform_set_drvdata(pdev, fwtp_cpm_dev);

	/* Get a CPM mailbox interface client. */
	cpm_client = cpm_iface_request_client(dev, CPM_COMMON_FWTP_SERVICE,
					      NULL, NULL);
	if (IS_ERR(cpm_client)) {
		ret = PTR_ERR(cpm_client);
		dev_err(dev,
			"Failed to get a CPM mailbox interface handle with error %d.\n",
			ret);
		goto out;
	}
	fwtp_cpm_dev->cpm_client = cpm_client;

	/* Get the CPM mailbox channel. */
	ret = of_property_read_u32(dev->of_node, "mba-dest-channel",
				   &fwtp_cpm_dev->mba_channel);
	if (ret < 0) {
		dev_err(dev, "Failed to read mba-dest-channel.\n");
		goto out;
	}

	/* Get DMA memory. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev,
			"Failed to set DMA mask to 32 bits with error %d.\n",
			ret);
		goto out;
	}
	dma_reserved_mem_node =
		of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!dma_reserved_mem_node) {
		dev_err(dev, "Failed to get DMA memory region node.\n");
		ret = -ENODEV;
		goto out;
	}
	dma_reserved_mem = of_reserved_mem_lookup(dma_reserved_mem_node);
	if (!dma_reserved_mem) {
		dev_err(dev, "Failed to acquire DMA reserved memory.\n");
		ret = -ENODEV;
		goto out;
	}
	fwtp_cpm_dev->dma_size = dma_reserved_mem->size;
	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev,
			"Failed to assign device memory regions with error %d.\n",
			ret);
		goto out;
	}
	fwtp_cpm_dev->reserved_mem_initialized = true;
	fwtp_cpm_dev->dma_base =
		dma_alloc_coherent(dev, fwtp_cpm_dev->dma_size,
				   &fwtp_cpm_dev->dma_base_phys, GFP_KERNEL);
	if (!fwtp_cpm_dev->dma_base) {
		dev_err(dev, "Failed to allocate DMA memory.\n");
		ret = -ENOMEM;
		goto out;
	}

	/* Initialize the base FWTP device. */
	fwtp_dev = &(fwtp_cpm_dev->base);
	fwtp_dev->dev = dev;
	fwtp_dev->fwtp_ipc_client.fwtp_if.send_message = fwtp_cpm_send_message;
	ret = fwtp_dev_init(fwtp_dev);
	if (ret) {
		dev_err(dev,
			"Failed to initialize the base FWTP device with error %d.\n",
			ret);
		goto out;
	}

	/* Log an info message to validate driver. */
	/* TODO: b/358130519 - Remove this when testable features are added. */
	dev_info(dev, "Successfully probed CPM firmware tracepoint device.\n");

out:
	/* Clean up. */
	if (dma_reserved_mem_node)
		of_node_put(dma_reserved_mem_node);

	/* Clean up on error. */
	if (ret)
		fwtp_cpm_remove(pdev);

	return ret;
}

/**
 * fwtp_cpm_remove - Removes a CPM FWTP device.
 *
 * @pdev: The platform device to remove.
 */
static int fwtp_cpm_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwtp_cpm_dev *fwtp_cpm_dev;

	/*
	 * Get the CPM FWTP device record and remove it from the platform
	 * device.
	 */
	fwtp_cpm_dev = platform_get_drvdata(pdev);
	if (!fwtp_cpm_dev)
		return 0;
	platform_set_drvdata(pdev, NULL);

	/* Free DMA memory. */
	if (fwtp_cpm_dev->dma_base)
		dma_free_coherent(dev, fwtp_cpm_dev->dma_size,
				  fwtp_cpm_dev->dma_base,
				  fwtp_cpm_dev->dma_base_phys);

	/* Release the device reserved memory. */
	if (fwtp_cpm_dev->reserved_mem_initialized)
		of_reserved_mem_device_release(dev);

	/* Free the CPM mailbox interface client. */
	if (fwtp_cpm_dev->cpm_client)
		cpm_iface_free_client(fwtp_cpm_dev->cpm_client);

	/* Deinitialize the base FWTP device. */
	fwtp_dev_deinit(&(fwtp_cpm_dev->base));

	/* Log removal. */
	dev_dbg(dev, "Removed CPM FWTP device.\n");

	return 0;
}

/*******************************************************************************
 * Platform device configuration.
 ******************************************************************************/

/* Device tree match table. */
static const struct of_device_id fwtp_cpm_of_match_table[] = {
	{ .compatible = "google,fwtp-cpm" },
	{},
};
MODULE_DEVICE_TABLE(of, fwtp_cpm_of_match_table);

/* Platform driver configuration. */
static struct platform_driver fwtp_cpm_driver = {
	.probe = fwtp_cpm_probe,
	.remove = fwtp_cpm_remove,
	.driver = {
		.name = "fwtp-cpm",
		.owner = THIS_MODULE,
		.of_match_table = fwtp_cpm_of_match_table,
	},
};
module_platform_driver(fwtp_cpm_driver);

/*******************************************************************************
 * Module info.
 ******************************************************************************/

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("CPM firmware tracepoint");
MODULE_LICENSE("GPL");
