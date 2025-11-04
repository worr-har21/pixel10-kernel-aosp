/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Google LLC
 */

#ifndef _GOOG_MBA_CTRL_PRIV_H_
#define _GOOG_MBA_CTRL_PRIV_H_

#define MAX_Q_PAYLOAD_WORDS 4
#define MAX_NQ_PAYLOAD_WORDS 4

#define CLIENT_IRQ_TRIG_OFFSET   0x0
#define SET_HOST_IRQ		 0x1

#define CLIENT_IRQ_CONFIG_OFFSET 0x4
#define ENABLE_HOST_AUTO_ACK	 BIT(8)
#define CLIENT_IRQ_MASK_MSG_INT	 BIT(16)
#define CLIENT_IRQ_MASK_ACK_INT	 BIT(24)

#define CLIENT_IRQ_STATUS_OFFSET  0x8
#define CLIENT_IRQ_STATUS_MSG_INT 0x1
#define CLIENT_IRQ_STATUS_ACK_INT 0x100

#define CLIENT_OUTSTANDING_MSG	0x10

#define GLOBAL_NUM_MSG_REG_OFFSET(x) (0x10 + ((x) * 4))
#define GLOBAL_MBA_IP_VER_OFFSET 4

#define MAX_MBA_CHANNELS 1
#define NR_PHANDLE_ARG_COUNT 1

#define CMN_MSG_OFFSET_20 0x20
#define CMN_MSG_OFFSET_100 0x100

#define MBA_IP_MAJOR_VER_1 0x1

#define MBA_IP_DT_PROP_MAX_CNT   2
#define MBA_IP_MAJOR_VER_SHIFT   24

#define MAX_MBOX_CTRL_NAME 64

struct goog_mba_ctrl_info {
	struct device *dev;

	void __iomem *base;
	int irq;
	struct regmap *global_reg;
	u32 cmn_msg_offset;

	struct mbox_controller mbox;

	u32 msg_buf_size; /* size of the common message registers (in words) */
	u32 payload_size; /* size of the protocol payload (in words) */
	u32 *payload; /* message payload received from remote */

	bool queue_mode;

	/*
	 * protect queue operation including tx_q_rd_ptr, tx_q_wr_ptr, tx_q_size and tx_q_capacity.
	 */
	spinlock_t lock;
	u32 tx_q_rd_ptr;
	u32 tx_q_wr_ptr;
	u32 tx_q_size;
	u32 tx_q_capacity;

	u32 rx_q_rd_ptr;
	u32 rx_q_capacity;
};

#endif /* _GOOG_MBA_CTRL_PRIV_H_ */
