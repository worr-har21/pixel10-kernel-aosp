/* SPDX-License-Identifier: GPL-2.0 */

/**
 * @copyright Copyright (c) 2023 Samsung Electronics Co., Ltd
 *
 */

#ifndef __UWB_H__
#define __UWB_H__
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <linux/netdevice.h>
#include <linux/atomic.h>

#include "uwb_fw.h"
#include "uwb_sysnodes.h"

#define MAX_NAME_LEN 64
#define MAX_RXQ_DEPTH 1024

#define UWB_IOC_TYPE 'U'

#define UWB_IOCTL_RESET _IO(UWB_IOC_TYPE, 1)
#define UWB_IOCTL_FW_FLASH _IOW(UWB_IOC_TYPE, 2, struct flash_param)
#define UWB_IOCTL_POWER_ON _IO(UWB_IOC_TYPE, 3)
#define UWB_IOCTL_POWER_OFF _IO(UWB_IOC_TYPE, 4)
#define UWB_IOCTL_FW_INFO _IOR(UWB_IOC_TYPE, 5, struct firmware_info)
#define UWB_IOCTL_DISABLE_COREDUMP_RESET _IOW(UWB_IOC_TYPE, 6, int)
#define UWB_IOCTL_RESET_VBAT _IO(UWB_IOC_TYPE, 7)

#define UCI_HEAD_SIZE ((int)sizeof(struct uci_msg_hdr))
#define UCI_MT_CMD (0b001)
#define UCI_MT_RESP (0b010)
#define UCI_MT_NTF (0b011)
#define UCI_ATR_BID (0x10)
#define GID_VENDOR_CONFIG (0b1100)
#define OID_VENDOR_DEV_CTRL (0b111111)
#define VENDOR_BID_SIZE 4
#define U100_STATE_INDEX_IN_ATR 8
#define FW_TYPE_INDEX_IN_ATR 10
#define TLV_SIZE_INDEX_IN_ATR 11
#define UCI_DATA_LEN_OFFSET 3
#define MINIMAL_ATR_SIZE 9
#define U100_BL0_STATE 0x00
#define U100_BL1_STATE 0x01
#define U100_FW_STATE 0x02
#define U100_EDS_STATE 0x03
#define U100_UNKNOWN_STATE 0xFF
#define NONE_DATA_00 0x00
#define NONE_DATA_FF 0xFF
#define PROBE_ATTR_TIMEOUT msecs_to_jiffies(1000)
#define UNKNOWN_STR "Unknown"
#define SKB_MAX_PRINT_SIZE 64U

enum {
	DOWNLOAD,
	BL0,
	BL1,
};

struct uwb_irq {
	spinlock_t lock;
	unsigned int num;
	char name[MAX_NAME_LEN];
	unsigned long flags;
	bool active;
	bool registered;
	struct irq_desc *u100_irq;
};

struct uci_msg_hdr {
	u8 gid:4;
	u8 pbf:1;
	u8 mt:3;
	union {
		struct {
			u8 oid:6;
			u8 rfu1:2;
		} __packed;
		u8 data_rfu1:8;
	};
	union {
		struct {
			u8 rfu2:8;
			u8 len:8;
		} __packed;
		u16 data_len:16;
	};
} __packed;

struct u100_ctx {
	atomic_t opened;
	struct spi_device *spi;
	struct miscdevice uci_dev;
	struct gpio_desc *gpio_u100_sync;
	struct uwb_irq gpio_u100_irq;
	struct mutex lock;
	atomic_t num_spi_slow_txs;

	struct mutex ioctl_mutex;

	struct gpio_desc *gpio_u100_power;
	struct gpio_desc *gpio_u100_reset;
	struct gpio_desc *gpio_u100_en;

	wait_queue_head_t wq;
	struct sk_buff_head sk_rx_q;
	struct sk_buff_head sk_tx_q;
	struct completion tx_done_cmpl;
	struct completion atr_done_cmpl;
	struct completion irq_done_cmpl;
	struct completion process_done_cmpl;
	struct mutex atr_lock;

	struct uwb_firmware_ctx uwb_fw_ctx;
	atomic_t flashing;
	atomic_t flashing_fw;
	int u100_state;
	atomic_t u100_powered_on;
	atomic_t u100_enter_download;
	atomic_t waiting_atr;
	int wait_atr_err;
	struct firmware_info fw_info;
	bool misc_registered;
	unsigned long flags;
	bool is_download_mode;
	bool is_atr_right;
	bool is_bhalf_entered;
	struct task_struct *fw_download_thr;
	struct uwb_sysnode uwb_node;

	void (*recv_package)(struct u100_ctx *u100_ctx, struct sk_buff *skb);
	void (*register_device)(struct u100_ctx *u100_ctx);

	struct uwb_coredump *coredump;
};

int init_controller_layer(struct u100_ctx *u100_ctx);
int link_send_package(struct u100_ctx *u100_ctx, char *buff, unsigned int size);
irqreturn_t rx_tsk_work(int irq, void *data);
void handle_fw_ap_send(struct u100_ctx *u100_ctx);

#define LOG_TAG "UWB_Kernel: "
#define CALLER  (__builtin_return_address(0))

#define UWB_ERR(fmt, ...) \
	pr_err(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define UWB_DEBUG(fmt, ...) \
	pr_debug(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define UWB_INFO(fmt, ...) \
	pr_info(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
#define UWB_WARN(fmt, ...) \
	pr_warn(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
/* system is unusable */
#define UWB_EMERG(fmt, ...) \
	pr_emerg(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
/* action must be taken immediately */
#define UWB_ALERT(fmt, ...) \
	pr_alert(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
/* critical conditions */
#define UWB_CRIT(fmt, ...) \
	pr_crit(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
/* normal but significant condition */
#define UWB_NOTICE(fmt, ...) \
	pr_notice(LOG_TAG "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)

int init_fw_download_thread(struct u100_ctx *u100_ctx, bool flag);
void stop_fw_download_thread(struct u100_ctx *u100_ctx);

void uwbs_init(struct u100_ctx *u100_ctx);

void uwbs_power_on(struct u100_ctx *u100_ctx);

void uwbs_power_off(struct u100_ctx *u100_ctx);

void uwbs_reset(struct u100_ctx *u100_ctx);

/**
 * UWBS reset VBAT by means of power-switch pin.
 * Calling IS_ERR_OR_NULL(u100_ctx->gpio_u100_power) to check if it is available since
 * it is not defined in older version device tree.
 */
void uwbs_reset_vbat(struct u100_ctx *u100_ctx);

/**
 * @brief SyncReset. Need to wait for firmware ATR message.
 * @return If 0 is returned, reset times out; otherwise, success.
 */
int uwbs_sync_reset(struct u100_ctx *u100_ctx);

void uwbs_start_download(struct u100_ctx *u100_ctx);

bool uwbs_sync_start_download(struct u100_ctx *u100_ctx);

static inline unsigned int get_irq_count(struct irq_desc *irq)
{
	unsigned int cnt = 0;

	if (!IS_ERR_OR_NULL(irq))
		cnt = irq->irq_count;
	return cnt;
}

#endif /* __UWB_H__ */
