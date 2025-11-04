// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Google LLC.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/base64.h>
#include <linux/of_reserved_mem.h>
#include <soc/google/goog_gdmc_service_ids.h>
#include <soc/google/goog-mba-gdmc-iface.h>
#include <soc/google/goog_mba_nq_xport.h>

#include "vdu_service.h"
#include "google-vdu.h"

static int64_t gvdu_handle_mailbox_error(struct device *dev, int message_res,
					 uint32_t *header)
{
	if (goog_mba_nq_xport_get_error(header)) {
		/* GDMC firmware error code is int16_t, alignment is required */
		int16_t gdmc_error =
			(int16_t)(goog_mba_nq_xport_get_data(header) &
				  GENMASK(15, 0));

		return gdmc_error;
	}

	return message_res;
}

static bool gvdu_get_user_consent(struct device *dev)
{
	return of_find_property(dev->of_node, "consent-granted", NULL) != NULL;
}

static int64_t gvdu_retrieve_nonce_request(struct device *dev,
					   dma_addr_t buffer_pa)
{
	struct gdmc_mba_vdu_msg msg;
	int message_res;
	struct gvdu_base *base = dev_get_drvdata(dev);

	msg.header = goog_mba_nq_xport_create_hdr(GDMC_MBA_SERVICE_ID_VDU,
						  GDMC_MBA_VDU_RETRIEVE_NONCE);
	msg.payload.retrieve_nonce_req.pa_low = (uint32_t)buffer_pa;
	msg.payload.retrieve_nonce_req.pa_high = (uint32_t)(buffer_pa >> 32);
	msg.payload.retrieve_nonce_req.buffer_capacity =
		sizeof(struct gdmc_mba_vdu_msg_nonce_buffer);

	message_res = gdmc_send_message(base->gdmc_iface, &msg);

	return gvdu_handle_mailbox_error(dev, message_res, &msg.header);
}

static int64_t gvdu_get_vector_request(struct device *dev, dma_addr_t buffer_pa)
{
	struct gvdu_base *base = dev_get_drvdata(dev);
	struct gdmc_mba_vdu_msg msg;
	int message_res;

	msg.header = goog_mba_nq_xport_create_hdr(GDMC_MBA_SERVICE_ID_VDU,
						  GDMC_MBA_VDU_GET_VECTOR);
	msg.payload.get_vector_req.pa_low = (uint32_t)buffer_pa;
	msg.payload.get_vector_req.pa_high = (uint32_t)(buffer_pa >> 32);
	msg.payload.get_vector_req.buffer_capacity =
		sizeof(struct gdmc_mba_vdu_msg_vector_buffer);

	message_res = gdmc_send_message(base->gdmc_iface, &msg);

	return gvdu_handle_mailbox_error(dev, message_res, &msg.header);
}

static int64_t gvdu_get_timer_request(struct device *dev,
				      struct gdmc_mba_vdu_msg *msg)
{
	struct gvdu_base *base = dev_get_drvdata(dev);
	int message_res;

	msg->header = goog_mba_nq_xport_create_hdr(GDMC_MBA_SERVICE_ID_VDU,
						   GDMC_MBA_VDU_GET_TIMER);

	message_res = gdmc_send_message(base->gdmc_iface, msg);

	return gvdu_handle_mailbox_error(dev, message_res, &msg->header);
}

static int64_t gvdu_process_directive_request(struct device *dev,
					      dma_addr_t buffer_pa,
					      uint16_t grant_size,
					      uint16_t delegate_size)
{
	struct gdmc_mba_vdu_msg msg;
	int message_res;
	struct gvdu_base *base = dev_get_drvdata(dev);

	msg.header =
		goog_mba_nq_xport_create_hdr(GDMC_MBA_SERVICE_ID_VDU,
					     GDMC_MBA_VDU_PROCESS_DIRECTIVE);
	msg.payload.process_directive_req.pa_low = (uint32_t)buffer_pa;
	msg.payload.process_directive_req.pa_high = (uint32_t)(buffer_pa >> 32);
	msg.payload.process_directive_req.size.grant = grant_size;
	msg.payload.process_directive_req.size.delegate = delegate_size;

	message_res = gdmc_send_message(base->gdmc_iface, &msg);

	return gvdu_handle_mailbox_error(dev, message_res, &msg.header);
}

/* Caller must free memory */
static ssize_t gvdu_decode_base64_buffer(struct device *dev, const char *source,
					 size_t count, char **dest)
{
	/* 4 Base64 characters -> 3 binary data bytes */
	ssize_t decoded_length_upper_bound = count * 3 / 4;
	int len;
	void *buf;

	if (decoded_length_upper_bound > GDMC_MBA_VDU_DIRECTIVE_MAX_SIZE)
		return -EINVAL;

	buf = kmalloc(decoded_length_upper_bound, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len = base64_decode(source, count, buf);
	if (len < 0) {
		kfree(buf);
		return -EINVAL;
	}

	*dest = buf;
	return len;
}

static int64_t gvdu_emit_retrieve_nonce_request(struct device *dev,
						char *sysfs_buf,
						bool write_nonce)
{
	const uint32_t buffer_size =
		sizeof(struct gdmc_mba_vdu_msg_nonce_buffer);
	dma_addr_t buffer_pa;
	struct gdmc_mba_vdu_msg_nonce_buffer *buffer =
		dma_alloc_coherent(dev, buffer_size, &buffer_pa, GFP_KERNEL);
	ssize_t ret;

	if (!buffer || !buffer_pa)
		return -ENOMEM;

	ret = gvdu_retrieve_nonce_request(dev, buffer_pa);
	if (ret) {
		dev_dbg(dev, "Mailbox request failed: %zd.\n", ret);
		goto exit;
	}

	if (write_nonce)
		ret = sysfs_emit(sysfs_buf, "%*phN\n", GDMC_MBA_VDU_NONCE_LEN,
				 buffer->nonce);
	else
		ret = sysfs_emit(sysfs_buf, "%*phN\n", GDMC_MBA_VDU_CHIPID_LEN,
				 buffer->chipid);

exit:
	dma_free_coherent(dev, buffer_size, buffer, buffer_pa);
	return ret;
}

static int64_t gvdu_emit_get_timer_request(struct device *dev, char *buf,
					   bool write_timer)
{
	struct gdmc_mba_vdu_msg msg;
	ssize_t ret;

	ret = gvdu_get_timer_request(dev, &msg);
	if (ret) {
		dev_dbg(dev, "Mailbox request failed: %zd.\n", ret);
		return ret;
	}

	if (write_timer)
		ret = sysfs_emit(buf, "%04x\n",
				 msg.payload.get_timer_rsp.remaining_mins);
	else
		ret = sysfs_emit(buf, "%u\n",
				 msg.payload.get_timer_rsp.status);

	return ret;
}

static int64_t gvdu_read_base64_buffer(struct device *dev,
				       const char *buf, size_t count,
				       char **dest, ssize_t *length)
{
	int ret;

	kfree(*dest);
	*dest = NULL;

	ret = gvdu_decode_base64_buffer(dev, buf, count, dest);

	if (ret >= 0) {
		*length = ret;
		ret = count;
	}

	return ret;
}

static ssize_t chip_id_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return gvdu_emit_retrieve_nonce_request(dev, buf, false);
}

static DEVICE_ATTR_RO(chip_id);

static ssize_t default_debug_vector_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "F8FFFFEFFFFFFFFFFFFF9F0100000000\n");
}

static DEVICE_ATTR_RO(default_debug_vector);

static ssize_t default_policy_type_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(
		buf,
		"POLICY_TYPE_LGA_DEV_AP,POLICY_TYPE_LGA_DEV_GSA,POLICY_TYPE_LGA_RAMDUMP\n");
}

static DEVICE_ATTR_RO(default_policy_type);

static ssize_t nonce_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return gvdu_emit_retrieve_nonce_request(dev, buf, true);
}

static DEVICE_ATTR_RO(nonce);

static ssize_t consent_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%c\n", gvdu_get_user_consent(dev) ? '1' : '0');
}

static DEVICE_ATTR_RO(consent);

static ssize_t vector_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	const int buffer_size = sizeof(struct gdmc_mba_vdu_msg_vector_buffer);
	dma_addr_t buffer_pa;
	struct gdmc_mba_vdu_msg_vector_buffer *buffer =
		dma_alloc_coherent(dev, buffer_size, &buffer_pa, GFP_KERNEL);
	ssize_t ret;

	if (!buffer || !buffer_pa) {
		return -ENOMEM;
	}

	ret = gvdu_get_vector_request(dev, buffer_pa);
	if (ret) {
		dev_dbg(dev, "Mailbox request failed: %zd.\n", ret);
		goto exit;
	}

	ret = sysfs_emit(buf, "%*phN\n", GDMC_MBA_VDU_VECTOR_LEN,
			 buffer->data);

exit:
	dma_free_coherent(dev, buffer_size, buffer, buffer_pa);
	return ret;
}

static DEVICE_ATTR_RO(vector);

static ssize_t remaining_mins_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	return gvdu_emit_get_timer_request(dev, buf, true);
}

static DEVICE_ATTR_RO(remaining_mins);

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return gvdu_emit_get_timer_request(dev, buf, false);
}

static DEVICE_ATTR_RO(enabled);

static ssize_t grant_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct gvdu_base *base = dev_get_drvdata(dev);

	return gvdu_read_base64_buffer(dev, buf, count,
				       &base->grant_buffer,
				       &base->grant_length);
}

DEVICE_ATTR_WO(grant);

static ssize_t delegate_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gvdu_base *base = dev_get_drvdata(dev);

	return gvdu_read_base64_buffer(dev, buf, count,
				       &base->delegate_buffer,
				       &base->delegate_length);
}

DEVICE_ATTR_WO(delegate);

static ssize_t auth_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct gvdu_base *base = dev_get_drvdata(dev);
	dma_addr_t buffer_pa;
	char *buffer;
	const size_t buffer_size = base->grant_length + base->delegate_length;
	ssize_t ret;

	if (!gvdu_get_user_consent(dev)) {
		dev_dbg(dev, "User consent is not granted, failed to process VDU directive\n");
		ret = -EACCES;
		goto error;
	}

	if (!base->grant_buffer || !base->delegate_buffer) {
		ret = -EINVAL;
		goto error;
	}

	if (buffer_size > GDMC_MBA_VDU_DIRECTIVE_MAX_SIZE) {
		ret = -EINVAL;
		goto error;
	}

	buffer = dma_alloc_coherent(dev, buffer_size, &buffer_pa, GFP_KERNEL);
	if (!buffer || !buffer_pa) {
		ret = -ENOMEM;
		goto error;
	}

	memcpy(buffer, base->grant_buffer, base->grant_length);
	memcpy(buffer + base->grant_length, base->delegate_buffer,
	       base->delegate_length);

	ret = gvdu_process_directive_request(dev, buffer_pa, base->grant_length,
					     base->delegate_length);
	if (ret)
		dev_dbg(dev, "Mailbox request failed: %zd.\n", ret);
	else
		ret = count;

	dma_free_coherent(dev, buffer_size, buffer, buffer_pa);

error:
	kfree(base->grant_buffer);
	base->grant_buffer = NULL;

	kfree(base->delegate_buffer);
	base->delegate_buffer = NULL;

	return ret;
}

DEVICE_ATTR_WO(auth);

static struct attribute *gvdu_info_attrs[] = { &dev_attr_chip_id.attr,
					       &dev_attr_default_debug_vector.attr,
					       &dev_attr_default_policy_type.attr, NULL };

static struct attribute *gvdu_interface_attrs[] = { &dev_attr_nonce.attr,
						    &dev_attr_grant.attr,
						    &dev_attr_delegate.attr,
						    &dev_attr_auth.attr, NULL };

static struct attribute *gvdu_status_attrs[] = { &dev_attr_consent.attr,
						 &dev_attr_vector.attr,
						 &dev_attr_remaining_mins.attr,
						 &dev_attr_enabled.attr, NULL };

static const struct attribute_group gvdu_info_group = {
	.name = "info",
	.attrs = gvdu_info_attrs,
};

static const struct attribute_group gvdu_interface_group = {
	.name = "interface",
	.attrs = gvdu_interface_attrs,
};

static const struct attribute_group gvdu_status_group = {
	.name = "status",
	.attrs = gvdu_status_attrs,
};

static const struct attribute_group *gvdu_groups[] = {
	&gvdu_info_group,
	&gvdu_interface_group,
	&gvdu_status_group,
	NULL,
};

static int google_vdu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gvdu_base *base;
	int ret;

	base = devm_kzalloc(dev, sizeof(struct gvdu_base), GFP_KERNEL);
	if (!base)
		return -ENOMEM;
	platform_set_drvdata(pdev, base);

	base->gdmc_iface = gdmc_iface_get(dev);

	if (IS_ERR(base->gdmc_iface)) {
		dev_err(dev, "Failed to get GDMC interface");
		return PTR_ERR(base->gdmc_iface);
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to get reserved memory region.\n");
		gdmc_iface_put(base->gdmc_iface);
		return ret;
	}

	ret = devm_device_add_groups(dev, gvdu_groups);
	if (ret != 0) {
		gdmc_iface_put(base->gdmc_iface);
		of_reserved_mem_device_release(dev);
	}
	return ret;
}

static int google_vdu_remove(struct platform_device *pdev)
{
	struct gvdu_base *base = platform_get_drvdata(pdev);

	kfree(base->grant_buffer);
	kfree(base->delegate_buffer);

	of_reserved_mem_device_release(&pdev->dev);
	gdmc_iface_put(base->gdmc_iface);
	return 0;
}

static const struct of_device_id google_vdu_of_match[] = {
	{ .compatible = "google,volatile-debug-unlock" },
	{},
};
MODULE_DEVICE_TABLE(of, google_vdu_of_match);

static struct platform_driver google_vdu_driver = {
	.probe = google_vdu_probe,
	.remove = google_vdu_remove,
	.driver = {
		.name  = "google-vdu",
		.of_match_table = of_match_ptr(google_vdu_of_match),
	},
};
module_platform_driver(google_vdu_driver);

MODULE_AUTHOR("Kanstantsin Yarmash <kyarmash@google.com>");
MODULE_DESCRIPTION("Google Volatile Debug Unlock");
MODULE_LICENSE("GPL");
