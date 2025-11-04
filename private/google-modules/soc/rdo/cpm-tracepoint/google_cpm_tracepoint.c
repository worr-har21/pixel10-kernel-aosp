// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2022 Google LLC */
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/eventfd.h>
#include <linux/fdtable.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <soc/google/goog_mba_q_xport.h>
#include <soc/google/goog_mba_cpm_iface.h>
#define CPM_SOURCE_ID_CPM_TRACEPOINT 0x2

#include "cpm_tracepoint_decoder.h"
#include "soc/google/google_timestamp_sync.h"

#define CREATE_TRACE_POINTS /* This must to be before including "cpm_trace.h" */
#include "cpm_trace.h"

#define IOCTL_EVENTFD_PARAM_WRITE \
	_IOW('C', 0, struct google_cpm_tp_notif_param *)
#define IOCTL_MMAP_PARAM_READ \
	_IOR('C', 1, struct google_cpm_tp_mmap_param *)
#define IOCTL_RING_BUFF_PARAMS_READ \
	_IOR('C', 2, struct google_cpm_tp_ring_buf_param *)
#define IOCTL_USERSPACE_ACCESS_DONE_WRITE \
	_IOW('C', 3, void *)

#define TP_STR_REQUEST 0
#define TP_REQUEST 1
#define TP_BUFF_ADDR_REQUEST 2
#define TP_ENABLE_PULL_MODE 3

#define TP_PAYLOAD_TABLE_BIT	5

/* Timeouts in ms */
#define MBOX_CLIENT_TX_TOUT	3000
#define MBOX_REQ_TIMEOUT	6000
/* TODO(b/290072650): Adjust the timeout based on real use cases. */
#define USERSPACE_TIMEOUT	10000

#define PAYLOAD_OVERFLOW_FIELD	BIT(8)
#define PAYLOAD_TP_TYPE		GENMASK(7, 0)

/* Error codes in LK */
#define NO_ERROR (0)
#define ERR_NOT_ENOUGH_BUFFER (-9)
#define ERR_NOT_SUPPORTED (-24)

#define CDEV_NAME "cpm-tracepoint"

static const char *const UNREACHABLE_TP_PAYLOAD = "XXX";

/*
 * Following enums and struct must match the ones in
 * "cpm/google/lib/tp/include/lib/tp.h"
 */

enum google_cpm_tp_req_t {
	TP_REQUEST_INFO,
	TP_REQUEST_ERROR,
	TP_REQUEST_DRAM,
	NUM_TP_REQUESTS
};

enum google_cpm_tp_t {
	TYPE_INTEGER,
	TYPE_STRING_INT,
	TYPE_STRING_ADDR,
	NUM_TP_TYPES
};

/*
 * This struct should not directly used to parse data from the CPM.
 * Instead, use "FIELD_GET".
 */
struct google_cpm_tp {
	u64 timestamp : 56;
	u32 error : 1;
	u32 plugin_id : 7;
	u32 arg_type : 8;
	u32 arg_id : 24;
	u32 arg_payload;
} __packed;
static_assert(sizeof(struct google_cpm_tp) == 16);

/* Linked-list node of message payload for queue mode mailbox */
struct google_cpm_tp_payload_node {
	u32 context;
	struct cpm_iface_payload payload;
	struct list_head list;
};

struct google_cpm_tp_cdev_prop {
	struct cdev cdev;
	dev_t dev_num;
	struct class *class;
};

/* Parameters to get an eventfd interface for notifications */
struct google_cpm_tp_notif_param {
	u32 pid;
	u32 eventfd;
} __packed;
static_assert(sizeof(struct google_cpm_tp_notif_param) == 8);

/*
 * Parameters of the tracepoint ring buffers to pass to user-space.
 * This struct is compatible with the user-space because endieness is same.
 */
struct google_cpm_tp_ring_buf_param {
	/* Use u32 instead of bool to intentionally put 31-bit padding */
	u32 overflow;
	u32 type;
	u32 tail;
	u32 count;
} __packed;
static_assert(sizeof(struct google_cpm_tp_ring_buf_param) == 16);

struct google_cpm_tp_mmap_param {
	u64 tp_buff_size;
	u64 tp_str_buff_size;
	u32 tp_str_buff_copied_size;
	u32 tp_str_buff_offset_in_cpm;
} __packed;
static_assert(sizeof(struct google_cpm_tp_mmap_param) == 24);

struct google_cpm_tp_string {
	/*
	 * A page-aligned memory (size: `TP_STR_BUF_SIZE`)
	 * is allocated to `buf` in the probe function.
	 */
	char *buf;
	u32 copied_size;
	u32 offset_in_cpm;
};

struct google_cpm_tp_ring_buf_info {
	void *buffer;
	u32 num_tracepoints;
};

struct google_cpm_tp_dev {
	struct device *dev;

	dma_addr_t dma_phys;
	void *dma_virt;
	u32 dma_size;

	struct google_cpm_tp_string tp_string;
	phys_addr_t tp_buffer_phys;
	struct google_cpm_tp_ring_buf_info ring_buf_info_list[NUM_TP_REQUESTS];

	struct cpm_iface_client *cpm_client;

	u32 remote_ch;

	/* To handle the received payload and reply to CPM */
	struct work_struct received_msg_handle_work;
	/* protects the list of the received payload */
	spinlock_t received_payload_list_lock;
	struct list_head received_payload_list;

	struct google_cpm_tp_cdev_prop cdev_prop;

	/* For notification from the driver to the user space. */
	struct google_cpm_tp_notif_param notif_param;
	bool notif_params_initialized;
	struct eventfd_ctx *event_ctx;

	/* To wait for the userspace to complete reading tracepoints */
	struct completion userspace_completion;

	/* Stores ring buffer params to pass them to the user-space */
	struct google_cpm_tp_ring_buf_param ring_buf_param;
	/* Protects `ring_buf_param` */
	struct mutex ring_buf_param_lock;

	/* debugfs handle */
	struct dentry *debugfs;

	/* status on whether cpm supports the new pull mode commands */
	bool cpm_pull_mode_supported;

	/* tracks the cpm tp operating mode */
	bool cpm_pull_mode;

	/* ignores overflows on first collected tracepoints */
	bool ignore_overflow_on_first_tracepoint;
};

/* This structure should sync with remote tp driver */
struct google_cpm_tp_str_resp_payload {
	__le32 status;
	__le32 size;
	__le32 str_offset;
} __packed;

struct google_cpm_tp_buff_addr_payload {
	__le32 status;
	__le32 addr;
	__le32 size;
} __packed;

struct google_cpm_tp_packed {
	u64 meta;
	u64 arg;
} __packed;
static_assert(sizeof(struct google_cpm_tp_packed) == 16,
	      "struct google_cpm_tp_packed size must be 16-byte.");

#define TP_PLUGIN_ID_FIELD	GENMASK_ULL(63, 57)
#define TP_ERROR_FIELD		BIT_ULL(56)
#define TP_TIMESTAMP_FIELD	GENMASK_ULL(55, 0)
#define TP_ARG_PAYLOAD_FIELD	GENMASK_ULL(63, 32)
#define TP_ARG_ID_FIELD		GENMASK_ULL(31, 8)
#define TP_ARG_TYPE_FIELD	GENMASK_ULL(7, 0)

/*
 * These macros must match the ones in
 * "interfaces/protocols/tracepoint/include/interfaces/protocols/tracepoint/cpm_parameters.h"
 */
#define NUM_BUFFERS 2
#define TRACEPOINTS_SECTION_SIZE (0x8000) /* Each section is 32 KB */

/*
 * These sizes should be aligned with PAGE_SIZE
 * since the buffers are mmap'ed by the user space.
 */
#define TP_BUF_TOTAL_SIZE	(2 * TRACEPOINTS_SECTION_SIZE)
#define TP_STR_BUF_SIZE		SZ_32K
static_assert(PAGE_ALIGNED(TP_BUF_TOTAL_SIZE));
static_assert(PAGE_ALIGNED(TP_STR_BUF_SIZE));

/*
 * Sets the eventfd context.
 * Returns 0 on success, negative code on failure.
 */
static int google_cpm_tp_set_eventfd_ctx(struct google_cpm_tp_dev *cpm_tp)
{
	struct device *dev = cpm_tp->dev;
	struct task_struct *userspace_task;
	struct file *eventfd_filp;
	struct google_cpm_tp_notif_param *notif_param = &cpm_tp->notif_param;

	rcu_read_lock();
	userspace_task = find_task_by_vpid(notif_param->pid);
	rcu_read_unlock();
	if (!userspace_task) {
		dev_err(dev, "Failed to find task with PID %d\n",
			notif_param->pid);
		return -EINVAL;
	}

	rcu_read_lock();
	eventfd_filp = files_lookup_fd_rcu(userspace_task->files,
					   notif_param->eventfd);
	rcu_read_unlock();
	if (!eventfd_filp) {
		dev_err(dev,
			"Failed to find eventfd file with file descriptor: %d\n",
			notif_param->eventfd);
		return -EINVAL;
	}

	cpm_tp->event_ctx = eventfd_ctx_fileget(eventfd_filp);
	if (IS_ERR(cpm_tp->event_ctx)) {
		unsigned long err = PTR_ERR(cpm_tp->event_ctx);

		dev_err(dev, "Failed to get eventfd context: %ld\n", err);
		return (int)err;
	}

	return 0;
}

static void google_cpm_tp_rx_callback(u32 context, void *msg, void *priv_data)
{
	struct google_cpm_tp_dev *cpm_tp = priv_data;
	struct cpm_iface_payload *msg_payload = msg;
	struct google_cpm_tp_payload_node *node;

	dev_dbg(cpm_tp->dev, "rx callback msg 0x%x\n", msg_payload->payload[0]);
	dev_dbg(cpm_tp->dev, "rx callback type 0x%x\n",
		goog_mba_q_xport_get_type(&msg_payload->header));

	if (goog_mba_q_xport_get_type(msg) != GOOG_MBA_Q_XPORT_TYPE_REQUEST) {
		dev_err(cpm_tp->dev,
			"Received message type %u, but not supported.",
			goog_mba_q_xport_get_type(msg));
		return;
	}

	/* "node" will be freed when the response is sent. */
	node = devm_kmalloc(cpm_tp->dev, sizeof(*node), GFP_ATOMIC);
	if (!node)
		return;

	/* Copy received payload to handle it later */
	node->context = context;
	node->payload = *msg_payload;

	/*
	 * Schedule a function that handles received CPM tracepoints,
	 * and sends response message to the CPM.
	 */
	spin_lock(&cpm_tp->received_payload_list_lock);
	list_add_tail(&node->list, &cpm_tp->received_payload_list);
	spin_unlock(&cpm_tp->received_payload_list_lock);
	schedule_work(&cpm_tp->received_msg_handle_work);
}

static void google_cpm_tp_unpack(const struct google_cpm_tp_packed *packed,
				 struct google_cpm_tp *tp)
{
	tp->error = FIELD_GET(TP_ERROR_FIELD, packed->meta);
	tp->plugin_id = FIELD_GET(TP_PLUGIN_ID_FIELD, packed->meta);
	tp->timestamp = FIELD_GET(TP_TIMESTAMP_FIELD, packed->meta);
	tp->arg_type = FIELD_GET(TP_ARG_TYPE_FIELD, packed->arg);
	tp->arg_id = FIELD_GET(TP_ARG_ID_FIELD, packed->arg);
	tp->arg_payload = FIELD_GET(TP_ARG_PAYLOAD_FIELD, packed->arg);
}

/*
 * Returns a pointer to a string corresponding to the passed payload address.
 * If the resolved pointer points to somewhere in the tracepoint string buffer,
 * then the function returns the resolved pointer.
 * Otherwise, it means that the address is unreachable from the AP,
 * then the function returns a pointer to the placeholder text.
 */
const char *google_cpm_tp_resolve_payload_str(struct google_cpm_tp_dev *cpm_tp,
					      u32 payload_addr_in_cpm)
{
	struct google_cpm_tp_string *tp_string = &cpm_tp->tp_string;
	u32 arg_payload_offset;

	if (payload_addr_in_cpm < tp_string->offset_in_cpm) {
		/* Since offset will be negative, returning placeholder */
		return UNREACHABLE_TP_PAYLOAD;
	}

	/*
	 * payload_addr_in_cpm: String offset in CPM
	 * offset_in_cpm: Start address of the string section in CPM
	 */
	arg_payload_offset = payload_addr_in_cpm - tp_string->offset_in_cpm;
	if (arg_payload_offset > tp_string->copied_size) {
		/* Since offset is out of bounds, returning placeholder */
		return UNREACHABLE_TP_PAYLOAD;
	}

	return tp_string->buf + arg_payload_offset;
}

/*
 * Prepare for notification from the driver to the user space using an eventfd.
 * This function successds only after the user space passes the eventfd parameters.
 * Returns 0 on success, a negative error code on failure.
 */
static int google_cpm_tp_prepare_eventfd(struct google_cpm_tp_dev *cpm_tp)
{
	struct google_cpm_tp_notif_param *notif_param = &cpm_tp->notif_param;
	int ret;

	dev_dbg(cpm_tp->dev, "Setting eventfd ctx for PID: %u, eventfd: %u\n",
		notif_param->pid, notif_param->eventfd);

	if (!cpm_tp->notif_params_initialized) {
		dev_dbg(cpm_tp->dev,
			"eventfd params have not been passed yet.\n");
		return -EAGAIN;
	}

	ret = google_cpm_tp_set_eventfd_ctx(cpm_tp);
	if (ret) {
		/*
		 * eventfd params have been passed from the user-space proram
		 * once, but the user-space program seems terminated.
		 * Resets the eventfd context to let the driver revert back to
		 * processing CPM tracepoints in the driver.
		 */
		dev_dbg(cpm_tp->dev,
			"The user-space trace reader seems terminated.\n");
		if (!IS_ERR_OR_NULL(cpm_tp->event_ctx))
			eventfd_ctx_put(cpm_tp->event_ctx);
		cpm_tp->event_ctx = NULL;
		cpm_tp->notif_params_initialized = false;
		return ret;
	}

	return 0;
}

static int google_cpm_tp_send_resp(struct google_cpm_tp_dev *cpm_tp,
				   u32 context,
				   struct cpm_iface_payload *payload)
{
	struct cpm_iface_req client_resp = {
		.msg_type = RESPONSE_MSG,
		.req_msg = payload,
		.tout_ms = MBOX_CLIENT_TX_TOUT,
		.resp_context = context,
	};
	int ret;

	ret = cpm_send_message(cpm_tp->cpm_client, &client_resp);
	if (ret < 0) {
		dev_err(cpm_tp->dev,
			"Failed to send response package (ret: %d)\n", ret);
	}

	return ret;
}

static int __google_cpm_tp_store_and_reply(struct google_cpm_tp_dev *cpm_tp,
					  struct google_cpm_tp_ring_buf_param *ring_buf_param)
{
	static u64 prev_event_timestamp;
	struct device *dev = cpm_tp->dev;
	struct google_cpm_tp_packed *packed_tps;
	u32 num_tracepoints;
	u32 i;
	int ret = 0;
	unsigned long userspace_timeout = msecs_to_jiffies(USERSPACE_TIMEOUT);

	dev_dbg(dev,
		"Tracepoint info:\n"
		"\toverflow: %u, type: %u, tail: %u, count: %u\n",
		ring_buf_param->overflow, ring_buf_param->type,
		ring_buf_param->tail, ring_buf_param->count);

	if (unlikely(ring_buf_param->type >= NUM_TP_REQUESTS)) {
		dev_err(dev, "Unknown tp type: %u\n", ring_buf_param->type);
		return -ESERVERFAULT;
	}

	if (!google_cpm_tp_prepare_eventfd(cpm_tp) &&
	    eventfd_signal_allowed()) {
		/*
		 * Userspace is ready to process tracepoints.
		 * Let the userspace know, and let it process tracepoints.
		 */
		reinit_completion(&cpm_tp->userspace_completion);

		eventfd_signal(cpm_tp->event_ctx, 1);
		dev_dbg(dev, "Sent a signal to user space.\n");

		/* Wait for eventfd signal from user-space */
		ret = wait_for_completion_timeout(&cpm_tp->userspace_completion,
						  userspace_timeout);
		if (ret) {
			dev_dbg(dev,
				"Processed CPM tracepoints in user space.\n");
			return ret;
		}

		dev_err(dev, "User-space access timed out\n");
		/* Revert back to processing tracepoints in the driver */
	}

	packed_tps = cpm_tp->ring_buf_info_list[ring_buf_param->type].buffer;
	num_tracepoints =
		cpm_tp->ring_buf_info_list[ring_buf_param->type].num_tracepoints;

	if (ring_buf_param->overflow)
		dev_warn(dev, "CPM tracepoint buffer overflowed.\n");

	for (i = 0; i < ring_buf_param->count; ++i) {
		struct google_cpm_tp tp;
		enum tracepoint_handle decode_status;
		u32 index = (i + ring_buf_param->tail) % num_tracepoints;
		const char *arg_id_str;
		u64 event_timestamp;

		google_cpm_tp_unpack(&packed_tps[index], &tp);

		arg_id_str =
			google_cpm_tp_resolve_payload_str(cpm_tp, tp.arg_id);

		switch (tp.arg_type) {
		case TYPE_INTEGER:
			event_timestamp = max(goog_gtc_ticks_to_boottime(tp.timestamp),
						prev_event_timestamp);
			decode_status = cpm_tracepoint_decode(arg_id_str,
							      tp.arg_payload,
							      event_timestamp);
			prev_event_timestamp = event_timestamp;
			if (decode_status != CLIENT_TP_HANDLING_COMPLETE)
				trace_cpm_int(tp.timestamp, arg_id_str,
					      tp.arg_payload);
			if (decode_status == CLIENT_TP_HANDLING_ERROR)
				dev_warn_ratelimited(
					dev,
					"tracepoint decode returned an error\n");
			break;
		case TYPE_STRING_INT: {
			/* Casts one 4-byte integer into four 1-byte characters */
			u32 payload_str[2] = { tp.arg_payload, 0 };

			trace_cpm_string(tp.timestamp, arg_id_str,
					 (const char *)payload_str);
			break;
		}
		case TYPE_STRING_ADDR: {
			const char *const arg_payload_str =
				google_cpm_tp_resolve_payload_str(cpm_tp,
								  tp.arg_payload);
			trace_cpm_string(tp.timestamp, arg_id_str,
					 arg_payload_str);
			break;
		}
		default:
			dev_err(dev, "Unknown arg_type %u, timestamp: %llu\n",
				tp.arg_type, tp.timestamp);
		}
	}
	dev_dbg(dev, "Received tracepoints (type: %u, count: %u).\n",
		ring_buf_param->type, ring_buf_param->count);
	dev_dbg(dev, "Processed CPM tracepoints in kernel space.\n");

	return ret;
}

/*
 * Stores the received CPM tracepoints into ftrace, then sends a response
 * message to the CPM.
 * Returns 0 on success, otherwise, negative error code.
 * Note that the received payload is reused to send the response message.
 */
static int google_cpm_tp_store_and_reply(struct google_cpm_tp_dev *cpm_tp,
					 struct google_cpm_tp_payload_node *payload_node)
{
	int ret;
	struct google_cpm_tp_ring_buf_param *ring_buf_param = &cpm_tp->ring_buf_param;
	u32 context = payload_node->context;
	struct cpm_iface_payload *payload = &payload_node->payload;

	ret = mutex_lock_interruptible(&cpm_tp->ring_buf_param_lock);
	if (ret)
		return ret;

	/*
	 * Update ring buffer params here.
	 * The parameters will be passed to the user-space via ioctl
	 */
	if (cpm_tp->ignore_overflow_on_first_tracepoint) {
		ring_buf_param->overflow = false;
		cpm_tp->ignore_overflow_on_first_tracepoint = false;
	} else {
		ring_buf_param->overflow =
			FIELD_GET(PAYLOAD_OVERFLOW_FIELD, payload->payload[0]);
	}
	ring_buf_param->type = FIELD_GET(PAYLOAD_TP_TYPE, payload->payload[0]);
	ring_buf_param->tail = payload->payload[1];
	ring_buf_param->count = payload->payload[2];

	ret = __google_cpm_tp_store_and_reply(cpm_tp, ring_buf_param);
	if (ret == 0) {
		payload->payload[0] = ring_buf_param->type;
		payload->payload[1] = ring_buf_param->tail;
		payload->payload[2] = ring_buf_param->count;
	} else {
		payload->payload[0] = 0;
		payload->payload[1] = 0;
		payload->payload[2] = 0;
	}
	google_cpm_tp_send_resp(cpm_tp, context, payload);
	mutex_unlock(&cpm_tp->ring_buf_param_lock);

	return ret;
}

static void google_cpm_tp_msg_hdl(struct work_struct *work)
{
	struct google_cpm_tp_dev *cpm_tp;
	struct list_head list;
	struct google_cpm_tp_payload_node *received_itr;
	struct google_cpm_tp_payload_node *tmp;
	unsigned long flags;

	cpm_tp = container_of(work, struct google_cpm_tp_dev,
			      received_msg_handle_work);
	spin_lock_irqsave(&cpm_tp->received_payload_list_lock, flags);
	list_replace_init(&cpm_tp->received_payload_list, &list);
	spin_unlock_irqrestore(&cpm_tp->received_payload_list_lock, flags);
	list_for_each_entry_safe(received_itr, tmp, &list, list) {
		google_cpm_tp_store_and_reply(cpm_tp, received_itr);
		list_del(&received_itr->list);
		devm_kfree(cpm_tp->dev, received_itr);
	}
}

static int __google_cpm_tp_get_string_buff(struct google_cpm_tp_dev *cpm_tp,
					   struct google_cpm_tp_str_resp_payload *resp_payload)
{
	struct device *dev = cpm_tp->dev;
	u32 resp_status, resp_size, str_length_to_copy;

	resp_status = le32_to_cpu(resp_payload->status);
	resp_size = le32_to_cpu(resp_payload->size);
	str_length_to_copy = resp_size;

	cpm_tp->tp_string.offset_in_cpm = le32_to_cpu(resp_payload->str_offset);

	switch (resp_status) {
	case NO_ERROR:
		break; /* Proceed to copying reseponse to memory */
	case ERR_NOT_ENOUGH_BUFFER:
		dev_err(dev, "Buffer is too small for response size: %d\n",
			resp_size);
		return -ETOOSMALL;
	case ERR_NOT_SUPPORTED:
		dev_err(dev, "Unsupported request.\n");
		return -EINVAL;
	default:
		dev_err(dev, "Unknown error response.\n");
		return -ESERVERFAULT;
	}

	if (resp_size > TP_STR_BUF_SIZE - 1) {
		dev_warn(dev,
			 "Response size (%u) is greater than buffer size (%d).\n",
			 resp_size, TP_STR_BUF_SIZE - 1);
		dev_warn(dev, "Remaining part will be dropped.\n");

		str_length_to_copy = TP_STR_BUF_SIZE - 1;
	}

	cpm_tp->tp_string.copied_size = str_length_to_copy;

	memcpy(cpm_tp->tp_string.buf, cpm_tp->dma_virt, str_length_to_copy);
	/* In case memory stores garbage or NULL terminator is dropped. */
	cpm_tp->tp_string.buf[TP_STR_BUF_SIZE - 1] = '\0';

	return 0;
}

static int google_cpm_tp_get_string_buff(struct google_cpm_tp_dev *cpm_tp)
{
	struct device *dev = cpm_tp->dev;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	struct cpm_iface_req client_req = {
		.msg_type = REQUEST_MSG,
		.req_msg = &req_msg,
		.resp_msg = &resp_msg,
		.dst_id = cpm_tp->remote_ch,
		.tout_ms = MBOX_REQ_TIMEOUT,
	};
	int ret;

	req_msg.payload[0] = TP_STR_REQUEST;
	req_msg.payload[1] = lower_32_bits(cpm_tp->dma_phys);
	req_msg.payload[2] = TP_STR_BUF_SIZE;

	/*
	 * The AP should not suspend until it gets the response from the CPM
	 * (i.e. the CPM finishes writing data into DRAM).
	 * However, since this function is called only from the probe function,
	 * and the AP blocks the suspend transition until the probe function
	 * returns, nothing that blocks the suspend is needed here.
	 */
	ret = cpm_send_message(cpm_tp->cpm_client, &client_req);
	if (ret) {
		dev_err(dev, "Failed to send the request for the tracepoint buffer address.\n");
		return ret;
	}

	/* Parse received data and store */
	return __google_cpm_tp_get_string_buff(cpm_tp,
			(struct google_cpm_tp_str_resp_payload *)resp_msg.payload);
}

static long google_cpm_tp_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	void __user *uptr = (void __user *)arg;
	struct google_cpm_tp_dev *cpm_tp = filp->private_data;
	struct device *dev = cpm_tp->dev;
	int rc = 0;

	switch (cmd) {
	case IOCTL_EVENTFD_PARAM_WRITE: {
		struct google_cpm_tp_notif_param *notif_param =
			&cpm_tp->notif_param;

		rc = copy_from_user(notif_param,
				    (struct google_cpm_tp_notif_param *)uptr,
				    sizeof(*notif_param));
		if (rc) {
			dev_err(dev, "copy_from_user() returns %d\n", rc);
			return -EFAULT;
		}

		dev_dbg(dev, "PID: %u, eventfd: %u", notif_param->pid,
			notif_param->eventfd);
		cpm_tp->notif_params_initialized = true;

		return 0;
	}
	case IOCTL_MMAP_PARAM_READ: {
		struct google_cpm_tp_mmap_param mmap_param = {
			.tp_buff_size = NUM_BUFFERS * TRACEPOINTS_SECTION_SIZE,
			.tp_str_buff_size = TP_STR_BUF_SIZE,
			.tp_str_buff_copied_size =
				cpm_tp->tp_string.copied_size,
			.tp_str_buff_offset_in_cpm =
				cpm_tp->tp_string.offset_in_cpm,
		};

		dev_dbg(dev, "TP buff size: %llu\n", mmap_param.tp_buff_size);
		dev_dbg(dev,
			"TP str buff size: %llu, TP str copied size: %u, TP str offset: %u",
			mmap_param.tp_str_buff_size,
			mmap_param.tp_str_buff_copied_size,
			mmap_param.tp_str_buff_offset_in_cpm);

		rc = copy_to_user((struct google_cpm_tp_mmap_param *)uptr,
				  &mmap_param, sizeof(mmap_param));
		if (rc) {
			dev_err(dev, "copy_to_user() returns %d\n", rc);
			return -EFAULT;
		}

		return 0;
	}
	case IOCTL_RING_BUFF_PARAMS_READ:
		if (mutex_trylock(&cpm_tp->ring_buf_param_lock)) {
			/*
			 * The lock should be held by the driver
			 * when the user space calls this.
			 */
			dev_err(dev,
				"Lock should be held by the driver when calling this ioctl\n");
			mutex_unlock(&cpm_tp->ring_buf_param_lock);
			return -EAGAIN;
		}

		rc = copy_to_user(uptr, &cpm_tp->ring_buf_param,
				  sizeof(cpm_tp->ring_buf_param));
		if (rc) {
			dev_err(dev, "copy_to_user() returns %d\n", rc);
			return -EFAULT;
		}

		return 0;
	case IOCTL_USERSPACE_ACCESS_DONE_WRITE:
		dev_dbg(dev, "Received a notification from user\n");
		complete(&cpm_tp->userspace_completion);
		return 0;
	default: /* Unknown ioctl number */
		return -ENOTTY;
	}
}

static int google_cpm_tp_open(struct inode *inode, struct file *filp)
{
	struct google_cpm_tp_cdev_prop *cdev_prop =
		container_of(inode->i_cdev, struct google_cpm_tp_cdev_prop,
			     cdev);
	struct google_cpm_tp_dev *cpm_tp =
		container_of(cdev_prop, struct google_cpm_tp_dev, cdev_prop);

	dev_dbg(cpm_tp->dev, "Device opened\n");
	filp->private_data = cpm_tp;
	return 0;
}

static int google_cpm_tp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct google_cpm_tp_dev *cpm_tp = filp->private_data;
	struct device *dev = cpm_tp->dev;

	unsigned long size_by_user = vma->vm_end - vma->vm_start;
	int ret;

	dev_dbg(dev, "User-requested size: %lu\n", size_by_user);

	if (!PAGE_ALIGNED(size_by_user)) {
		dev_err(dev, "Size %lu is not aligned with PAGE_SIZE %lu\n",
			size_by_user, PAGE_SIZE);
		return -EINVAL;
	}

	switch (size_by_user) {
	case TP_STR_BUF_SIZE: {
		unsigned long tp_string_buf_phys =
			(unsigned long)virt_to_phys(cpm_tp->tp_string.buf);
		unsigned long pfn = tp_string_buf_phys >> PAGE_SHIFT;

		dev_dbg(dev,
			"User's trying to mmap tracepoint string buffer.\n");

		ret = remap_pfn_range(vma, vma->vm_start, pfn, size_by_user,
				      vma->vm_page_prot);
		if (ret) {
			dev_err(cpm_tp->dev,
				"Failed to mmap the tracepoint string (error: %d)\n",
				ret);
			return ret;
		}

		return 0;
	}
	case TP_BUF_TOTAL_SIZE:
		dev_dbg(dev, "User's trying to mmap TP buffers.\n");

		if (!PAGE_ALIGNED(cpm_tp->tp_buffer_phys)) {
			dev_err(dev,
				"Tracepoint buff addr should be page aligned for mmap to work: 0x%llx\n",
				cpm_tp->tp_buffer_phys);
			return -EINVAL;
		}

		/* The CPM writes these directly to memory, so AP cannot cache */
		/* This is a non-cacheable memory mapping, use pgprot_writecombine */
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		ret = vm_iomap_memory(vma, cpm_tp->tp_buffer_phys,
				      size_by_user);
		if (ret) {
			dev_err(dev,
				"Failed to mmap the tracepoint buffers (error: %d)\n",
				ret);
			return ret;
		}

		return 0;
	default:
		dev_err(dev, "User tried to mmap unknown buffer type.\n");
		return -EINVAL;
	}
}

static void google_cpm_tp_clean_cdev(struct google_cpm_tp_dev *cpm_tp)
{
	struct google_cpm_tp_cdev_prop *cdev_prop = &cpm_tp->cdev_prop;

	device_destroy(cdev_prop->class, cdev_prop->dev_num);
	class_destroy(cdev_prop->class);
	cdev_del(&cdev_prop->cdev);
	unregister_chrdev_region(cdev_prop->dev_num, 1);
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = google_cpm_tp_ioctl,
	.open = google_cpm_tp_open,
	.mmap = google_cpm_tp_mmap,
};

static int google_cpm_tp_init_cdev(struct google_cpm_tp_dev *cpm_tp)
{
	int ret = 0;
	struct device *dev = NULL;
	struct google_cpm_tp_cdev_prop *cdev_prop = &cpm_tp->cdev_prop;

	ret = alloc_chrdev_region(&cdev_prop->dev_num, 0, 1, CDEV_NAME);
	if (ret) {
		dev_err(cpm_tp->dev, "Failed to alloc chrdev ret:%d.\n", ret);
		goto exit;
	}

	cdev_init(&cdev_prop->cdev, &fops);

	ret = cdev_add(&cdev_prop->cdev, cdev_prop->dev_num, 1);
	if (ret) {
		dev_err(cpm_tp->dev, "Failed to add cdev ret:%d.\n", ret);
		goto unregister_device;
	}

	cdev_prop->class = class_create(CDEV_NAME);
	if (IS_ERR(cdev_prop->class)) {
		dev_err(cpm_tp->dev, "Failed to create a class\n");
		ret = PTR_ERR(cdev_prop->class);
		goto del_cdev;
	}

	dev = device_create(cdev_prop->class, NULL, cdev_prop->dev_num, NULL,
			    CDEV_NAME);
	if (IS_ERR(dev)) {
		dev_err(cpm_tp->dev, "Failed to create device\n");
		ret = PTR_ERR(dev);
		goto destroy_class;
	}
	return 0;

destroy_class:
	class_destroy(cdev_prop->class);
del_cdev:
	cdev_del(&cdev_prop->cdev);
unregister_device:
	unregister_chrdev_region(cdev_prop->dev_num, 1);
exit:
	return ret;
}

static int google_cpm_tp_parse_address(struct device *dev,
				       struct google_cpm_tp_buff_addr_payload *resp_payload,
				       phys_addr_t *p_addr)
{
	u32 resp_status, resp_addr;

	resp_status = le32_to_cpu(resp_payload->status);
	resp_addr = le32_to_cpu(resp_payload->addr);

	switch (resp_status) {
	case NO_ERROR:
		*p_addr = resp_addr;
		return 0;
	case ERR_NOT_SUPPORTED:
		dev_err(dev, "Unsupported request.\n");
		return -EINVAL;
	default:
		dev_err(dev, "Unknown error response.\n");
		return -ESERVERFAULT;
	}
}

/*
 * Request the starting address of the CPM tracepoint buffers to the CPM,
 * so that the driver can access them later.
 * Returns a negative error code on failure.
 */
static int google_cpm_tp_get_buff_addr(
	struct google_cpm_tp_dev *cpm_tp, enum google_cpm_tp_req_t tp_req,
	phys_addr_t *p_addr, u32 *p_size)
{
	int ret;
	struct device *dev = cpm_tp->dev;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	struct cpm_iface_req client_req = {
		.msg_type = REQUEST_MSG,
		.req_msg = &req_msg,
		.resp_msg = &resp_msg,
		.dst_id = cpm_tp->remote_ch,
		.tout_ms = MBOX_REQ_TIMEOUT,
	};
	struct google_cpm_tp_buff_addr_payload *resp_payload;

	req_msg.payload[0] = TP_BUFF_ADDR_REQUEST;
	req_msg.payload[1] = tp_req;
	req_msg.payload[2] = 0;

	ret = cpm_send_message(cpm_tp->cpm_client, &client_req);
	if (ret) {
		dev_err(dev, "Failed to send the request for the tracepoint buffer address.\n");
		return ret;
	}

	resp_payload = (struct google_cpm_tp_buff_addr_payload *)&resp_msg.payload;
	ret = google_cpm_tp_parse_address(dev, resp_payload, p_addr);
	if (ret) {
		return ret;
	}
	*p_size = resp_payload->size;
	return 0;
}

static int google_cpm_tp_request_mailbox(struct google_cpm_tp_dev *cpm_tp)
{
	struct cpm_iface_client *cpm_client;

	cpm_client = cpm_iface_request_client(cpm_tp->dev, CPM_SOURCE_ID_CPM_TRACEPOINT,
					      google_cpm_tp_rx_callback, cpm_tp);
	if (IS_ERR(cpm_client))
		return PTR_ERR(cpm_client);

	cpm_tp->cpm_client = cpm_client;

	return 0;
}

/*
 * Request CPM to turn on the pull mode so that this driver can be notified of trace events
 * Returns a negative error code on failure.
 * Sets flag cpm_pull_mode_supported to indicate whether CPM supports the pull mode
 */
static int google_cpm_tp_pull_mode(struct google_cpm_tp_dev *cpm_tp,
				   bool enable)
{
	int ret;
	struct device *dev = cpm_tp->dev;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	struct cpm_iface_req client_req = {
		.msg_type = REQUEST_MSG,
		.req_msg = &req_msg,
		.resp_msg = &resp_msg,
		.dst_id = cpm_tp->remote_ch,
		.tout_ms = MBOX_REQ_TIMEOUT,
	};

	req_msg.payload[0] = TP_ENABLE_PULL_MODE;
	req_msg.payload[1] = (u32)enable;
	req_msg.payload[2] = 0;

	cpm_tp->ignore_overflow_on_first_tracepoint = true;

	ret = cpm_send_message(cpm_tp->cpm_client, &client_req);
	if (ret) {
		dev_err(dev,
			"Failed to send the request to enable CPM pull mode.\n");
		return ret;
	}

	struct google_cpm_tp_buff_addr_payload *resp_payload =
		(struct google_cpm_tp_buff_addr_payload *)&resp_msg.payload;

	cpm_tp->cpm_pull_mode_supported = resp_payload->status == NO_ERROR;
	return 0;
}

static inline void google_cpm_tp_mbox_free(struct google_cpm_tp_dev *cpm_tp)
{
	if (!IS_ERR_OR_NULL(cpm_tp->cpm_client))
		cpm_iface_free_client(cpm_tp->cpm_client);
}

static int cpm_pull_mode_set(struct google_cpm_tp_dev *cpm_tp, bool mode)
{
	int ret = 0;

	if (cpm_tp->cpm_pull_mode == mode) {
		dev_dbg(cpm_tp->dev, "cpm pull mode already set as %d.\n", mode);
		return 0;
	}

	ret = google_cpm_tp_pull_mode(cpm_tp, mode);
	if (ret)
		return ret;

	cpm_tp->cpm_pull_mode = mode;
	if (mode)
		client_init_callbacks();
	else
		client_exit_callbacks();

	/*
	 * TODO: (b/376268942) undo this workaround after a new DRAM allocation
	 * for tracepoint string table is available
	 */
	if (cpm_tp->cpm_pull_mode && !cpm_tp->tp_string.copied_size) {
		ret = google_cpm_tp_get_string_buff(cpm_tp);
		if (ret) {
			dev_warn(cpm_tp->dev,
				 "Failed to copy string table from CPM\n");
			cpm_tp->cpm_pull_mode = false;
			return ret;
		}
	}

	return 0;
}

static int debugfs_cpm_pull_mode_set(void *data, u64 val)
{
	struct google_cpm_tp_dev *cpm_tp = data;

	return cpm_pull_mode_set(cpm_tp, val ? true : false);
}

static int debugfs_cpm_pull_mode_get(void *data, u64 *val)
{
	struct google_cpm_tp_dev *cpm_tp = data;

	*val = (u64)cpm_tp->cpm_pull_mode;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(cpm_pull_mode_fops, debugfs_cpm_pull_mode_get,
			 debugfs_cpm_pull_mode_set, "%llu\n");

static struct google_cpm_tp_dev *cpm_tp;

/**
 * Function called before trace_event "param_set_value_cpm" enabled.
 * It will ensure tracepoint data flow is in pull mode.
 */
int param_set_value_cpm_enable(void)
{
	if (!cpm_tp)
		return -EINVAL;

	return cpm_pull_mode_set(cpm_tp, true);
}

/**
 * Function called after trace_event "param_set_value_cpm" enabled.
 * It will ensure tracepoint data flow is in push mode.
 */
void param_set_value_cpm_disable(void)
{
	if (cpm_tp)
		cpm_pull_mode_set(cpm_tp, false);
}

static int tp_init_debugfs(struct google_cpm_tp_dev *cpm_tp)
{
	struct device *dev = cpm_tp->dev;
	struct dentry *cpm_pull_mode_dentry;

	cpm_tp->debugfs = debugfs_create_dir("cpm_tp", NULL);
	if (IS_ERR(cpm_tp->debugfs)) {
		dev_warn(dev, "Failed to create debugfs\n");
		return -EIO;
	}

	cpm_pull_mode_dentry = debugfs_create_file("cpm_pull_mode", 0644,
						   cpm_tp->debugfs, cpm_tp,
						   &cpm_pull_mode_fops);
	if (IS_ERR(cpm_pull_mode_dentry)) {
		dev_warn(dev,
			 "Failed to create cpm_pull_mode debugfs file node\n");
		debugfs_remove_recursive(cpm_tp->debugfs);
		return -EIO;
	}

	return 0;
}

void add_cpm_param_trace(char *param_name, unsigned int value,
			 unsigned long timestamp)
{
	trace_param_set_value_cpm(param_name, value, timestamp);
}

static int google_cpm_tp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dma_region, *node = dev->of_node;
	struct reserved_mem *rmem;
	int ret;
	phys_addr_t tp_buffer_phys;
	void *tp_buffer_virt;
	u32 tp_buffer_size;
	enum google_cpm_tp_req_t tp_req;

	cpm_tp = devm_kzalloc(dev, sizeof(*cpm_tp), GFP_KERNEL);
	if (!cpm_tp)
		return -ENOMEM;

	cpm_tp->dev = dev;

	cpm_tp->tp_string.copied_size = 0;
	cpm_tp->tp_string.offset_in_cpm = 0;
	cpm_tp->tp_string.buf =
		(char *)devm_get_free_pages(dev, GFP_KERNEL | __GFP_ZERO,
					    get_order(TP_STR_BUF_SIZE));
	if (!cpm_tp->tp_string.buf)
		return -ENOMEM;

	platform_set_drvdata(pdev, cpm_tp);

	ret = google_cpm_tp_init_cdev(cpm_tp);
	if (ret)
		return ret;

	mutex_init(&cpm_tp->ring_buf_param_lock);

	ret = of_property_read_u32(node, "mba-dest-channel",
				   &cpm_tp->remote_ch);
	if (ret < 0) {
		dev_err(dev, "Failed to read mba-dest-channel.\n");
		goto clean_cdev;
	}

	dma_region = of_parse_phandle(node, "memory-region", 0);
	if (!dma_region) {
		dev_err(dev, "Failed to get memory-region\n");
		ret = -ENODEV;
		goto clean_cdev;
	}

	rmem = of_reserved_mem_lookup(dma_region);
	of_node_put(dma_region);
	if (!rmem) {
		dev_err(dev, "Failed to get reserved mem of node name %s\n",
			node->name);
		ret = -ENODEV;
		goto clean_cdev;
	}

	if (rmem->size >= BIT_ULL(32)) {
		dev_err(dev, "DMA size must not exceed 32-bit: %llu\n",
			rmem->size);
		ret = -EINVAL;
		goto clean_cdev;
	}

	if (rmem->size < TP_STR_BUF_SIZE) {
		dev_err(dev,
			"DMA size (%llu) is too small (remote buffer: %u)\n",
			rmem->size, TP_STR_BUF_SIZE);
		ret = -ENOMEM;
		goto clean_cdev;
	}

	cpm_tp->dma_size = (u32)rmem->size;

	ret = of_reserved_mem_device_init(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to get reserved memory region.\n");
		goto clean_cdev;
	}

	/* TODO(b/201487692) Change the mask to fit RDO SMMU design. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(dev, "Failed to set dma mask to 32 bits.\n");
		goto release_reserved_region;
	}

	cpm_tp->dma_virt = dma_alloc_coherent(dev, cpm_tp->dma_size,
					      &cpm_tp->dma_phys, GFP_KERNEL);
	if (!cpm_tp->dma_virt) {
		ret = -ENOMEM;
		goto release_reserved_region;
	}

	/* TODO(b/283179225): Workaround for the CPM address limitation */
	if (cpm_tp->dma_size + cpm_tp->dma_phys >= 0xA0000000) {
		dev_err(dev,
			"CPM can't handle the address over 0xA0000000 now on gem5.\n");
		ret = -ENOMEM;
		goto free_dma;
	}

	spin_lock_init(&cpm_tp->received_payload_list_lock);
	INIT_LIST_HEAD(&cpm_tp->received_payload_list);
	devm_work_autocancel(dev, &cpm_tp->received_msg_handle_work,
			     google_cpm_tp_msg_hdl);
	init_completion(&cpm_tp->userspace_completion);

	ret = google_cpm_tp_request_mailbox(cpm_tp);
	if (ret < 0) {
		if (ret == -EPROBE_DEFER)
			dev_dbg(dev,
				"CPM intrerface is not ready. Try again later\n");
		else
			dev_err(dev,
				"Failed to request mailbox channel err %d.\n",
				ret);
		goto free_dma;
	}

	cpm_tp->cpm_pull_mode = false;
	cpm_tp->cpm_pull_mode_supported = false;
	/*
	 * Requesting the push mode during init
	 * This command doesn't change any functionality since CPM default is push mode
	 * It allows driver to know if the CPM pull mode support is available or not
	 */
	ret = google_cpm_tp_pull_mode(cpm_tp, cpm_tp->cpm_pull_mode);
	if (ret < 0) {
		dev_err(dev, "Failed to check cpm pull mode support err %d\n",
			ret);
		goto free_mbox_channel;
	}

	initialize_cpm_tracepoint_decoder();

	for (tp_req = TP_REQUEST_INFO; tp_req < NUM_TP_REQUESTS; ++tp_req) {
		ret = google_cpm_tp_get_buff_addr(
			cpm_tp, tp_req, &tp_buffer_phys, &tp_buffer_size);
		if (ret) {
			dev_err(dev,
				"Failed to get the remote buffer address: %d\n",
				ret);
			goto free_mbox_channel;
		}

		dev_dbg(dev,
			"Physical starting address of tracepoint buffer #%d: %pap",
			(int)tp_req, (void *)tp_buffer_phys);

		if (!PAGE_ALIGNED(tp_buffer_phys)) {
			dev_warn(dev,
				 "Tracepoint buff #%d addr not page aligned: %pap\n",
				 (int)tp_req, (void *)tp_buffer_phys);
		}

		tp_buffer_virt =
			devm_ioremap(cpm_tp->dev, tp_buffer_phys,
				     tp_buffer_size);
		if (IS_ERR(tp_buffer_virt)) {
			ret = PTR_ERR(tp_buffer_virt);
			goto free_mbox_channel;
		}
		cpm_tp->ring_buf_info_list[tp_req].buffer = tp_buffer_virt;
		cpm_tp->ring_buf_info_list[tp_req].num_tracepoints =
			tp_buffer_size / sizeof(struct google_cpm_tp);

		/* Save address of info buffer. */
		if (tp_req == TP_REQUEST_INFO)
			cpm_tp->tp_buffer_phys = tp_buffer_phys;
	}

	/*
	 * TODO: (b/376268942) undo this workaround after a new DRAM allocation
	 * for tracepoint string table is available
	 */
	//ret = google_cpm_tp_get_string_buff(cpm_tp);
	//if (ret)
	//	goto free_mbox_channel;

	if (cpm_tp->cpm_pull_mode_supported)
		if (tp_init_debugfs(cpm_tp))
			goto free_mbox_channel;

	return 0;

free_mbox_channel:
	google_cpm_tp_mbox_free(cpm_tp);

free_dma:
	dma_free_coherent(dev, cpm_tp->dma_size, cpm_tp->dma_virt,
			  cpm_tp->dma_phys);
release_reserved_region:
	of_reserved_mem_device_release(dev);
clean_cdev:
	google_cpm_tp_clean_cdev(cpm_tp);

	return ret;
}

static int google_cpm_tp_remove(struct platform_device *pdev)
{
	struct google_cpm_tp_dev *cpm_tp = platform_get_drvdata(pdev);

	if (!IS_ERR_OR_NULL(cpm_tp->event_ctx))
		eventfd_ctx_put(cpm_tp->event_ctx);

	debugfs_remove_recursive(cpm_tp->debugfs);
	google_cpm_tp_mbox_free(cpm_tp);
	dma_free_coherent(cpm_tp->dev, cpm_tp->dma_size, cpm_tp->dma_virt,
			  cpm_tp->dma_phys);
	of_reserved_mem_device_release(cpm_tp->dev);
	google_cpm_tp_clean_cdev(cpm_tp);

	return 0;
}

static const struct of_device_id google_cpm_tp_of_match_table[] = {
	{ .compatible = "google,cpm-tracepoint" },
	{},
};
MODULE_DEVICE_TABLE(of, google_cpm_tp_of_match_table);

struct platform_driver google_cpm_tp_driver = {
	.driver = {
		.name = "google-cpm-tracepoint",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(google_cpm_tp_of_match_table),
	},
	.probe  = google_cpm_tp_probe,
	.remove = google_cpm_tp_remove,
};

module_platform_driver(google_cpm_tp_driver);
MODULE_DESCRIPTION("Google CPM tracepoint driver");
MODULE_LICENSE("GPL");
