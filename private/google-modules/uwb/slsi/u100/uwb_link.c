// SPDX-License-Identifier: GPL-2.0
/**
 * @copyright Copyright (c) 2023 Samsung Electronics Co., Ltd
 *
 */

#include "include/uwb.h"
#include "include/uwb_spi.h"
#include "include/uwb_gpio.h"

#define MT_OFFSET 0
#define CONTROL_PACKET_LENGTH_OFFSET 3
#define DATA_PACKET_LENGTH_MSB_OFFSET 3
#define DATA_PACKET_LENGTH_LSB_OFFSET 2
#define CRASH_UCI_LEN 8
#define MIN_UCI_PAYLOAD_SIZE 0
#define MAX_UCI_PAYLOAD_SIZE 0xFFFF

static char VENDOR_CMD_CRASH[CRASH_UCI_LEN] = { 0x2C, 0x3F, 0x00, 0x04, 0x0F,
							0x00, 0x00, 0x10 };

static bool drop_uci_pkt_on_format_error = true;
module_param(drop_uci_pkt_on_format_error, bool, 0664);
MODULE_PARM_DESC(drop_uci_pkt_on_format_error,
		"Drop uci packet on formatted error");

static int irq_receive_timeout_ms = 100;
module_param(irq_receive_timeout_ms, int, 0664);
MODULE_PARM_DESC(irq_receive_timeout_ms, "Timeout of receive IRQ from U100");

static int spi_send_timeout_ms = 200;
module_param(spi_send_timeout_ms, int, 0664);
MODULE_PARM_DESC(spi_send_timeout_ms, "Timeout of SPI sending");

static int uci_retry_count = 6;
module_param(uci_retry_count, int, 0664);
MODULE_PARM_DESC(uci_retry_count, "The max retry count of UCI command");

static int force_crash_on_uci_timeout = 1;
module_param(force_crash_on_uci_timeout, int, 0664);
MODULE_PARM_DESC(force_crash_on_uci_timeout,
	"Send Crash command to UWBS when UCI retry count exceeds the max count");

static int get_uci_payload_len(unsigned char *buffer)
{
	struct uci_msg_hdr *msg_hdr = (struct uci_msg_hdr *)buffer;

	if (msg_hdr->mt == UCI_MT_CMD || msg_hdr->mt == UCI_MT_RESP
		|| msg_hdr->mt == UCI_MT_NTF)
		return msg_hdr->len;
	else
		return msg_hdr->data_len;
}

static bool valid_header(char *buffer)
{
	if ((*(buffer) == NONE_DATA_FF && *(buffer + 1) == NONE_DATA_FF &&
				*(buffer + 2) == NONE_DATA_FF && *(buffer + 3) == NONE_DATA_FF))
		return false;
	if ((*(buffer) == NONE_DATA_00 && *(buffer + 1) == NONE_DATA_00 &&
				*(buffer + 2) == NONE_DATA_00 && *(buffer + 3) == NONE_DATA_00))
		return false;
	return true;
}

/* Function to handle receive-only communication */
static void handle_receive_only(struct u100_ctx *u100_ctx)
{
	int ret;
	unsigned int uci_payload_len, uci_transfer_len;
	struct sk_buff *rx_skb;
	char *rx_head_buff = NULL;
	char *rx_data_buff = NULL;

	rx_head_buff = kzalloc(UCI_HEAD_SIZE, GFP_KERNEL);
	if (!rx_head_buff)
		goto exit;

	ret = uwb_spi_recv(u100_ctx, rx_head_buff, UCI_HEAD_SIZE);
	if (ret)
		goto exit;

	if (!valid_header(rx_head_buff))
		goto exit;

	uci_payload_len = get_uci_payload_len(rx_head_buff);

	/* UCI payload length can be zero according to spec */
	/* Should not be treated as error */
	if (likely(uci_payload_len > 0)) {
		uci_transfer_len = ALIGN(uci_payload_len, 4);
		rx_data_buff = kzalloc(uci_transfer_len, GFP_KERNEL);
		if (!rx_data_buff)
			goto exit;

		ret = uwb_spi_recv(u100_ctx, rx_data_buff, uci_transfer_len);
		if (ret)
			goto exit;
	}

	rx_skb = alloc_skb(uci_payload_len + UCI_HEAD_SIZE, GFP_KERNEL);
	if (!rx_skb) {
		UWB_ERR("Failed to allocate sk_buff (msg size:%d)\n",
			uci_payload_len + UCI_HEAD_SIZE);
		goto exit;
	}
	skb_put(rx_skb, uci_payload_len + UCI_HEAD_SIZE);
	memcpy(rx_skb->data, rx_head_buff, UCI_HEAD_SIZE);
	if (likely(uci_payload_len > 0))
		memcpy((char *)(rx_skb->data + UCI_HEAD_SIZE), rx_data_buff, uci_payload_len);
	u100_ctx->recv_package(u100_ctx, rx_skb);
	UWB_DEBUG("Received UCI package %d bytes\n", uci_payload_len + UCI_HEAD_SIZE);

exit:
	kfree(rx_head_buff);
	kfree(rx_data_buff);
}

static void wakeup_sync_low(struct u100_ctx *u100_ctx)
{
	if (!atomic_read(&u100_ctx->u100_enter_download))
		set_gpio_value(&u100_ctx->gpio_u100_sync, 0);
}

/* Function to handle full-duplex communication */
static void handle_full_duplex(struct u100_ctx *u100_ctx)
{
	struct sk_buff_head *txq = &u100_ctx->sk_tx_q;
	struct sk_buff *rx_skb;
	int ret;
	struct sk_buff *tx_queue_skb;
	char *rx_data_buff = NULL;
	char *rx_rest_data_buff = NULL;
	char *tx_data_buff = NULL;
	unsigned int send_len, recv_len, recv_rest_len, recv_rest_align_len;

	u100_ctx->is_bhalf_entered = true;
	tx_queue_skb = skb_dequeue(txq);
	if (unlikely(!tx_queue_skb)) {
		wakeup_sync_low(u100_ctx);
		complete_all(&u100_ctx->tx_done_cmpl);
		handle_receive_only(u100_ctx);
		return;
	}
	send_len = ALIGN(tx_queue_skb->len, 4);
	tx_data_buff = kzalloc(send_len, GFP_KERNEL);
	rx_data_buff = kzalloc(send_len, GFP_KERNEL);
	if (unlikely(!tx_data_buff) || unlikely(!rx_data_buff)) {
		wakeup_sync_low(u100_ctx);
		complete_all(&u100_ctx->tx_done_cmpl);
		kfree(tx_data_buff);
		kfree(rx_data_buff);
		dev_kfree_skb_any(tx_queue_skb);
		handle_receive_only(u100_ctx);
		return;
	}
	memcpy(tx_data_buff, tx_queue_skb->data, tx_queue_skb->len);
	dev_kfree_skb_any(tx_queue_skb);
	ret = uwb_spi_send(u100_ctx, tx_data_buff, send_len, rx_data_buff);
	wakeup_sync_low(u100_ctx);
	complete_all(&u100_ctx->tx_done_cmpl);

	if (ret < 0)
		goto exit;
	if (!valid_header(rx_data_buff))
		goto exit;
	recv_len = get_uci_payload_len(rx_data_buff);
	if (recv_len + UCI_HEAD_SIZE > send_len) {
		recv_rest_len = recv_len + UCI_HEAD_SIZE - send_len;
		recv_rest_align_len = ALIGN(recv_rest_len, 4);
		rx_rest_data_buff = kzalloc(recv_rest_align_len, GFP_KERNEL);
		if (!rx_rest_data_buff)
			goto exit;
		ret = uwb_spi_recv(u100_ctx, rx_rest_data_buff, recv_rest_align_len);
		if (ret < 0)
			goto exit;
	}

	rx_skb = alloc_skb(recv_len + UCI_HEAD_SIZE, GFP_KERNEL);
	if (!rx_skb) {
		UWB_ERR("Failed to allocate sk buff (msg size:%d)\n",
			recv_len + UCI_HEAD_SIZE);
		goto exit;
	}
	skb_put(rx_skb, recv_len + UCI_HEAD_SIZE);
	if (!rx_rest_data_buff)
		memcpy(rx_skb->data, rx_data_buff, recv_len + UCI_HEAD_SIZE);
	else {
		memcpy(rx_skb->data, rx_data_buff, send_len);
		memcpy(rx_skb->data + send_len, rx_rest_data_buff, recv_rest_len);
	}
	u100_ctx->recv_package(u100_ctx, rx_skb);

exit:
	kfree(rx_data_buff);
	kfree(rx_rest_data_buff);
	kfree(tx_data_buff);
}

irqreturn_t rx_tsk_work(int irq, void *data)
{
	struct u100_ctx *u100_ctx = data;
	struct sk_buff_head *txq = &u100_ctx->sk_tx_q;

	if (atomic_read(&u100_ctx->flashing_fw)) {
		handle_fw_ap_send(u100_ctx);
		return IRQ_HANDLED;
	}

	if (skb_queue_empty(txq)) {
		/* no data in txq, only receive */
		handle_receive_only(u100_ctx);
	} else {
		/* need full duplex */
		/* first get send data from tx queue */
		handle_full_duplex(u100_ctx);
	}
	return IRQ_HANDLED;
}

static void link_clear_txqueue(struct u100_ctx *u100_ctx)
{
	skb_queue_purge(&u100_ctx->sk_tx_q);
}

static int link_add_txqueue(struct u100_ctx *u100_ctx, char *buff, unsigned int size)
{
	struct sk_buff *tx_skb;
	struct sk_buff_head *txq = &u100_ctx->sk_tx_q;
	int uci_send_len;
	int payload_len;

	if (size < UCI_HEAD_SIZE)
		return -EPERM;
	payload_len = get_uci_payload_len(buff);
	if (payload_len != size - UCI_HEAD_SIZE) {
		if (drop_uci_pkt_on_format_error) {
			print_hex_dump(KERN_ERR, LOG_TAG "Host UCI: ",
				DUMP_PREFIX_NONE, 16, 1, buff, size, false);
			UWB_ERR("Uci packet will be dropped due to format error, size[%u]",
					size);
			return -EBADMSG;
		}
		print_hex_dump(KERN_WARNING, LOG_TAG "Host UCI: ",
				DUMP_PREFIX_NONE, 16, 1, buff, size, false);
		UWB_WARN("Uci packet format error, size[%u]", size);
	}
	uci_send_len = size;

	tx_skb = alloc_skb(uci_send_len, GFP_KERNEL);
	if (!tx_skb) {
		UWB_ERR("%s: Failed to allocate sk_buff (msg size:%d)\n", __func__, uci_send_len);
		return -ENOMEM;
	}
	skb_put(tx_skb, uci_send_len);
	memcpy(tx_skb->data, buff, uci_send_len);

	if (unlikely(txq->qlen >= MAX_RXQ_DEPTH)) {
		UWB_ERR("Tx queue overflow: total %d, packet will be dropped",
				MAX_RXQ_DEPTH);
		dev_kfree_skb_any(tx_skb);
		return -EAGAIN;
	}
	skb_queue_tail(txq, tx_skb);

	return 0;
}

static int link_send_tx(struct u100_ctx *u100_ctx)
{
	int ret;
	unsigned int irq_count_before, irq_count_after;

	if (get_gpio_value(&u100_ctx->gpio_u100_sync)
		&& (atomic_read(&u100_ctx->u100_enter_download) == 0)) {
		set_gpio_value(&u100_ctx->gpio_u100_sync, 0);
		udelay(100);
	}

	u100_ctx->is_bhalf_entered = false;
	irq_count_before = get_irq_count(u100_ctx->gpio_u100_irq.u100_irq);
	set_gpio_value(&u100_ctx->gpio_u100_sync, 1);

	ret = wait_for_completion_timeout(&u100_ctx->irq_done_cmpl,
			msecs_to_jiffies(irq_receive_timeout_ms));
	irq_count_after = get_irq_count(u100_ctx->gpio_u100_irq.u100_irq);
	if (ret == 0) {
		wakeup_sync_low(u100_ctx);
		UWB_WARN("Wait interrupt timeout for %dms", irq_receive_timeout_ms);
		UWB_WARN("Irq count before [%d], after [%d]", irq_count_before, irq_count_after);
		return -ETIMEDOUT;
	}

	ret = wait_for_completion_timeout(&u100_ctx->tx_done_cmpl,
			msecs_to_jiffies(spi_send_timeout_ms));
	wakeup_sync_low(u100_ctx);
	if (ret == 0) {
		if (!u100_ctx->is_bhalf_entered) {
			UWB_WARN("Irq received, not enter irq's Bottom Half");
			return -EBUSY;
		}
		UWB_WARN("Irq received, send not finished");
		return -ETIME;
	}

	return 0;
}

static void on_send_timeout(struct u100_ctx *u100_ctx)
{
	UWB_WARN("Send UCI timeout %d ms * %d times", irq_receive_timeout_ms, uci_retry_count + 1);
	link_clear_txqueue(u100_ctx);
	if (force_crash_on_uci_timeout) {
		link_add_txqueue(u100_ctx, VENDOR_CMD_CRASH, sizeof(VENDOR_CMD_CRASH));
		if (link_send_tx(u100_ctx) < 0) {
			UWB_WARN("Force vendor crash timeout, force reset");
			uwbs_reset(u100_ctx);
			return;
		}
		UWB_ERR("Force vendor crash done since UCI timeout");
	} else {
		UWB_WARN("Uci timeout, force reset");
		uwbs_reset(u100_ctx);
	}
}

int link_send_package(struct u100_ctx *u100_ctx, char *buff, unsigned int size)
{
	int retry = 0;
	int ret = link_add_txqueue(u100_ctx, buff, size);

	if (ret)
		return ret;

	UWB_DEBUG("Add send packet to TX queue and prepare to wakeup U100\n");
	reinit_completion(&u100_ctx->irq_done_cmpl);
	reinit_completion(&u100_ctx->tx_done_cmpl);

	do {
		ret = link_send_tx(u100_ctx);
		retry++;
	} while (-ETIMEDOUT == ret && retry <= uci_retry_count);

	if (-ETIMEDOUT == ret)
		on_send_timeout(u100_ctx);

	return ret;
}

