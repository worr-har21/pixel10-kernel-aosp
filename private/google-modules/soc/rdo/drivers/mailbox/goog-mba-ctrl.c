// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google MailBox Array (MBA) Driver
 *
 * Copyright (c) 2023 Google LLC
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/spinlock.h>

#include "goog-mba-ctrl.h"

#define CREATE_TRACE_POINTS
#include "goog-mba-ctrl-trace.h"

static inline u32 mba_readl(struct goog_mba_ctrl_info *mba_info, u32 offset)
{
	return readl(mba_info->base + offset);
}

static inline void mba_writel(struct goog_mba_ctrl_info *mba_info, u32 val, u32 offset)
{
	writel(val, mba_info->base + offset);
}

static inline int goog_mba_ctrl_get_msg_reg_off(struct goog_mba_ctrl_info *mbox_info,
						u32 wd_idx)
{
	return mbox_info->cmn_msg_offset + (wd_idx * sizeof(u32));
}

static int goog_mba_ctrl_set_cmn_msg_offset(struct platform_device *pdev,
					    struct goog_mba_ctrl_info *mbox_info)
{
	const char *mbox_prop_name = "mba-ip-version";
	int ret = 0, cnt;
	struct device *dev = &pdev->dev;
	u32 mba_ip_major_ver, val;
	bool read_ip_major_ver_from_regmap = false;

	cnt = of_property_count_elems_of_size(dev->of_node, mbox_prop_name, sizeof(u32));
	if (cnt > MBA_IP_DT_PROP_MAX_CNT) {
		dev_err(dev, "%s: DT property %s element count exceeded(%d) ret: %d",
			__func__, mbox_prop_name, cnt, -EOVERFLOW);
		return -EOVERFLOW;
	} else if (cnt < 0) {
		read_ip_major_ver_from_regmap = true;
	} else {
		/*
		 * for differentiating between cmn_msg_offset, we only need the major version part
		 * of the MBA IP Version, hence index = 0
		 */
		ret = of_property_read_u32_index(dev->of_node, mbox_prop_name, 0,
						 &mba_ip_major_ver);
		switch (ret) {
		case 0:
			break;
		case -EINVAL:
			read_ip_major_ver_from_regmap = true;
			break;
		case -EOVERFLOW:
		case -ENODATA:
		default:
			dev_err(dev, "%s: DT property %s ret: %d",
				__func__, mbox_prop_name, ret);
			return ret;
		}
	}

	if (read_ip_major_ver_from_regmap) {
		/*
		 * DT property is not available, read the MBA IP version from the global
		 * register
		 */
		ret = regmap_read(mbox_info->global_reg, GLOBAL_MBA_IP_VER_OFFSET, &val);
		mba_ip_major_ver = (val >> MBA_IP_MAJOR_VER_SHIFT);
	}

	if (mba_ip_major_ver == MBA_IP_MAJOR_VER_1)
		mbox_info->cmn_msg_offset = CMN_MSG_OFFSET_20;
	else
		mbox_info->cmn_msg_offset = CMN_MSG_OFFSET_100;

	return 0;
};

/* clear all the pending interrupts */
static void goog_mba_ctrl_clr_intrs(struct goog_mba_ctrl_info *mbox_info)
{
	u32 irq_status;

	irq_status = mba_readl(mbox_info, CLIENT_IRQ_STATUS_OFFSET);
	irq_status &= CLIENT_IRQ_STATUS_MSG_INT | CLIENT_IRQ_STATUS_ACK_INT;

	mba_writel(mbox_info, irq_status, CLIENT_IRQ_STATUS_OFFSET);
}

/*
 * Reads payload data from the mailbox controller into the designated payload slot.
 *
 * Returns the offset of the payload within the payload buffer.
 */
static u32 goog_mba_ctrl_rd_payload(struct goog_mba_ctrl_info *mbox_info)
{
	u32 wd_idx;
	u32 payload_off = 0;

	if (mbox_info->queue_mode)
		payload_off = mbox_info->rx_q_rd_ptr * mbox_info->payload_size;

	for (wd_idx = 0; wd_idx < mbox_info->payload_size; wd_idx++)
		mbox_info->payload[payload_off + wd_idx] = mba_readl(mbox_info,
			goog_mba_ctrl_get_msg_reg_off(mbox_info, payload_off + wd_idx));

	return payload_off;
}

static void goog_mba_ctrl_wr_payload(struct goog_mba_ctrl_info *mbox_info,
				     u32 *data)
{
	u32 wd_idx;
	u32 payload_off = 0;

	if (mbox_info->queue_mode)
		payload_off = mbox_info->tx_q_wr_ptr * mbox_info->payload_size;

	for (wd_idx = 0; wd_idx < mbox_info->payload_size; wd_idx++)
		mba_writel(mbox_info, data[wd_idx],
			goog_mba_ctrl_get_msg_reg_off(mbox_info, payload_off + wd_idx));
}

static void goog_mba_ctrl_init_q_state(struct goog_mba_ctrl_info *info, int num_chans)
{
	info->tx_q_rd_ptr = 0;
	info->tx_q_wr_ptr = 0;
	info->tx_q_size = 0;
	info->tx_q_capacity = num_chans;

	info->rx_q_rd_ptr = 0;
	info->rx_q_capacity = num_chans;
}

#define GOOG_MBA_Q_XPORT_INIT_PROTO_VAL 0x100
static bool goog_mba_ctrl_check_q_reset(struct goog_mba_ctrl_info *mbox_info)
{
	if (mbox_info->tx_q_rd_ptr == 0) {
		u32 val = mba_readl(mbox_info, goog_mba_ctrl_get_msg_reg_off(mbox_info, 0));

		if (val == GOOG_MBA_Q_XPORT_INIT_PROTO_VAL)
			return true;
	}

	return false;
}

static int goog_mba_ctrl_get_chan_idx(struct goog_mba_ctrl_info *mbox_info,
				      struct mbox_chan *chan)
{
	int chan_idx;

	for (chan_idx = 0; chan_idx < mbox_info->mbox.num_chans; chan_idx++) {
		if (&mbox_info->mbox.chans[chan_idx] == chan)
			return chan_idx;
	}

	return -EINVAL;
}

static void goog_mba_ctrl_process_nq_txdone(struct goog_mba_ctrl_info *mbox_info)
{
	trace_goog_mba_ctrl_process_nq_txdone(mbox_info);
	mbox_chan_txdone(&mbox_info->mbox.chans[0], 0);
}

static void goog_mba_ctrl_process_q_txdone(struct goog_mba_ctrl_info *mbox_info)
{
	struct mbox_chan *chan;
	u32 outstanding_msgs;
	u32 reqs_completed;
	u32 chan_idx;

	spin_lock(&mbox_info->lock);
	outstanding_msgs = mba_readl(mbox_info, CLIENT_OUTSTANDING_MSG);
	reqs_completed = mbox_info->tx_q_size - outstanding_msgs;
	spin_unlock(&mbox_info->lock);

	while (reqs_completed) {
		bool q_reset;

		spin_lock(&mbox_info->lock);
		chan_idx = mbox_info->tx_q_rd_ptr;
		chan = &mbox_info->mbox.chans[chan_idx];

		trace_goog_mba_ctrl_process_q_txdone(mbox_info, reqs_completed, outstanding_msgs);
		q_reset = goog_mba_ctrl_check_q_reset(mbox_info);
		if (!q_reset) {
			mbox_info->tx_q_rd_ptr = (mbox_info->tx_q_rd_ptr + 1) %
						  mbox_info->tx_q_capacity;
			mbox_info->tx_q_size--;
			reqs_completed--;
		} else {
			goog_mba_ctrl_init_q_state(mbox_info, mbox_info->mbox.num_chans);
		}
		spin_unlock(&mbox_info->lock);

		mbox_chan_txdone(chan, 0);

		if (q_reset)
			break;
	}
}

static void goog_mba_ctrl_process_nq_rx(struct goog_mba_ctrl_info *mbox_info, void *msg)
{
	trace_goog_mba_ctrl_process_nq_rx(mbox_info, msg);
	mbox_chan_received_data(&mbox_info->mbox.chans[0], msg);
}

static void goog_mba_ctrl_process_q_rx(struct goog_mba_ctrl_info *mbox_info)
{
	struct mbox_chan *chan;
	u32 chan_idx;
	u32 payload_off;

	chan_idx = mbox_info->rx_q_rd_ptr;
	chan = &mbox_info->mbox.chans[chan_idx];

	payload_off = goog_mba_ctrl_rd_payload(mbox_info);
	trace_goog_mba_ctrl_process_q_rx(mbox_info, &mbox_info->payload[payload_off]);

	mbox_info->rx_q_rd_ptr = (mbox_info->rx_q_rd_ptr + 1) % mbox_info->rx_q_capacity;
	mbox_chan_received_data(chan, &mbox_info->payload[payload_off]);
}

static irqreturn_t goog_mba_ctrl_isr(int irq, void *dev)
{
	struct goog_mba_ctrl_info *mbox_info = dev_get_drvdata(dev);
	u32 irq_status;

	irq_status = mba_readl(mbox_info, CLIENT_IRQ_STATUS_OFFSET);

	if (!irq_status)
		return IRQ_NONE;

	if (irq_status & CLIENT_IRQ_STATUS_ACK_INT) {
		/*
		 * ACK interrupt needs to be cleared before handling tx_done()
		 * so that the next ack is not missed while the current ACK is getting processed.
		 */
		mba_writel(mbox_info, CLIENT_IRQ_STATUS_ACK_INT, CLIENT_IRQ_STATUS_OFFSET);

		if (!mbox_info->queue_mode)
			goog_mba_ctrl_process_nq_txdone(mbox_info);
		else
			goog_mba_ctrl_process_q_txdone(mbox_info);
	}

	if (irq_status & CLIENT_IRQ_STATUS_MSG_INT) {
		if (!mbox_info->queue_mode) {
			u32 payload_off = goog_mba_ctrl_rd_payload(mbox_info);

			/*
			 * For use case of GDMC, we need to make sure we clear irq register after
			 * data copy to protect data integraty.
			 * For AoC use case, MSG interrupt is used to send notification
			 * regardless client site state.
			 * b388309333: report a latency limitation that we should release irq line
			 * ASAP otherwise next coming MSG interrupt will be lost.
			 */
			mba_writel(mbox_info, CLIENT_IRQ_STATUS_MSG_INT, CLIENT_IRQ_STATUS_OFFSET);
			goog_mba_ctrl_process_nq_rx(mbox_info, &mbox_info->payload[payload_off]);
		} else {
			/*
			 * TODO: move time to clear MSG interrupt earlier before process client's
			 * callback after data copy out.
			 */
			goog_mba_ctrl_process_q_rx(mbox_info);
			mba_writel(mbox_info, CLIENT_IRQ_STATUS_MSG_INT, CLIENT_IRQ_STATUS_OFFSET);
		}
	}

	return IRQ_HANDLED;
}

static void goog_mba_ctrl_trigger_host_irq(struct goog_mba_ctrl_info *mbox_info)
{
	mba_writel(mbox_info, SET_HOST_IRQ, CLIENT_IRQ_TRIG_OFFSET);
}

static void goog_mba_ctrl_disable_client_irq(struct goog_mba_ctrl_info *mbox_info)
{
	u32 val;

	val = mba_readl(mbox_info, CLIENT_IRQ_CONFIG_OFFSET);
	val &= ~(CLIENT_IRQ_MASK_MSG_INT | CLIENT_IRQ_MASK_ACK_INT | ENABLE_HOST_AUTO_ACK);

	mba_writel(mbox_info, val, CLIENT_IRQ_CONFIG_OFFSET);
}

static void goog_mba_ctrl_enable_client_irq(struct goog_mba_ctrl_info *mbox_info)
{
	u32 val;

	val = mba_readl(mbox_info, CLIENT_IRQ_CONFIG_OFFSET);
	val |= (CLIENT_IRQ_MASK_MSG_INT | CLIENT_IRQ_MASK_ACK_INT | ENABLE_HOST_AUTO_ACK);

	mba_writel(mbox_info, val, CLIENT_IRQ_CONFIG_OFFSET);
}

static int goog_mba_ctrl_send_data_nq(struct goog_mba_ctrl_info *mbox_info, void *data)
{
	if (data) {
		goog_mba_ctrl_wr_payload(mbox_info, data);
		trace_goog_mba_ctrl_send_data_nq(mbox_info, data);
	}

	goog_mba_ctrl_trigger_host_irq(mbox_info);

	return 0;
}

static int goog_mba_ctrl_send_data_q(struct goog_mba_ctrl_info *mbox_info,
				     struct mbox_chan *chan, void *data)
{
	int chan_idx;
	unsigned long flags;
	int ret = 0;

	chan_idx = goog_mba_ctrl_get_chan_idx(mbox_info, chan);
	if (chan_idx < 0) {
		dev_err(mbox_info->dev, "chan invalid, ret %d", chan_idx);
		return chan_idx;
	}

	spin_lock_irqsave(&mbox_info->lock, flags);
	if (mbox_info->tx_q_size == mbox_info->tx_q_capacity) {
		dev_err(mbox_info->dev, "mailbox queue is full");
		ret = -EBUSY;
		goto exit;
	}

	if (mbox_info->tx_q_wr_ptr != chan_idx) {
		dev_err(mbox_info->dev, "chan_idx (%d) != tx_q_wr_ptr (%d)",
			chan_idx, mbox_info->tx_q_wr_ptr);
		ret = -EPROTO;
		goto exit;
	}

	if (data) {
		goog_mba_ctrl_wr_payload(mbox_info, data);
		trace_goog_mba_ctrl_send_data_q(mbox_info, data);

		mbox_info->tx_q_wr_ptr = (mbox_info->tx_q_wr_ptr + 1) % mbox_info->tx_q_capacity;
		mbox_info->tx_q_size++;
	}

	goog_mba_ctrl_trigger_host_irq(mbox_info);
exit:
	spin_unlock_irqrestore(&mbox_info->lock, flags);

	return ret;
}

static int goog_mba_ctrl_send_data(struct mbox_chan *chan, void *data)
{
	struct goog_mba_ctrl_info *mbox_info = dev_get_drvdata(chan->mbox->dev);
	int ret;

	if (!mbox_info->queue_mode)
		ret = goog_mba_ctrl_send_data_nq(mbox_info, data);
	else
		ret = goog_mba_ctrl_send_data_q(mbox_info, chan, data);

	return ret;
}

static int goog_mba_ctrl_startup(struct mbox_chan *chan)
{
	struct goog_mba_ctrl_info *mbox_info = dev_get_drvdata(chan->mbox->dev);

	goog_mba_ctrl_enable_client_irq(mbox_info);

	return 0;
}

static void goog_mba_ctrl_shutdown(struct mbox_chan *chan)
{
	struct goog_mba_ctrl_info *mbox_info = dev_get_drvdata(chan->mbox->dev);

	goog_mba_ctrl_disable_client_irq(mbox_info);
	synchronize_irq(mbox_info->irq);
}

static struct mbox_chan_ops goog_mba_ctrl_chan_ops = {
	.send_data = goog_mba_ctrl_send_data,
	.startup = goog_mba_ctrl_startup,
	.shutdown = goog_mba_ctrl_shutdown,
};

static int goog_mba_ctrl_get_num_chans(struct goog_mba_ctrl_info *info, bool queue_mode,
				       u32 msg_buf_size, u32 payload_size)
{
	if (!queue_mode)
		return 1;

	if ((msg_buf_size % payload_size) != 0) {
		dev_err(info->dev, "msg_buf_size (%u) is not multiple of payload_size (%u)",
			msg_buf_size, payload_size);
		return -EINVAL;
	}

	return (msg_buf_size / payload_size);
}

static int goog_mba_ctrl_ioremap(struct goog_mba_ctrl_info *mbox_info,
				 struct platform_device *pdev)
{
	struct device *dev = mbox_info->dev;

	mbox_info->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mbox_info->base)) {
		dev_err(dev, "Failed to get and ioremap region. (ret: %ld)\n",
			PTR_ERR(mbox_info->base));
		return PTR_ERR(mbox_info->base);
	}

	return 0;
}

static int goog_mba_ctrl_get_msg_buf_size(struct goog_mba_ctrl_info *mbox_info)
{
	int ret;
	struct device *dev = mbox_info->dev;
	unsigned int msg_reg_offset;
	unsigned int out_args[NR_PHANDLE_ARG_COUNT];
	u32 msg_buf_size;

	mbox_info->global_reg =
		syscon_regmap_lookup_by_phandle_args(dev->of_node, "google,syscon-phandle",
						     NR_PHANDLE_ARG_COUNT, out_args);
	if (IS_ERR(mbox_info->global_reg)) {
		dev_err(dev, "Failed to create regmap from phandle. (ret: %ld)\n",
			PTR_ERR(mbox_info->global_reg));
		return PTR_ERR(mbox_info->global_reg);
	}

	msg_reg_offset = GLOBAL_NUM_MSG_REG_OFFSET(out_args[0]);
	if (msg_reg_offset >= PAGE_SIZE) {
		dev_err(dev, "Offset size bigger than one page. (offset: 0x%x)\n", msg_reg_offset);
		return -EINVAL;
	}

	ret = regmap_read(mbox_info->global_reg, msg_reg_offset, &msg_buf_size);
	if (ret) {
		dev_err(dev, "Failed to read from regmap for channel#%d. (ret: %d)",
			out_args[0], ret);
		return ret;
	}

	mbox_info->msg_buf_size = msg_buf_size;

	return 0;
}

static int goog_mba_ctrl_alloc_payload_buf(struct goog_mba_ctrl_info *mbox_info)
{
	int ret;
	struct device *dev = mbox_info->dev;
	u32 payload_size;

	ret = of_property_read_u32(dev->of_node, "payload-size", &payload_size);
	if (!ret)
		mbox_info->payload_size = min(mbox_info->msg_buf_size, payload_size);
	else
		mbox_info->payload_size = mbox_info->msg_buf_size;

	/* allocate the payload buffer as large as msg_buf_size to cover the queue mode */
	mbox_info->payload = devm_kcalloc(dev, mbox_info->msg_buf_size,
					  sizeof(*mbox_info->payload), GFP_KERNEL);
	if (!mbox_info->payload)
		return -ENOMEM;

	return 0;
}

static int goog_mba_ctrl_request_irq(struct goog_mba_ctrl_info *mbox_info,
				     struct platform_device *pdev)
{
	struct device *dev = mbox_info->dev;
	int ret;

	mbox_info->irq = platform_get_irq(pdev, 0);
	if (mbox_info->irq < 0) {
		dev_err(dev, "No interrupt for device\n");
		return -EINVAL;
	}

	ret = devm_request_irq(dev, mbox_info->irq, goog_mba_ctrl_isr,
			       IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
			       dev_name(dev), dev);
	if (ret != 0) {
		dev_err(dev, "failed to register interrupt handler: %d\n", ret);
		return -ENXIO;
	}

	return 0;
}

static int goog_mba_ctrl_probe(struct platform_device *pdev)
{
	struct goog_mba_ctrl_info *mbox_info;
	struct mbox_controller *mbox;
	struct mbox_chan *channels;
	int ret;
	int i;
	struct device *dev = &pdev->dev;

	mbox_info = devm_kzalloc(dev, sizeof(*mbox_info), GFP_KERNEL);
	if (!mbox_info)
		return -ENOMEM;

	mbox_info->dev = dev;
	spin_lock_init(&mbox_info->lock);

	ret = goog_mba_ctrl_ioremap(mbox_info, pdev);
	if (ret)
		return ret;

	ret = goog_mba_ctrl_get_msg_buf_size(mbox_info);
	if (ret)
		return ret;

	ret = goog_mba_ctrl_alloc_payload_buf(mbox_info);
	if (ret)
		return ret;

	mbox_info->queue_mode = of_property_read_bool(dev->of_node, "queue-mode");

	if (mbox_info->queue_mode && (mbox_info->payload_size == 0)) {
		dev_err(dev, "%s: queue mode doesn't support zero payload_size\n", __func__);
		return -EINVAL;
	}

	mbox = &mbox_info->mbox;
	mbox->txdone_irq = true;
	mbox->dev = dev;
	mbox->ops = &goog_mba_ctrl_chan_ops;
	ret = goog_mba_ctrl_get_num_chans(mbox_info, mbox_info->queue_mode,
					  mbox_info->msg_buf_size,
					  mbox_info->payload_size);
	if (ret < 0)
		return ret;

	mbox->num_chans = ret;
	channels = devm_kcalloc(dev, mbox->num_chans, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	mbox->chans = channels;

	for (i = 0; i < mbox->num_chans; i++)
		channels[i].mbox = mbox;

	if (mbox_info->queue_mode)
		goog_mba_ctrl_init_q_state(mbox_info, mbox->num_chans);

	ret = goog_mba_ctrl_set_cmn_msg_offset(pdev, mbox_info);
	if (ret != 0) {
		dev_err(dev, "%s: error setting the cmn_msg_offset: ret: %d", __func__, ret);
		return ret;
	}

	platform_set_drvdata(pdev, mbox_info);

	ret = devm_mbox_controller_register(dev, mbox);
	if (ret != 0) {
		dev_err(dev, "failed to register mailbox controller: %d\n", ret);
		return ret;
	}

	goog_mba_ctrl_disable_client_irq(mbox_info);
	goog_mba_ctrl_clr_intrs(mbox_info);

	ret = goog_mba_ctrl_request_irq(mbox_info, pdev);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id goog_mba_ctrl_match[] = {
	{ .compatible = "google,mba-ctrl" },
	{ /* Sentinel */ }
};

static struct platform_driver goog_mba_ctrl = {
	.probe = goog_mba_ctrl_probe,
	.driver = {
		.name = "goog-mba-ctrl",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(goog_mba_ctrl_match),
	},
};

module_platform_driver(goog_mba_ctrl);

MODULE_DESCRIPTION("Google MailBox Array (MBA) Driver");
MODULE_AUTHOR("Lucas Wei <lucaswei@google.com>");
MODULE_LICENSE("GPL");
