/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM goog_mba_ctrl

#if !defined(_GOOG_MBA_CTRL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _GOOG_MBA_CTRL_H

#include <linux/tracepoint.h>

#include "goog-mba-ctrl.h"

TRACE_EVENT(
	goog_mba_ctrl_process_nq_txdone,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info),

	TP_ARGS(mbox_info),

	TP_STRUCT__entry(
		__array(char, dev_name, MAX_MBOX_CTRL_NAME)
	),

	TP_fast_assign(
		scnprintf(__entry->dev_name, MAX_MBOX_CTRL_NAME, "%s", dev_name(mbox_info->dev));
	),

	TP_printk("%s", __entry->dev_name)
);

TRACE_EVENT(
	goog_mba_ctrl_process_q_txdone,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info, u32 reqs_completed, u32 outstanding_msgs),

	TP_ARGS(mbox_info, reqs_completed, outstanding_msgs),

	TP_STRUCT__entry(
		__field(u32, tx_q_rd_ptr)
		__field(u32, outstanding_msgs)
		__field(u32, reqs_completed)
	),

	TP_fast_assign(
		__entry->tx_q_rd_ptr = mbox_info->tx_q_rd_ptr;
		__entry->outstanding_msgs = outstanding_msgs;
		__entry->reqs_completed = reqs_completed;
	),

	TP_printk("tx_q_rd_ptr=%u reqs_completed=%u outstanding_msgs=%u",
		  __entry->tx_q_rd_ptr,
		  __entry->reqs_completed, __entry->outstanding_msgs)
);

TRACE_EVENT(
	goog_mba_ctrl_send_data_nq,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info, u32 *data),

	TP_ARGS(mbox_info, data),

	TP_STRUCT__entry(
		__array(char, dev_name, MAX_MBOX_CTRL_NAME)
		__array(u32, data, MAX_NQ_PAYLOAD_WORDS)
	),

	TP_fast_assign(
		int payload_size = min(mbox_info->payload_size, MAX_NQ_PAYLOAD_WORDS);

		scnprintf(__entry->dev_name, MAX_MBOX_CTRL_NAME, "%s", dev_name(mbox_info->dev));
		memcpy(__entry->data, data, payload_size * sizeof(*data))
	),

	TP_printk("%s: %#010x %#010x %#010x %#010x", __entry->dev_name,
		  __entry->data[0], __entry->data[1], __entry->data[2], __entry->data[3])
);

TRACE_EVENT(
	goog_mba_ctrl_send_data_q,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info, u32 *data),

	TP_ARGS(mbox_info, data),

	TP_STRUCT__entry(
		__field(u32, tx_q_wr_ptr)
		__array(u32, data, MAX_Q_PAYLOAD_WORDS)
	),

	TP_fast_assign(
		__entry->tx_q_wr_ptr = mbox_info->tx_q_wr_ptr;
		memcpy(__entry->data, data, MAX_Q_PAYLOAD_WORDS * sizeof(*data));
	),

	TP_printk("%#010x %#010x %#010x %#010x tx_q_wr_ptr=%u",
		  __entry->data[0], __entry->data[1], __entry->data[2], __entry->data[3],
		  __entry->tx_q_wr_ptr)
);

TRACE_EVENT(
	goog_mba_ctrl_process_nq_rx,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info, u32 *data),

	TP_ARGS(mbox_info, data),

	TP_STRUCT__entry(
		__field(u32, rx_q_rd_ptr)
		__array(u32, data, MAX_Q_PAYLOAD_WORDS)
		__array(char, dev_name, MAX_MBOX_CTRL_NAME)
	),

	TP_fast_assign(
		__entry->rx_q_rd_ptr = mbox_info->rx_q_rd_ptr;
		memcpy(__entry->data, data, MAX_Q_PAYLOAD_WORDS * sizeof(*data));
		scnprintf(__entry->dev_name, MAX_MBOX_CTRL_NAME, "%s", dev_name(mbox_info->dev));
	),

	TP_printk("%s: %#010x %#010x %#010x %#010x", __entry->dev_name,
		  __entry->data[0], __entry->data[1], __entry->data[2], __entry->data[3])
);

TRACE_EVENT(
	goog_mba_ctrl_process_q_rx,

	TP_PROTO(struct goog_mba_ctrl_info *mbox_info, u32 *data),

	TP_ARGS(mbox_info, data),

	TP_STRUCT__entry(
		__field(u32, rx_q_rd_ptr)
		__array(u32, data, MAX_Q_PAYLOAD_WORDS)
	),

	TP_fast_assign(
		__entry->rx_q_rd_ptr = mbox_info->rx_q_rd_ptr;
		memcpy(__entry->data, data, MAX_Q_PAYLOAD_WORDS * sizeof(*data));
	),

	TP_printk("%#010x %#010x %#010x %#010x rx_q_rd_ptr=%u",
		  __entry->data[0], __entry->data[1], __entry->data[2], __entry->data[3],
		  __entry->rx_q_rd_ptr)
);

#endif /* _GOOG_MBA_CTRL_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../drivers/mailbox
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE goog-mba-ctrl-trace
#include <trace/define_trace.h>
