// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023, Google LLC
 *
 * MAX777x9 SPMI bus driver
 */
#include <linux/gpio/consumer.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <linux/pm_wakeup.h>

#include "tcpci_max77759.h"
#include "tcpci_max77759_vendor_reg.h"

#define SPMI_MAX_EXT_RD_WR_SIZE                 16U
#define MAX_PD_MSG_SIZE_PER_READ                (SPMI_MAX_EXT_RD_WR_SIZE - \
						 TCPC_RECEIVE_BUFFER_METADATA_SIZE)

#define TX_BUF_REG_COUNT_SIZE                   1U
#define MAX_PD_MSG_SIZE_PER_WRITE               (SPMI_MAX_EXT_RD_WR_SIZE - TX_BUF_REG_COUNT_SIZE)

#define MAX777x9_BUCK_BOOST_SPMI_SID 0x3

/* To protect against direct access to spmi driver and access via regmap */
struct mutex g_spmi_regmap_lock;

static void spmi_access_lock(void *arg)
{
	mutex_lock(&g_spmi_regmap_lock);
}

static void spmi_access_unlock(void *arg)
{
	mutex_unlock(&g_spmi_regmap_lock);
}

static const struct regmap_range max77759_tcpci_range[] = {
	regmap_reg_range(MAX77759_MIN_ADDR, MAX77759_MAX_ADDR)
};

const struct regmap_access_table max77759_tcpci_write_table = {
	.yes_ranges = max77759_tcpci_range,
	.n_yes_ranges = ARRAY_SIZE(max77759_tcpci_range),
};

static const struct regmap_config max77759_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = MAX77759_MAX_ADDR,
	.wr_table = &max77759_tcpci_write_table,
	.lock = spmi_access_lock,
	.unlock = spmi_access_unlock,
};

static int max777x9_spmi_direct_otg_en(struct max77759_plat *chip, bool en)
{
	struct spmi_device *sdev = chip->client;
	u8 reg, buffer, cached_usid = sdev->usid;
	int ret;

	reg = (chip->product_id == MAX77779_PRODUCT_ID) ? MAX77779_BUCK_BOOST_OP :
							  MAX77759_BUCK_BOOST_OP;
	buffer = en ? MAX777x9_BUCK_BOOST_SOURCE : MAX777x9_BUCK_BOOST_OFF;

	mutex_lock(&g_spmi_regmap_lock);
	sdev->usid = MAX777x9_BUCK_BOOST_SPMI_SID;
	ret = spmi_ext_register_write(sdev, reg, &buffer, 1);
	sdev->usid = cached_usid;
	mutex_unlock(&g_spmi_regmap_lock);

	return ret;
}

static int max777x9_init_alert(struct max77759_plat *chip)
{
	struct gpio_desc *irq_gpio;
	int irq;

	irq_gpio = devm_gpiod_get_optional(chip->dev, "usbpd,usbpd_int", GPIOD_ASIS);
	irq = gpiod_to_irq(irq_gpio);
	if (!irq)
		return -ENODEV;

	return max77759_register_irq(irq, (IRQF_TRIGGER_LOW | IRQF_ONESHOT), chip);
}

/* Note: 'rx_buf' has space allocated for metadata (count and frametype) + pd_msg_size. */
static int max777x9_spmi_rx(struct google_shim_tcpci *tcpci, u8 *rx_buf, size_t pd_msg_size)
{
	struct max77759_plat *chip = tdata_to_max77759(tcpci->data);
	int rx_buf_head = 0, read_size, ret;
	int pending_pd_msg_size = (int)pd_msg_size;

	/*
	 * Use cache to store last 2 bytes as subsequent reads re-write it with
	 * metadata. Restore these cached values after read (except first) as
	 * the metadata for is discarded for subsequent reads.
	 */
	u8 cache[TCPC_RECEIVE_BUFFER_METADATA_SIZE] = {0,};
	u8 frame_type, cnt, frame_type_idx, cnt_idx;

	while (pending_pd_msg_size > 0) {
		if (rx_buf_head)
			memcpy(cache, rx_buf + rx_buf_head, TCPC_RECEIVE_BUFFER_METADATA_SIZE);

		read_size = min_t(int, pending_pd_msg_size, MAX_PD_MSG_SIZE_PER_READ);
		if (unlikely(read_size + rx_buf_head > pd_msg_size)) {
			logbuffer_log(chip->log,
				      "Insufficient buffer size, required size: %d, available size: %zu",
				      read_size, pd_msg_size - rx_buf_head);
			return -ENOMEM;
		}

		ret = regmap_raw_read(chip->data.regmap, TCPC_RX_BYTE_CNT, rx_buf + rx_buf_head,
				      read_size + TCPC_RECEIVE_BUFFER_METADATA_SIZE);
		if (ret) {
			logbuffer_log(chip->log,
				      "Regmap read failed with error %d", ret);
			return ret;
		}

		cnt_idx = rx_buf_head + TCPC_RECEIVE_BUFFER_COUNT_OFFSET;
		frame_type_idx = rx_buf_head + TCPC_RECEIVE_BUFFER_FRAME_TYPE_OFFSET;

		if (rx_buf_head) {
			if (unlikely(cnt < rx_buf[cnt_idx] ||
				     frame_type !=
				     rx_buf[frame_type_idx])) {
				logbuffer_log(chip->log,
					      "Mismatched RX buffer metadata. Previously, cnt=%u, frame_type=%u. Now, cnt=%u, frame_type=%u.",
					      cnt, frame_type, rx_buf[cnt_idx],
					      rx_buf[frame_type_idx]);
				return -EIO;
			}
			memcpy(rx_buf + rx_buf_head, cache, TCPC_RECEIVE_BUFFER_METADATA_SIZE);
		} else {
			/*
			 * Save metadata obtained from first SPMI read. These values will be used to
			 * compare with metadata from subsequent reads for correctness.
			 */
			cnt = rx_buf[cnt_idx];
			frame_type = rx_buf[frame_type_idx];
		}

		pending_pd_msg_size -= read_size;
		rx_buf_head += read_size;
	}
	return 0;
}

static int max777x9_spmi_tx(struct google_shim_tcpci *tcpci, enum tcpm_transmit_type type,
			    const struct pd_message *msg, unsigned int negotiated_rev)
{
	u8 tx_buf[TCPC_TRANSMIT_BUFFER_MAX_LEN] = {0,};
	struct max77759_plat *chip = tdata_to_max77759(tcpci->data);
	unsigned int reg;
	u16 header = msg ? le16_to_cpu(msg->header) : 0;

	/*
	 * Reserve TX_BUF_REG_COUNT_SIZE space at the beginning of tx_buf to store
	 * count.
	 */
	int pending_pd_msg_size = 0, tx_buf_head = TX_BUF_REG_COUNT_SIZE;

	int msg_payload_size, send_size;
	int ret;

	msg_payload_size = msg ? pd_header_cnt(header) * 4 : 0;

	if (!tcpci->data->TX_BUF_BYTE_x_hidden) {
		logbuffer_logk(chip->log, LOGLEVEL_ERR, "TX_BUF_BYTE_x_hidden should be set");
		return -EINVAL;
	}

	if (msg) {
		memcpy(&tx_buf[pending_pd_msg_size + TX_BUF_REG_COUNT_SIZE], &msg->header,
		       sizeof(msg->header));
		pending_pd_msg_size += sizeof(msg->header);
		memcpy(&tx_buf[pending_pd_msg_size + TX_BUF_REG_COUNT_SIZE], msg->payload,
		       msg_payload_size);
		pending_pd_msg_size += msg_payload_size;
	}

	while (pending_pd_msg_size > 0) {
		send_size = min_t(size_t, pending_pd_msg_size, MAX_PD_MSG_SIZE_PER_WRITE);
		tx_buf[tx_buf_head - TX_BUF_REG_COUNT_SIZE] = send_size;
		ret = regmap_raw_write(tcpci->regmap, TCPC_TX_BYTE_CNT,
				       tx_buf + tx_buf_head - TX_BUF_REG_COUNT_SIZE,
				       send_size + TX_BUF_REG_COUNT_SIZE);
		if (ret < 0) {
			logbuffer_logk(chip->log, LOGLEVEL_ERR,
				       "Regmap write failed with error %d", ret);
			return ret;
		}
		tx_buf_head += send_size;
		pending_pd_msg_size -= send_size;
	}

	/* nRetryCount is 3 in PD2.0 spec where 2 in PD3.0 spec */
	reg = ((negotiated_rev > PD_REV20 ? PD_RETRY_COUNT_3_0_OR_HIGHER : PD_RETRY_COUNT_DEFAULT)
	       << TCPC_TRANSMIT_RETRY_SHIFT) | (type << TCPC_TRANSMIT_TYPE_SHIFT);
	ret = regmap_write(tcpci->regmap, TCPC_TRANSMIT, reg);

	return ret;
}

static int max777x9_spmi_probe(struct spmi_device *sdev)
{
	int ret;
	struct regmap *regmap;
	struct max777x9_desc desc = {};

	mutex_init(&g_spmi_regmap_lock);
	sdev->dev.init_name = "spmi-max77759tcpc";
	regmap = devm_regmap_init_spmi_ext(sdev, &max77759_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&sdev->dev, "Regmap init failed");
		return PTR_ERR(regmap);
	}

	desc.dev = &sdev->dev;
	desc.rmap = regmap;
	desc.client = (void *)sdev;
	desc.tx = max777x9_spmi_tx;
	desc.rx = max777x9_spmi_rx;

	ret = max77759_register(&desc, max777x9_spmi_direct_otg_en);
	if (ret)
		return ret;

	if (!desc.plat) {
		dev_err(&sdev->dev, "Expected non-NULL desc.plat value");
		return -EINVAL;
	}

	ret = max777x9_init_alert(desc.plat);
	if (ret < 0) {
		dev_err(&sdev->dev, "init alert failed");
		max77759_unregister(desc.plat);
		return ret;
	}

	device_init_wakeup(&sdev->dev, true);
	return ret;
}

static void max777x9_spmi_remove(struct spmi_device *sdev)
{
	struct max77759_plat *chip = spmi_device_get_drvdata(sdev);

	max77759_unregister(chip);
}

static void max777x9_spmi_shutdown(struct spmi_device *sdev)
{
	struct max77759_plat *chip = spmi_device_get_drvdata(sdev);

	max77759_shutdown(chip);
}

#ifdef CONFIG_OF
static const struct of_device_id max777x9_of_match[] = {
	{ .compatible = "max77759tcpc-spmi", },
	{},
};
MODULE_DEVICE_TABLE(of, max777x9_of_match);
#endif

static struct spmi_driver max777x9_spmi_driver = {
	.driver = {
		.name = "max77759tcpc-spmi",
		.of_match_table = of_match_ptr(max777x9_of_match),
	},
	.probe = max777x9_spmi_probe,
	.remove = max777x9_spmi_remove,
	.shutdown = max777x9_spmi_shutdown,
};

module_spmi_driver(max777x9_spmi_driver);

MODULE_DESCRIPTION("MAX777x9 TCPCI SPMI Driver");
MODULE_LICENSE("GPL");
