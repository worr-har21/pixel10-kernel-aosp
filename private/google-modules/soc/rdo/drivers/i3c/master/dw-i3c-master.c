// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 *
 * Author: Vitor Soares <vitor.soares@synopsys.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i3c/master.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "dw-i3c-master.h"

#define DEVICE_CTRL			0x0
#define DEV_CTRL_ENABLE			BIT(31)
#define DEV_CTRL_RESUME			BIT(30)
#define DEV_CTRL_HOT_JOIN_NACK		BIT(8)
#define DEV_CTRL_I2C_SLAVE_PRESENT	BIT(7)

#define DEVICE_ADDR			0x4
#define DEV_ADDR_DYNAMIC_ADDR_VALID	BIT(31)
#define DEV_ADDR_DYNAMIC(x)		(((x) << 16) & GENMASK(22, 16))

#define HW_CAPABILITY			0x8
#define COMMAND_QUEUE_PORT		0xc
#define COMMAND_PORT_TOC		BIT(30)
#define COMMAND_PORT_READ_TRANSFER	BIT(28)
#define COMMAND_PORT_SDAP		BIT(27)
#define COMMAND_PORT_ROC		BIT(26)
#define COMMAND_PORT_SPEED(x)		(((x) << 21) & GENMASK(23, 21))
#define COMMAND_PORT_DEV_INDEX(x)	(((x) << 16) & GENMASK(20, 16))
#define COMMAND_PORT_CP			BIT(15)
#define COMMAND_PORT_CMD(x)		(((x) << 7) & GENMASK(14, 7))
#define COMMAND_PORT_TID(x)		(((x) << 3) & GENMASK(6, 3))
#define COMMAND_PORT_TID_MASK(x)	((x) & GENMASK(3, 0))
#define COMMON_COMMAND_CODE_TID		0xF

#define COMMAND_PORT_ARG_DATA_LEN(x)	(((x) << 16) & GENMASK(31, 16))
#define COMMAND_PORT_ARG_DATA_LEN_MAX	65536
#define COMMAND_PORT_TRANSFER_ARG	0x01

#define COMMAND_PORT_SDA_DATA_BYTE_3(x)	(((x) << 24) & GENMASK(31, 24))
#define COMMAND_PORT_SDA_DATA_BYTE_2(x)	(((x) << 16) & GENMASK(23, 16))
#define COMMAND_PORT_SDA_DATA_BYTE_1(x)	(((x) << 8) & GENMASK(15, 8))
#define COMMAND_PORT_SDA_BYTE_STRB_3	BIT(5)
#define COMMAND_PORT_SDA_BYTE_STRB_2	BIT(4)
#define COMMAND_PORT_SDA_BYTE_STRB_1	BIT(3)
#define COMMAND_PORT_SHORT_DATA_ARG	0x02

#define COMMAND_PORT_DEV_COUNT(x)	(((x) << 21) & GENMASK(25, 21))
#define COMMAND_PORT_CMD_ATTR_MASK(x)	(((x)) & GENMASK(2, 0))
#define COMMAND_PORT_ADDR_ASSGN_CMD	0x03

#define RESPONSE_QUEUE_PORT		0x10
#define RESPONSE_PORT_ERR_STATUS(x)	(((x) & GENMASK(31, 28)) >> 28)
#define RESPONSE_NO_ERROR		0
#define RESPONSE_ERROR_CRC		1
#define RESPONSE_ERROR_PARITY		2
#define RESPONSE_ERROR_FRAME		3
#define RESPONSE_ERROR_IBA_NACK		4
#define RESPONSE_ERROR_ADDRESS_NACK	5
#define RESPONSE_ERROR_OVER_UNDER_FLOW	6
#define RESPONSE_ERROR_TRANSF_ABORT	8
#define RESPONSE_ERROR_I2C_W_NACK_ERR	9
#define RESPONSE_PORT_TID(x)		(((x) & GENMASK(27, 24)) >> 24)
#define RESPONSE_PORT_DATA_LEN(x)	((x) & GENMASK(15, 0))

#define RX_TX_DATA_PORT			0x14
#define IBI_QUEUE_STATUS		0x18
#define IBI_QUEUE_STATUS_IBI_ID(x)	(((x) & GENMASK(15, 8)) >> 8)
#define IBI_QUEUE_STATUS_DATA_LEN(x)	((x) & GENMASK(7, 0))
#define IBI_QUEUE_IBI_ADDR(x)		(IBI_QUEUE_STATUS_IBI_ID(x) >> 1)
#define IBI_QUEUE_IBI_RNW(x)		(IBI_QUEUE_STATUS_IBI_ID(x) & BIT(0))
#define IBI_TYPE_MR(x)                                                         \
	((IBI_QUEUE_IBI_ADDR(x) != I3C_HOT_JOIN_ADDR) && !IBI_QUEUE_IBI_RNW(x))
#define IBI_TYPE_HJ(x)                                                         \
	((IBI_QUEUE_IBI_ADDR(x) == I3C_HOT_JOIN_ADDR) && !IBI_QUEUE_IBI_RNW(x))
#define IBI_TYPE_SIRQ(x)                                                        \
	((IBI_QUEUE_IBI_ADDR(x) != I3C_HOT_JOIN_ADDR) && IBI_QUEUE_IBI_RNW(x))

#define QUEUE_THLD_CTRL			0x1c
#define QUEUE_THLD_CTRL_IBI_STAT_MASK	GENMASK(31, 24)
#define QUEUE_THLD_CTRL_IBI_STAT(x)	(((x) - 1) << 24)
#define QUEUE_THLD_CTRL_IBI_DATA_MASK	GENMASK(20, 16)
#define QUEUE_THLD_CTRL_IBI_DATA(x)	((x) << 16)
#define QUEUE_THLD_CTRL_RESP_BUF_MASK	GENMASK(15, 8)
#define QUEUE_THLD_CTRL_RESP_BUF(x)	(((x) - 1) << 8)
#define QUEUE_THLD_CTRL_CMD_EMPTY_BUF_MASK GENMASK(7, 0)
#define QUEUE_THLD_CTRL_CMD_EMPTY_BUF(x) ((x) - 1)

#define DATA_BUFFER_THLD_CTRL		0x20
#define DATA_BUFFER_THLD_CTRL_TX_BUF(x) ((x) & GENMASK(2, 0))
#define DATA_BUFFER_THLD_CTRL_TX_BUF_MASK GENMASK(2, 0)
#define DATA_BUFFER_THLD_CTRL_RX_BUF(x) (((x) & GENMASK(10, 8)) >> 8)
#define DATA_BUFFER_THLD_CTRL_RX_BUF_MASK GENMASK(10, 8)

#define INTR_DATA_BUF_THLD_1		0x0
#define INTR_DATA_BUF_THLD_4		0x1
#define INTR_DATA_BUF_THLD_8		0x2
#define INTR_DATA_BUF_THLD_16		0x3

#define IBI_QUEUE_CTRL			0x24
#define IBI_MR_REQ_REJECT		0x2C
#define IBI_SIR_REQ_REJECT		0x30
#define IBI_REQ_REJECT_ALL		GENMASK(31, 0)

#define RESET_CTRL			0x34
#define RESET_CTRL_IBI_QUEUE		BIT(5)
#define RESET_CTRL_RX_FIFO		BIT(4)
#define RESET_CTRL_TX_FIFO		BIT(3)
#define RESET_CTRL_RESP_QUEUE		BIT(2)
#define RESET_CTRL_CMD_QUEUE		BIT(1)
#define RESET_CTRL_SOFT			BIT(0)

#define SLV_EVENT_CTRL			0x38
#define INTR_STATUS			0x3c
#define INTR_STATUS_EN			0x40
#define INTR_SIGNAL_EN			0x44
#define INTR_FORCE			0x48
#define INTR_BUSOWNER_UPDATE_STAT	BIT(13)
#define INTR_IBI_UPDATED_STAT		BIT(12)
#define INTR_READ_REQ_RECV_STAT		BIT(11)
#define INTR_DEFSLV_STAT		BIT(10)
#define INTR_TRANSFER_ERR_STAT		BIT(9)
#define INTR_DYN_ADDR_ASSGN_STAT	BIT(8)
#define INTR_CCC_UPDATED_STAT		BIT(6)
#define INTR_TRANSFER_ABORT_STAT	BIT(5)
#define INTR_RESP_READY_STAT		BIT(4)
#define INTR_CMD_QUEUE_READY_STAT	BIT(3)
#define INTR_IBI_THLD_STAT		BIT(2)
#define INTR_RX_THLD_STAT		BIT(1)
#define INTR_TX_THLD_STAT		BIT(0)
#define INTR_ALL			(INTR_BUSOWNER_UPDATE_STAT |	\
					INTR_IBI_UPDATED_STAT |		\
					INTR_READ_REQ_RECV_STAT |	\
					INTR_DEFSLV_STAT |		\
					INTR_TRANSFER_ERR_STAT |	\
					INTR_DYN_ADDR_ASSGN_STAT |	\
					INTR_CCC_UPDATED_STAT |		\
					INTR_TRANSFER_ABORT_STAT |	\
					INTR_RESP_READY_STAT |		\
					INTR_CMD_QUEUE_READY_STAT |	\
					INTR_IBI_THLD_STAT |		\
					INTR_TX_THLD_STAT |		\
					INTR_RX_THLD_STAT)

#define INTR_XFER_MASK			(INTR_TRANSFER_ERR_STAT |	\
					INTR_RESP_READY_STAT |		\
					INTR_CMD_QUEUE_READY_STAT |	\
					INTR_TX_THLD_STAT |		\
					INTR_RX_THLD_STAT)

#define QUEUE_STATUS_LEVEL		0x4c
#define QUEUE_STATUS_IBI_STATUS_CNT(x)	(((x) & GENMASK(28, 24)) >> 24)
#define QUEUE_STATUS_IBI_BUF_BLR(x)	(((x) & GENMASK(23, 16)) >> 16)
#define QUEUE_STATUS_LEVEL_RESP(x)	(((x) & GENMASK(15, 8)) >> 8)
#define QUEUE_STATUS_LEVEL_CMD(x)	((x) & GENMASK(7, 0))

#define DATA_BUFFER_STATUS_LEVEL	0x50
#define DATA_BUFFER_STATUS_LEVEL_TX(x)	((x) & GENMASK(7, 0))
#define DATA_BUFFER_STATUS_LEVEL_RX(x)	(((x) & GENMASK(23, 16)) >> 16)

#define PRESENT_STATE			0x54
#define CCC_DEVICE_STATUS		0x58
#define DEVICE_ADDR_TABLE_POINTER	0x5c
#define DEVICE_ADDR_TABLE_DEPTH(x)	(((x) & GENMASK(31, 16)) >> 16)
#define DEVICE_ADDR_TABLE_ADDR(x)	((x) & GENMASK(7, 0))

#define DEV_CHAR_TABLE_POINTER		0x60
#define VENDOR_SPECIFIC_REG_POINTER	0x6c
#define SLV_PID_VALUE			0x74
#define SLV_CHAR_CTRL			0x78
#define SLV_MAX_LEN			0x7c
#define MAX_READ_TURNAROUND		0x80
#define MAX_DATA_SPEED			0x84
#define SLV_DEBUG_STATUS		0x88
#define SLV_INTR_REQ			0x8c
#define DEVICE_CTRL_EXTENDED		0xb0
#define SCL_I3C_OD_TIMING		0xb4
#define SCL_I3C_PP_TIMING		0xb8
#define SCL_I3C_TIMING_HCNT(x)		(((x) << 16) & GENMASK(23, 16))
#define SCL_I3C_TIMING_LCNT(x)		((x) & GENMASK(7, 0))
#define SCL_I3C_TIMING_CNT_MIN		5

#define SCL_I2C_FM_TIMING		0xbc
#define SCL_I2C_FM_TIMING_HCNT(x)	(((x) << 16) & GENMASK(31, 16))
#define SCL_I2C_FM_TIMING_LCNT(x)	((x) & GENMASK(15, 0))

#define SCL_I2C_FMP_TIMING		0xc0
#define SCL_I2C_FMP_TIMING_HCNT(x)	(((x) << 16) & GENMASK(23, 16))
#define SCL_I2C_FMP_TIMING_LCNT(x)	((x) & GENMASK(15, 0))

#define SCL_EXT_LCNT_TIMING		0xc8
#define SCL_EXT_LCNT_4(x)		(((x) << 24) & GENMASK(31, 24))
#define SCL_EXT_LCNT_3(x)		(((x) << 16) & GENMASK(23, 16))
#define SCL_EXT_LCNT_2(x)		(((x) << 8) & GENMASK(15, 8))
#define SCL_EXT_LCNT_1(x)		((x) & GENMASK(7, 0))

#define SCL_EXT_TERMN_LCNT_TIMING	0xcc
#define STOP_HLD_CNT_MASK		GENMASK(31, 28)

#define SDA_HOLD_SWITCH_DLY_TIMING	0xd0
#define SDA_OD_PP_SWITCH_DLY(x)		((x) & GENMASK(2, 0))
#define SDA_PP_OD_SWITCH_DLY(x)		(((x) << 8) & GENMASK(10, 8))
#define SDA_TX_HOLD(x)			(((x) << 16) & GENMASK(18, 16))

#define BUS_FREE_TIMING			0xd4
#define BUS_I3C_MST_FREE(x)		((x) & GENMASK(15, 0))

#define BUS_IDLE_TIMING			0xd8

#define I3C_VER_ID			0xe0
#define I3C_VER_101			0x3130312a

#define I3C_VER_TYPE			0xe4
#define EXTENDED_CAPABILITY		0xe8
#define SLAVE_CONFIG			0xec

#define DEV_ADDR_TABLE_IBI_MDB		BIT(12)
#define DEV_ADDR_TABLE_SIR_REJECT	BIT(13)
#define DEV_ADDR_TABLE_LEGACY_I2C_DEV	BIT(31)
#define DEV_ADDR_TABLE_DYNAMIC_ADDR(x)	(((x) << 16) & GENMASK(23, 16))
#define DEV_ADDR_TABLE_STATIC_ADDR(x)	((x) & GENMASK(6, 0))
#define DEV_ADDR_TABLE_LOC(start, idx)	((start) + ((idx) << 2))

#define DEV_CHARS_TABLE_LOC(start, id_dev, id_reg) ((start) + ((id_dev) << 4) + ((id_reg) << 2))

#define I3C_BUS_SDR1_SCL_RATE		8000000
#define I3C_BUS_SDR2_SCL_RATE		6000000
#define I3C_BUS_SDR3_SCL_RATE		4000000
#define I3C_BUS_SDR4_SCL_RATE		2000000
#define I3C_BUS_I2C_FM_TLOW_MIN_NS	1300
#define I3C_BUS_I2C_FMP_TLOW_MIN_NS	500
#define I3C_BUS_THIGH_MAX_NS		41

#define DATA_BUF_ENTRY_SIZE		4

#define I2C_FAST_MODE 0
#define I2C_FAST_MODE_PLUS 1
#define I2C_LVR_MODE BIT(4)

/* Threshold chosen for transactions expected to complete within 200 microseconds. */
/* SDR rate = 12.5MHz. 100 bytes took ~72us + start header ~10us. */
#define I3C_HYBRID_BUSY_WAIT_BYTES_THLD 100
/* I2C rate = 1MHz. 10 bytes took ~100us + start header ~10us. */
#define I2C_HYBRID_BUSY_WAIT_BYTES_THLD 10
/* Polling for 300us first, the transfer is expected to finished in 200us. */
#define BUSY_WAIT_XFER_TIMEOUT_NS 300000
#define ADDRESS_BYTES 1

#define XFER_TIMEOUT (msecs_to_jiffies(1000))
#define RPM_AUTOSUSPEND_TIMEOUT_MS 1000

#define TLOW_OD_MARGIN_NS 5

struct dw_i3c_cmd {
	u32 cmd_lo;
	u32 cmd_hi;
	u16 tx_len;
	const void *tx_buf;
	u16 rx_len;
	void *rx_buf;
	u32 resp;
};

struct dw_i3c_xfer {
	struct list_head node;
	struct completion comp;
	int ret;
	int end_with_iba_nack;
	unsigned int ncmds;
	unsigned int cmd_idx;
	unsigned int resp_idx;
	unsigned int tx_cmd_idx;
	u32 res_tx_entries;
	u32 res_rx_entries;
	u16 tx_buf_cursor;
	unsigned int rx_cmd_idx;
	u16 rx_buf_cursor;
	struct dw_i3c_cmd cmds[];
};

struct dw_i3c_i2c_dev_data {
	u8 index;
	struct i3c_generic_ibi_pool *ibi_pool;
};

/* i2c or i3c selector enum */
enum ixc {
	IXC_I2C = '2',
	IXC_I3C
};

static const unsigned int regs2read[] = { DEVICE_CTRL, QUEUE_STATUS_LEVEL, DATA_BUFFER_STATUS_LEVEL,
					  SCL_I3C_OD_TIMING, SCL_I3C_PP_TIMING,
					  DEV_CHAR_TABLE_POINTER, INTR_STATUS, PRESENT_STATE };

static const char * const regs_names[] = { "DEV_CTL", "Q_STAT_LVL", "DAT_BUF_STAT_LVL",
					   "SCL_I3C_OD_TIMING", "SCL_I3C_PP_TIMING", "DCT_PTR",
					   "IR_STAT", "PRES_STAT" };

#define DUMP_REGS_NUM ARRAY_SIZE(regs2read)
#define DCT_PTR_I 5

static void dump_regs_locked(struct dw_i3c_master *master)
{
	BUILD_BUG_ON(ARRAY_SIZE(regs2read) != ARRAY_SIZE(regs_names));
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	u32 dct_addr, dct_size, dct_devs_n, tmp;
	int i;

	if (!__ratelimit(&_rs))
		return;

	tmp = readl(master->regs + regs2read[DCT_PTR_I]);
	dct_addr = tmp & 0xFFF;
	dct_size = (tmp >> 12) & 0x7F;	/* Size in bytes */
	dct_devs_n = dct_size / 16;	/* 4 regs x 8 bytes = 16 bytes per device */

	dev_err(master->dev, "regs:");
	for (i = 0; i < DUMP_REGS_NUM; ++i)
		pr_cont("%s:%#X ", regs_names[i], readl(master->regs + regs2read[i]));
	pr_cont("\n");

	if (dct_devs_n > 0) {
		dev_err(master->dev, "dct-<i>:regs[0-3]");

		for (i = 0; i < dct_devs_n; ++i) {
			dev_err(master->dev, "%d:%#010X %#010X %#010X %#010X", i,
				readl(master->regs + DEV_CHARS_TABLE_LOC(dct_addr, i, 0)),
				readl(master->regs + DEV_CHARS_TABLE_LOC(dct_addr, i, 1)),
				readl(master->regs + DEV_CHARS_TABLE_LOC(dct_addr, i, 2)),
				readl(master->regs + DEV_CHARS_TABLE_LOC(dct_addr, i, 3)));
		}
	}
}

static u8 even_parity(u8 p)
{
	p ^= p >> 4;
	p &= 0xf;

	return (0x9669 >> p) & 1;
}

static void dw_i3c_master_xfer_push_cmd_queue_locked(struct dw_i3c_master *master,
						     u32 queue_status_level);
static void dw_i3c_master_xfer_wr_tx_data_locked(struct dw_i3c_master *master,
						 u32 buf_status_level);
static bool dw_i3c_master_xfer_pull_resp_queue_locked(struct dw_i3c_master *master,
						      u32 queue_status_level);
static void dw_i3c_master_xfer_rd_rx_data_locked(struct dw_i3c_master *master,
						 u32 buf_status_level);
static bool dw_i3c_xfer_finished(struct dw_i3c_xfer *xfer)
{
	bool data_finished = false;
	bool queue_finished = false;

	if (!xfer)
		return false;

	data_finished = xfer->res_tx_entries == 0 && xfer->res_rx_entries == 0;
	queue_finished = xfer->cmd_idx >= xfer->ncmds && xfer->resp_idx >= xfer->ncmds;

	if (data_finished && queue_finished)
		xfer->ret = 0;

	return data_finished && queue_finished;
}

static bool dw_i3c_xfer_error(struct dw_i3c_xfer *xfer, struct dw_i3c_cmd *cmd)
{
	switch (RESPONSE_PORT_ERR_STATUS(cmd->resp)) {
	case RESPONSE_NO_ERROR:
		break;

	case RESPONSE_ERROR_PARITY:
	case RESPONSE_ERROR_TRANSF_ABORT:
	case RESPONSE_ERROR_CRC:
	case RESPONSE_ERROR_FRAME:
		xfer->ret = -EIO;
		break;

	case RESPONSE_ERROR_OVER_UNDER_FLOW:
		xfer->ret = -ENOSPC;
		break;

	case RESPONSE_ERROR_I2C_W_NACK_ERR:
	case RESPONSE_ERROR_ADDRESS_NACK:
	default:
		xfer->ret = -EINVAL;
		break;
	}

	return xfer->ret != -ETIMEDOUT;
}

/* return true : xfer timeout, false : xfer completed.*/
static bool dw_i3c_wait_for_xfer_completion(struct dw_i3c_xfer *xfer, bool busy_wait_mode)
{
	if (busy_wait_mode) {
		unsigned long start_time = ktime_get_ns();
		/* If polling timeout, fallback to wait_for_completion. */
		while (ktime_get_ns() - start_time < BUSY_WAIT_XFER_TIMEOUT_NS) {
			if (completion_done(&xfer->comp))
				return false;
			udelay(2);
		}
	}

	return wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT) == 0;
}

static bool dw_i3c_master_supports_ccc_cmd(struct i3c_master_controller *m,
					   const struct i3c_ccc_cmd *cmd)
{
	if (cmd->ndests > 1)
		return false;

	switch (cmd->id) {
	case I3C_CCC_ENEC(true):
	case I3C_CCC_ENEC(false):
	case I3C_CCC_DISEC(true):
	case I3C_CCC_DISEC(false):
	case I3C_CCC_ENTAS(0, true):
	case I3C_CCC_ENTAS(0, false):
	case I3C_CCC_RSTDAA(true):
	case I3C_CCC_RSTDAA(false):
	case I3C_CCC_ENTDAA:
	case I3C_CCC_SETMWL(true):
	case I3C_CCC_SETMWL(false):
	case I3C_CCC_SETMRL(true):
	case I3C_CCC_SETMRL(false):
	case I3C_CCC_ENTHDR(0):
	case I3C_CCC_SETDASA:
	case I3C_CCC_SETNEWDA:
	case I3C_CCC_GETMWL:
	case I3C_CCC_GETMRL:
	case I3C_CCC_GETPID:
	case I3C_CCC_GETBCR:
	case I3C_CCC_GETDCR:
	case I3C_CCC_GETSTATUS:
	case I3C_CCC_GETMXDS:
	case I3C_CCC_GETHDRCAP:
		return true;
	default:
		return false;
	}
}

static inline struct dw_i3c_master *
to_dw_i3c_master(struct i3c_master_controller *master)
{
	return container_of(master, struct dw_i3c_master, base);
}

static void dw_i3c_master_disable(struct dw_i3c_master *master)
{
	writel(master->dev_ctrl & ~DEV_CTRL_ENABLE, master->regs + DEVICE_CTRL);
}

static void dw_i3c_master_enable(struct dw_i3c_master *master)
{
	writel(master->dev_ctrl | DEV_CTRL_ENABLE, master->regs + DEVICE_CTRL);
}

static int dw_i3c_master_get_addr_pos(struct dw_i3c_master *master, u8 addr)
{
	int pos;

	for (pos = 0; pos < master->maxdevs; pos++) {
		if (addr == master->devs[pos].addr)
			return pos;
	}

	return -EINVAL;
}

static int dw_i3c_master_get_free_pos(struct dw_i3c_master *master)
{
	if (!(master->free_pos & GENMASK(master->maxdevs - 1, 0)))
		return -ENOSPC;

	return ffs(master->free_pos) - 1;
}

static void dw_i3c_master_wr_tx_fifo(struct dw_i3c_master *master,
				     const u8 *bytes, int nbytes)
{
	writesl(master->regs + RX_TX_DATA_PORT, bytes, nbytes / 4);
	if (nbytes & 3) {
		u32 tmp = 0;

		memcpy(&tmp, bytes + (nbytes & ~3), nbytes & 3);
		writesl(master->regs + RX_TX_DATA_PORT, &tmp, 1);
	}
}

static void dw_i3c_master_read_fifo(struct dw_i3c_master *master,
				    int reg,  u8 *bytes, int nbytes)
{
	readsl(master->regs + reg, bytes, nbytes / 4);
	if (nbytes & 3) {
		u32 tmp;

		readsl(master->regs + reg, &tmp, 1);
		memcpy(bytes + (nbytes & ~3), &tmp, nbytes & 3);
	}
}

static void dw_i3c_master_read_rx_fifo(struct dw_i3c_master *master,
				       u8 *bytes, int nbytes)
{
	return dw_i3c_master_read_fifo(master, RX_TX_DATA_PORT, bytes, nbytes);
}

static void dw_i3c_master_read_ibi_fifo(struct dw_i3c_master *master,
					u8 *bytes, int nbytes)
{
	return dw_i3c_master_read_fifo(master, IBI_QUEUE_STATUS, bytes, nbytes);
}

static struct dw_i3c_xfer *
dw_i3c_master_alloc_xfer(struct dw_i3c_master *master, unsigned int ncmds)
{
	struct dw_i3c_xfer *xfer;

	xfer = vzalloc(struct_size(xfer, cmds, ncmds));
	if (!xfer)
		return NULL;

	INIT_LIST_HEAD(&xfer->node);
	xfer->ncmds = ncmds;
	xfer->ret = -ETIMEDOUT;

	return xfer;
}

static void dw_i3c_master_free_xfer(struct dw_i3c_xfer *xfer)
{
	vfree(xfer);
}

static void
dw_i3c_master_program_xfer_irq_thld_locked(struct dw_i3c_master *master)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	u32 queue_thld_ctrl, data_thld_ctrl, intr_signal_en;
	unsigned int cmd_len, resp_len;

	/* Since every command takes two locations in the command and response buffers,
	 * the FIFO depth has to be divided by two to get the number of commands it can take.
	 */
	cmd_len = umin(master->caps.cmdfifodepth / 2, 2 * (xfer->ncmds - xfer->cmd_idx));
	resp_len = umin(master->caps.cmdfifodepth / 2, xfer->ncmds - xfer->resp_idx);

	intr_signal_en = readl(master->regs + INTR_SIGNAL_EN);
	intr_signal_en |= INTR_XFER_MASK;

	queue_thld_ctrl = readl(master->regs + QUEUE_THLD_CTRL);
	queue_thld_ctrl &= ~QUEUE_THLD_CTRL_RESP_BUF_MASK;
	queue_thld_ctrl &= ~QUEUE_THLD_CTRL_CMD_EMPTY_BUF_MASK;

	if (resp_len)
		queue_thld_ctrl |= QUEUE_THLD_CTRL_RESP_BUF(resp_len);
	else
		intr_signal_en &= ~INTR_RESP_READY_STAT;

	if (cmd_len)
		queue_thld_ctrl |= QUEUE_THLD_CTRL_CMD_EMPTY_BUF(cmd_len);
	else
		intr_signal_en &= ~INTR_CMD_QUEUE_READY_STAT;

	data_thld_ctrl = readl(master->regs + DATA_BUFFER_THLD_CTRL);
	data_thld_ctrl &= ~DATA_BUFFER_THLD_CTRL_TX_BUF_MASK;
	data_thld_ctrl &= ~DATA_BUFFER_THLD_CTRL_RX_BUF_MASK;

	if (xfer->res_tx_entries >= 16)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_TX_BUF(INTR_DATA_BUF_THLD_16);
	else if (xfer->res_tx_entries >= 8)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_TX_BUF(INTR_DATA_BUF_THLD_8);
	else if (xfer->res_tx_entries >= 4)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_TX_BUF(INTR_DATA_BUF_THLD_4);
	else if (xfer->res_tx_entries >= 1)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_TX_BUF(INTR_DATA_BUF_THLD_1);
	else
		intr_signal_en &= ~INTR_TX_THLD_STAT;

	if (xfer->res_rx_entries >= 16)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_RX_BUF(INTR_DATA_BUF_THLD_16);
	else if (xfer->res_rx_entries >= 8)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_RX_BUF(INTR_DATA_BUF_THLD_8);
	else if (xfer->res_rx_entries >= 4)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_RX_BUF(INTR_DATA_BUF_THLD_4);
	else if (xfer->res_rx_entries >= 1)
		data_thld_ctrl |= DATA_BUFFER_THLD_CTRL_RX_BUF(INTR_DATA_BUF_THLD_1);
	else
		intr_signal_en &= ~INTR_RX_THLD_STAT;

	writel(intr_signal_en, master->regs + INTR_SIGNAL_EN);
	writel(queue_thld_ctrl, master->regs + QUEUE_THLD_CTRL);
	writel(data_thld_ctrl, master->regs + DATA_BUFFER_THLD_CTRL);
}

static void dw_i3c_master_start_xfer_locked(struct dw_i3c_master *master)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	u32 buf_status_level, queue_status_level;

	if (!xfer)
		return;

	buf_status_level = readl(master->regs + DATA_BUFFER_STATUS_LEVEL);
	dw_i3c_master_xfer_wr_tx_data_locked(master, buf_status_level);

	queue_status_level = readl(master->regs + QUEUE_STATUS_LEVEL);
	dw_i3c_master_xfer_push_cmd_queue_locked(master, queue_status_level);

	dw_i3c_master_program_xfer_irq_thld_locked(master);
}

static void dw_i3c_master_enqueue_xfer(struct dw_i3c_master *master,
				       struct dw_i3c_xfer *xfer)
{
	unsigned long flags;
	int i;

	init_completion(&xfer->comp);
	xfer->cmd_idx = 0;
	xfer->resp_idx = 0;
	xfer->tx_cmd_idx = 0;
	xfer->rx_cmd_idx = 0;
	xfer->tx_buf_cursor = 0;
	xfer->rx_buf_cursor = 0;
	xfer->res_tx_entries = 0;
	xfer->res_rx_entries = 0;

	for (i = 0; i < xfer->ncmds; i++) {
		xfer->res_tx_entries +=
			(xfer->cmds[i].tx_len + DATA_BUF_ENTRY_SIZE - 1) / DATA_BUF_ENTRY_SIZE;
		xfer->res_rx_entries +=
			(xfer->cmds[i].rx_len + DATA_BUF_ENTRY_SIZE - 1) / DATA_BUF_ENTRY_SIZE;
	}

	spin_lock_irqsave(&master->xferqueue.lock, flags);
	if (master->xferqueue.cur) {
		list_add_tail(&xfer->node, &master->xferqueue.list);
	} else {
		master->xferqueue.cur = xfer;
		dw_i3c_master_start_xfer_locked(master);
	}
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static void dw_i3c_master_dequeue_xfer_locked(struct dw_i3c_master *master,
					      struct dw_i3c_xfer *xfer)
{
	dev_err(&master->base.dev, "%s: error reset fifo and data buffer.\n",
		__func__);
	dump_regs_locked(master);
	if (master->xferqueue.cur == xfer) {
		u32 status;

		master->xferqueue.cur = NULL;

		writel(RESET_CTRL_RX_FIFO | RESET_CTRL_TX_FIFO |
		       RESET_CTRL_RESP_QUEUE | RESET_CTRL_CMD_QUEUE,
		       master->regs + RESET_CTRL);

		readl_poll_timeout_atomic(master->regs + RESET_CTRL, status,
					  !status, 10, 1000000);
	} else {
		list_del_init(&xfer->node);
	}
}

static void dw_i3c_master_dequeue_xfer(struct dw_i3c_master *master,
				       struct dw_i3c_xfer *xfer)
{
	unsigned long flags;

	spin_lock_irqsave(&master->xferqueue.lock, flags);
	dw_i3c_master_dequeue_xfer_locked(master, xfer);
	spin_unlock_irqrestore(&master->xferqueue.lock, flags);
}

static void dw_i3c_master_end_xfer_locked(struct dw_i3c_master *master)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	u32 intr_signal_en;

	if (!xfer)
		return;

	intr_signal_en = readl(master->regs + INTR_SIGNAL_EN);
	intr_signal_en &= (~INTR_XFER_MASK);
	writel(intr_signal_en, master->regs + INTR_SIGNAL_EN);

	if (xfer->ret < 0) {
		dw_i3c_master_dequeue_xfer_locked(master, xfer);
		writel(master->dev_ctrl | DEV_CTRL_RESUME | DEV_CTRL_ENABLE,
		       master->regs + DEVICE_CTRL);
		dev_err(&master->base.dev, "xfer failed with ret value: %d", xfer->ret);
	}

	complete(&xfer->comp);

	xfer = list_first_entry_or_null(&master->xferqueue.list,
					struct dw_i3c_xfer,
					node);
	if (xfer)
		list_del_init(&xfer->node);

	master->xferqueue.cur = xfer;
	dw_i3c_master_start_xfer_locked(master);
}

static void dw_i3c_master_set_intr_regs(struct dw_i3c_master *master)
{
	u32 thld_ctrl;

	thld_ctrl = readl(master->regs + QUEUE_THLD_CTRL);
	thld_ctrl &= ~(QUEUE_THLD_CTRL_RESP_BUF_MASK |
		       QUEUE_THLD_CTRL_IBI_STAT_MASK |
		       QUEUE_THLD_CTRL_IBI_STAT_MASK);
	thld_ctrl |= QUEUE_THLD_CTRL_IBI_STAT(1) |
		     QUEUE_THLD_CTRL_IBI_DATA(31);
	writel(thld_ctrl, master->regs + QUEUE_THLD_CTRL);

	thld_ctrl = readl(master->regs + DATA_BUFFER_THLD_CTRL);
	thld_ctrl &= ~DATA_BUFFER_THLD_CTRL_RX_BUF_MASK;
	writel(thld_ctrl, master->regs + DATA_BUFFER_THLD_CTRL);

	writel(INTR_ALL, master->regs + INTR_STATUS);
	writel(INTR_ALL, master->regs + INTR_STATUS_EN);
	writel(0, master->regs + INTR_SIGNAL_EN);

	writel(IBI_REQ_REJECT_ALL, master->regs + IBI_SIR_REQ_REJECT);
	writel(IBI_REQ_REJECT_ALL, master->regs + IBI_MR_REQ_REJECT);

	if (readl(master->regs + I3C_VER_ID) == I3C_VER_101) {
		u32 val = readl(master->regs + SCL_EXT_TERMN_LCNT_TIMING);

		val &= ~STOP_HLD_CNT_MASK;
		writel(val, master->regs + SCL_EXT_TERMN_LCNT_TIMING);

		val =  SDA_OD_PP_SWITCH_DLY(1) | SDA_PP_OD_SWITCH_DLY(1) | SDA_TX_HOLD(1);
		writel(val, master->regs + SDA_HOLD_SWITCH_DLY_TIMING);
	}
}

static int dw_i3c_clk_cfg(struct dw_i3c_master *master)
{
	unsigned long core_rate, core_period;
	u32 scl_timing;
	u8 hcnt, lcnt;

	core_rate = clk_get_rate(master->core_clk);
	if (!core_rate)
		return -EINVAL;

	core_period = DIV_ROUND_UP(1000000000, core_rate);

	hcnt = DIV_ROUND_UP(I3C_BUS_THIGH_MAX_NS, core_period) - 1;
	if (hcnt < SCL_I3C_TIMING_CNT_MIN)
		hcnt = SCL_I3C_TIMING_CNT_MIN;

	lcnt = DIV_ROUND_UP(core_rate, master->base.bus.scl_rate.i3c) - hcnt;
	if (lcnt < SCL_I3C_TIMING_CNT_MIN)
		lcnt = SCL_I3C_TIMING_CNT_MIN;

	scl_timing = SCL_I3C_TIMING_HCNT(hcnt) | SCL_I3C_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I3C_PP_TIMING);
	master->i3c_pp_timing = scl_timing;

	/*
	 * In pure i3c mode, MST_FREE represents tCAS. In shared mode, this
	 * will be set up by dw_i2c_clk_cfg as tLOW.
	 */
	if (master->base.bus.mode == I3C_BUS_MODE_PURE) {
		writel(BUS_I3C_MST_FREE(lcnt), master->regs + BUS_FREE_TIMING);
		master->bus_free_timing = BUS_I3C_MST_FREE(lcnt);
	}

	lcnt = max_t(u8,
		     DIV_ROUND_UP(I3C_BUS_TLOW_OD_MIN_NS + TLOW_OD_MARGIN_NS, core_period), lcnt);
	scl_timing = SCL_I3C_TIMING_HCNT(hcnt) | SCL_I3C_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I3C_OD_TIMING);
	master->i3c_od_timing = scl_timing;

	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR1_SCL_RATE) - hcnt;
	scl_timing = SCL_EXT_LCNT_1(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR2_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_2(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR3_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_3(lcnt);
	lcnt = DIV_ROUND_UP(core_rate, I3C_BUS_SDR4_SCL_RATE) - hcnt;
	scl_timing |= SCL_EXT_LCNT_4(lcnt);
	writel(scl_timing, master->regs + SCL_EXT_LCNT_TIMING);
	master->ext_lcnt_timing = scl_timing;

	return 0;
}

static int dw_i2c_clk_cfg(struct dw_i3c_master *master)
{
	unsigned long core_rate, core_period;
	u16 hcnt, lcnt;
	u32 scl_timing;

	core_rate = clk_get_rate(master->core_clk);
	if (!core_rate)
		return -EINVAL;

	core_period = DIV_ROUND_UP(1000000000, core_rate);

	lcnt = DIV_ROUND_UP(I3C_BUS_I2C_FMP_TLOW_MIN_NS, core_period);
	hcnt = DIV_ROUND_UP(core_rate, I3C_BUS_I2C_FM_PLUS_SCL_RATE) - lcnt;
	scl_timing = SCL_I2C_FMP_TIMING_HCNT(hcnt) |
		     SCL_I2C_FMP_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I2C_FMP_TIMING);
	master->i2c_fmp_timing = scl_timing;

	lcnt = DIV_ROUND_UP(I3C_BUS_I2C_FM_TLOW_MIN_NS, core_period);
	hcnt = DIV_ROUND_UP(core_rate, I3C_BUS_I2C_FM_SCL_RATE) - lcnt;
	scl_timing = SCL_I2C_FM_TIMING_HCNT(hcnt) |
		     SCL_I2C_FM_TIMING_LCNT(lcnt);
	writel(scl_timing, master->regs + SCL_I2C_FM_TIMING);
	master->i2c_fm_timing = scl_timing;

	writel(BUS_I3C_MST_FREE(lcnt), master->regs + BUS_FREE_TIMING);
	master->bus_free_timing = BUS_I3C_MST_FREE(lcnt);
	writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_I2C_SLAVE_PRESENT,
	       master->regs + DEVICE_CTRL);

	return 0;
}

static int dw_i3c_master_bus_init(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct i3c_bus *bus = i3c_master_get_bus(m);
	struct i3c_device_info info = { };
	int ret;

	ret = pm_runtime_resume_and_get(master->dev);
	if (ret < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, ret);
		return ret;
	}

	ret = master->platform_ops->init(master);
	if (ret)
		goto rpm_out;

	switch (bus->mode) {
	case I3C_BUS_MODE_MIXED_FAST:
	case I3C_BUS_MODE_MIXED_LIMITED:
		ret = dw_i2c_clk_cfg(master);
		if (ret)
			goto rpm_out;
		fallthrough;
	case I3C_BUS_MODE_PURE:
		ret = dw_i3c_clk_cfg(master);
		if (ret)
			goto rpm_out;
		break;
	default:
		ret = -EINVAL;
		goto rpm_out;
	}

	ret = i3c_master_get_free_addr(m, 0);
	if (ret < 0)
		goto rpm_out;

	writel(DEV_ADDR_DYNAMIC_ADDR_VALID | DEV_ADDR_DYNAMIC(ret),
	       master->regs + DEVICE_ADDR);
	master->dev_addr = ret;

	memset(&info, 0, sizeof(info));
	info.dyn_addr = ret;

	ret = i3c_master_set_info(&master->base, &info);
	if (ret)
		goto rpm_out;

	master->ibi_sir_req_rej = false;
	if (readl(master->regs + IBI_SIR_REQ_REJECT) == IBI_REQ_REJECT_ALL)
		master->ibi_sir_req_rej = true;

	/* For now don't support Hot-Join */
	writel(readl(master->regs + DEVICE_CTRL) | DEV_CTRL_HOT_JOIN_NACK,
	       master->regs + DEVICE_CTRL);

	master->dev_ctrl = readl(master->regs + DEVICE_CTRL);

	dw_i3c_master_set_intr_regs(master);
	dw_i3c_master_enable(master);

rpm_out:
	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);
	return ret;
}

static void dw_i3c_master_bus_cleanup(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	dw_i3c_master_disable(master);
}

static int dw_i3c_ccc_set(struct dw_i3c_master *master,
			  struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	int ret, pos = 0;

	if (ccc->id & I3C_CCC_DIRECT) {
		pos = dw_i3c_master_get_addr_pos(master, ccc->dests[0].addr);
		if (pos < 0)
			return pos;
	}

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	cmd = xfer->cmds;
	cmd->tx_buf = ccc->dests[0].payload.data;
	cmd->tx_len = ccc->dests[0].payload.len;

	cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(ccc->dests[0].payload.len) |
		      COMMAND_PORT_TRANSFER_ARG;

	cmd->cmd_lo = COMMAND_PORT_CP |
		      COMMAND_PORT_TID(COMMON_COMMAND_CODE_TID) |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(ccc->id) |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	if (RESPONSE_PORT_ERR_STATUS(xfer->cmds[0].resp) == RESPONSE_ERROR_IBA_NACK)
		ccc->err = I3C_ERROR_M2;

	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_ccc_get(struct dw_i3c_master *master, struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	int ret, pos;

	pos = dw_i3c_master_get_addr_pos(master, ccc->dests[0].addr);
	if (pos < 0)
		return pos;

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer)
		return -ENOMEM;

	cmd = xfer->cmds;
	cmd->rx_buf = ccc->dests[0].payload.data;
	cmd->rx_len = ccc->dests[0].payload.len;

	cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(ccc->dests[0].payload.len) |
		      COMMAND_PORT_TRANSFER_ARG;

	cmd->cmd_lo = COMMAND_PORT_READ_TRANSFER |
		      COMMAND_PORT_TID(COMMON_COMMAND_CODE_TID) |
		      COMMAND_PORT_CP |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(ccc->id) |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	if (RESPONSE_PORT_ERR_STATUS(xfer->cmds[0].resp) == RESPONSE_ERROR_IBA_NACK)
		ccc->err = I3C_ERROR_M2;
	dw_i3c_master_free_xfer(xfer);

	return ret;
}

static int dw_i3c_master_send_ccc_cmd(struct i3c_master_controller *m,
				      struct i3c_ccc_cmd *ccc)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int ret = 0;

	if (ccc->id == I3C_CCC_ENTDAA)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(master->dev);
	if (ret < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, ret);
		return ret;
	}

	if (ccc->rnw)
		ret = dw_i3c_ccc_get(master, ccc);
	else
		ret = dw_i3c_ccc_set(master, ccc);

	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);
	return ret;
}

static int dw_i3c_master_daa(struct i3c_master_controller *m)
{
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	struct dw_i3c_cmd *cmd;
	u32 olddevs, newdevs;
	u8 p, last_addr = 0;
	int ret, pos;

	ret = pm_runtime_resume_and_get(master->dev);
	if (ret < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, ret);
		return ret;
	}

	olddevs = ~(master->free_pos);

	/* Prepare DAT before launching DAA. */
	for (pos = 0; pos < master->maxdevs; pos++) {
		if (olddevs & BIT(pos))
			continue;

		ret = i3c_master_get_free_addr(m, last_addr + 1);
		if (ret < 0) {
			ret = -ENOSPC;
			goto rpm_out;
		}

		master->devs[pos].addr = ret;
		p = even_parity(ret);
		last_addr = ret;
		ret |= (p << 7);

		writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(ret),
		       master->regs +
		       DEV_ADDR_TABLE_LOC(master->datstartaddr, pos));
	}

	/* Clear return value. The loop above will leave the last address there when finished. */
	ret = 0;

	xfer = dw_i3c_master_alloc_xfer(master, 1);
	if (!xfer) {
		ret = -ENOMEM;
		goto rpm_out;
	}

	/* DAA will iterate until NACK to make sure all i3c slaves have address assigned. */
	xfer->end_with_iba_nack = 1;

	pos = dw_i3c_master_get_free_pos(master);
	if (pos < 0) {
		dw_i3c_master_free_xfer(xfer);
		ret = pos;
		goto rpm_out;
	}
	cmd = &xfer->cmds[0];
	cmd->cmd_hi = COMMAND_PORT_TRANSFER_ARG;
	cmd->cmd_lo = COMMAND_PORT_DEV_COUNT(master->maxdevs - pos) |
		      COMMAND_PORT_TID(COMMON_COMMAND_CODE_TID) |
		      COMMAND_PORT_DEV_INDEX(pos) |
		      COMMAND_PORT_CMD(I3C_CCC_ENTDAA) |
		      COMMAND_PORT_ADDR_ASSGN_CMD |
		      COMMAND_PORT_TOC |
		      COMMAND_PORT_ROC;

	dw_i3c_master_enqueue_xfer(master, xfer);
	if (!wait_for_completion_timeout(&xfer->comp, XFER_TIMEOUT))
		dw_i3c_master_dequeue_xfer(master, xfer);

	/* ENTDAA is expected to end with NACK, resume the controller */
	writel(master->dev_ctrl | DEV_CTRL_RESUME | DEV_CTRL_ENABLE,
	       master->regs + DEVICE_CTRL);

	newdevs = GENMASK(master->maxdevs - RESPONSE_PORT_DATA_LEN(cmd->resp) - 1, 0);
	newdevs &= ~olddevs;

	for (pos = 0; pos < master->maxdevs; pos++) {
		if (newdevs & BIT(pos))
			i3c_master_add_i3c_dev_locked(m, master->devs[pos].addr);
	}

	dw_i3c_master_free_xfer(xfer);

rpm_out:
	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);
	return ret;
}

static int dw_i3c_master_priv_xfers(struct i3c_dev_desc *dev,
				    struct i3c_priv_xfer *i3c_xfers,
				    int i3c_nxfers)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	int i, ret = 0, total_bytes = 0;
	bool busy_wait_mode = false, timeout;

	if (!i3c_nxfers)
		return 0;

	xfer = dw_i3c_master_alloc_xfer(master, i3c_nxfers);
	if (!xfer)
		return -ENOMEM;

	ret = pm_runtime_resume_and_get(master->dev);
	if (ret < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < i3c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i3c_xfers[i].len) |
			COMMAND_PORT_TRANSFER_ARG;

		if (i3c_xfers[i].rnw) {
			cmd->rx_buf = i3c_xfers[i].data.in;
			cmd->rx_len = i3c_xfers[i].len;
			cmd->cmd_lo = COMMAND_PORT_READ_TRANSFER |
				      COMMAND_PORT_SPEED(dev->info.max_read_ds);

		} else {
			cmd->tx_buf = i3c_xfers[i].data.out;
			cmd->tx_len = i3c_xfers[i].len;
			cmd->cmd_lo =
				COMMAND_PORT_SPEED(dev->info.max_write_ds);
		}

		cmd->cmd_lo |= COMMAND_PORT_TID(i) |
			       COMMAND_PORT_DEV_INDEX(data->index) |
			       COMMAND_PORT_ROC;

		if (i == (i3c_nxfers - 1))
			cmd->cmd_lo |= COMMAND_PORT_TOC;

		total_bytes += (i3c_xfers[i].len + ADDRESS_BYTES);
	}

	busy_wait_mode = ((master->operation_mode == HYBRID_BUSY_WAIT_MODE) &&
			(total_bytes < master->i3c_hybrid_threshold));

	dw_i3c_master_enqueue_xfer(master, xfer);
	timeout = dw_i3c_wait_for_xfer_completion(xfer, busy_wait_mode);

	if (timeout)
		dw_i3c_master_dequeue_xfer(master, xfer);

	for (i = 0; i < i3c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		if (i3c_xfers[i].rnw)
			i3c_xfers[i].len = cmd->rx_len;
	}

	ret = xfer->ret;
	dw_i3c_master_free_xfer(xfer);

	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);
	return ret;
}

static int dw_i3c_master_reattach_i3c_dev(struct i3c_dev_desc *dev,
					  u8 old_dyn_addr)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int pos;

	pos = dw_i3c_master_get_free_pos(master);

	if (data->index > pos && pos > 0) {
		writel(0,
		       master->regs +
		       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

		master->devs[data->index].addr = 0;
		master->free_pos |= BIT(data->index);

		data->index = pos;
		master->devs[pos].addr = dev->info.dyn_addr;
		master->free_pos &= ~BIT(pos);
	}

	writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(dev->info.dyn_addr),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	master->devs[data->index].addr = dev->info.dyn_addr;

	return 0;
}

static int dw_i3c_master_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data;
	int pos;

	pos = dw_i3c_master_get_free_pos(master);
	if (pos < 0)
		return pos;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->index = pos;
	master->devs[pos].addr = dev->info.dyn_addr ? : dev->info.static_addr;
	master->free_pos &= ~BIT(pos);
	i3c_dev_set_master_data(dev, data);

	writel(DEV_ADDR_TABLE_DYNAMIC_ADDR(master->devs[pos].addr),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	return 0;
}

static void dw_i3c_master_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	writel(0,
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	i3c_dev_set_master_data(dev, NULL);
	master->devs[data->index].addr = 0;
	master->free_pos |= BIT(data->index);
	kfree(data);
}

static int dw_i3c_master_i2c_xfers(struct i2c_dev_desc *dev,
				   const struct i2c_msg *i2c_xfers,
				   int i2c_nxfers)
{
	struct dw_i3c_i2c_dev_data *data = i2c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_xfer *xfer;
	int i, ret = 0, total_bytes = 0;
	bool busy_wait_mode = false, timeout;
	int i2c_speed_mode = I2C_FAST_MODE_PLUS;

	if (!i2c_nxfers)
		return 0;

	xfer = dw_i3c_master_alloc_xfer(master, i2c_nxfers);
	if (!xfer)
		return -ENOMEM;

	ret = pm_runtime_resume_and_get(master->dev);
	if (ret < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, ret);
		return ret;
	}

	/* I3C LVR bit[4]: tell whether the device operates in FM (Fast Mode) or FM+ mode */
	if (dev->lvr & I2C_LVR_MODE)
		i2c_speed_mode = I2C_FAST_MODE;

	for (i = 0; i < i2c_nxfers; i++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[i];

		cmd->cmd_hi = COMMAND_PORT_ARG_DATA_LEN(i2c_xfers[i].len) |
			COMMAND_PORT_TRANSFER_ARG;

		cmd->cmd_lo = COMMAND_PORT_TID(i) |
			      COMMAND_PORT_DEV_INDEX(data->index) |
			      COMMAND_PORT_SPEED(i2c_speed_mode) |
			      COMMAND_PORT_ROC;

		if (i2c_xfers[i].flags & I2C_M_RD) {
			cmd->cmd_lo |= COMMAND_PORT_READ_TRANSFER;
			cmd->rx_buf = i2c_xfers[i].buf;
			cmd->rx_len = i2c_xfers[i].len;
		} else {
			cmd->tx_buf = i2c_xfers[i].buf;
			cmd->tx_len = i2c_xfers[i].len;
		}

		if (i == (i2c_nxfers - 1))
			cmd->cmd_lo |= COMMAND_PORT_TOC;

		total_bytes += (i2c_xfers[i].len + ADDRESS_BYTES);
	}

	busy_wait_mode = ((master->operation_mode == HYBRID_BUSY_WAIT_MODE) &&
			(total_bytes < master->i2c_hybrid_threshold));

	dw_i3c_master_enqueue_xfer(master, xfer);
	timeout = dw_i3c_wait_for_xfer_completion(xfer, busy_wait_mode);

	if (timeout)
		dw_i3c_master_dequeue_xfer(master, xfer);

	ret = xfer->ret;
	dw_i3c_master_free_xfer(xfer);

	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);
	return ret;
}

static int dw_i3c_master_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	struct dw_i3c_i2c_dev_data *data;
	int pos;

	pos = dw_i3c_master_get_free_pos(master);
	if (pos < 0)
		return pos;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->index = pos;
	master->devs[pos].addr = dev->addr;
	master->devs[pos].is_i2c = true;
	master->free_pos &= ~BIT(pos);
	i2c_dev_set_master_data(dev, data);

	writel(DEV_ADDR_TABLE_LEGACY_I2C_DEV |
	       DEV_ADDR_TABLE_STATIC_ADDR(dev->addr),
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	return 0;
}

static void dw_i3c_master_detach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i2c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);

	writel(0,
	       master->regs +
	       DEV_ADDR_TABLE_LOC(master->datstartaddr, data->index));

	i2c_dev_set_master_data(dev, NULL);
	master->devs[data->index].addr = 0;
	master->free_pos |= BIT(data->index);
	kfree(data);
}

static int dw_i3c_master_request_ibi(struct i3c_dev_desc *dev,
				     const struct i3c_ibi_setup *req)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	unsigned long flags;

	data->ibi_pool = i3c_generic_ibi_alloc_pool(dev, req);
	if (IS_ERR(data->ibi_pool))
		return PTR_ERR(data->ibi_pool);

	spin_lock_irqsave(&master->devs_lock, flags);
	master->devs[data->index].ibi_dev = dev;
	spin_unlock_irqrestore(&master->devs_lock, flags);

	return 0;
}

static void dw_i3c_master_free_ibi(struct i3c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	unsigned long flags;

	spin_lock_irqsave(&master->devs_lock, flags);
	master->devs[data->index].ibi_dev = NULL;
	spin_unlock_irqrestore(&master->devs_lock, flags);

	i3c_generic_ibi_free_pool(data->ibi_pool);
	data->ibi_pool = NULL;
}

static void dw_i3c_master_set_sir_enabled(struct dw_i3c_master *master,
					  struct i3c_dev_desc *dev,
					  u8 idx, bool enable)
{
	unsigned long flags;
	u32 dat_entry, reg;
	bool global;

	dat_entry = DEV_ADDR_TABLE_LOC(master->datstartaddr, idx);

	spin_lock_irqsave(&master->devs_lock, flags);
	reg = readl(master->regs + dat_entry);
	if (enable) {
		reg &= ~DEV_ADDR_TABLE_SIR_REJECT;
		if (dev->info.bcr & I3C_BCR_IBI_PAYLOAD)
			reg |= DEV_ADDR_TABLE_IBI_MDB;
	} else {
		reg |= DEV_ADDR_TABLE_SIR_REJECT;
	}
	master->platform_ops->set_dat_ibi(master, dev, enable, &reg);
	writel(reg, master->regs + dat_entry);

	global = true;
	if (master->ibi_sir_req_rej) {
		reg = readl(master->regs + IBI_SIR_REQ_REJECT);
		if (enable) {
			global = reg == IBI_REQ_REJECT_ALL;
			reg &= ~BIT(idx);
		} else {
			global = reg == 0;
			reg |= BIT(idx);
		}
		writel(reg, master->regs + IBI_SIR_REQ_REJECT);
	}

	if (global) {
		spin_lock(&master->xferqueue.lock);
		reg = readl(master->regs + INTR_STATUS_EN);
		reg &= ~INTR_IBI_THLD_STAT;
		if (enable)
			reg |= INTR_IBI_THLD_STAT;
		writel(reg, master->regs + INTR_STATUS_EN);

		reg = readl(master->regs + INTR_SIGNAL_EN);
		reg &= ~INTR_IBI_THLD_STAT;
		if (enable)
			reg |= INTR_IBI_THLD_STAT;
		writel(reg, master->regs + INTR_SIGNAL_EN);
		spin_unlock(&master->xferqueue.lock);
	}

	spin_unlock_irqrestore(&master->devs_lock, flags);
}

static int dw_i3c_master_enable_ibi(struct i3c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int rc;

	rc = pm_runtime_resume_and_get(master->dev);
	if (rc < 0) {
		dev_err(master->dev, "%s: cannot resume i3c bus master, err: %d\n", __func__, rc);
		return rc;
	}

	dw_i3c_master_set_sir_enabled(master, dev, data->index, true);

	rc = i3c_master_enec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);

	if (rc) {
		dw_i3c_master_set_sir_enabled(master, dev, data->index, false);
		pm_runtime_mark_last_busy(master->dev);
		pm_runtime_put_autosuspend(master->dev);
	}

	return rc;
}

static int dw_i3c_master_disable_ibi(struct i3c_dev_desc *dev)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct dw_i3c_master *master = to_dw_i3c_master(m);
	int rc;

	rc = i3c_master_disec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);
	if (rc)
		return rc;

	dw_i3c_master_set_sir_enabled(master, dev, data->index, false);

	pm_runtime_mark_last_busy(master->dev);
	pm_runtime_put_autosuspend(master->dev);

	return 0;
}

static void dw_i3c_master_recycle_ibi_slot(struct i3c_dev_desc *dev,
					   struct i3c_ibi_slot *slot)
{
	struct dw_i3c_i2c_dev_data *data = i3c_dev_get_master_data(dev);

	i3c_generic_ibi_recycle_slot(data->ibi_pool, slot);
}

static void dw_i3c_master_drain_ibi_queue(struct dw_i3c_master *master,
					  int len)
{
	int i;

	for (i = 0; i < DIV_ROUND_UP(len, 4); i++)
		readl(master->regs + IBI_QUEUE_STATUS);
}

static void dw_i3c_master_handle_ibi_sir(struct dw_i3c_master *master,
					 u32 status)
{
	struct dw_i3c_i2c_dev_data *data;
	struct i3c_ibi_slot *slot;
	struct i3c_dev_desc *dev;
	unsigned long flags;
	u8 addr, len;
	int idx;

	addr = IBI_QUEUE_IBI_ADDR(status);
	len = IBI_QUEUE_STATUS_DATA_LEN(status);

	/*
	 * We be tempted to check the error status in bit 30; however, due
	 * to the PEC errata workaround on some platform implementations (see
	 * ast2600_i3c_set_dat_ibi()), those will almost always have a PEC
	 * error on IBI payload data, as well as losing the last byte of
	 * payload.
	 *
	 * If we implement error status checking on that bit, we may need
	 * a new platform op to validate it.
	 */

	spin_lock_irqsave(&master->devs_lock, flags);
	idx = dw_i3c_master_get_addr_pos(master, addr);
	if (idx < 0) {
		dev_err_ratelimited(&master->base.dev,
				    "IBI from unknown addr 0x%x\n", addr);
		goto err_drain;
	}

	dev = master->devs[idx].ibi_dev;
	if (!dev || !dev->ibi) {
		dev_err_ratelimited(&master->base.dev,
				    "IBI from non-requested dev idx %d\n", idx);
		goto err_drain;
	}

	data = i3c_dev_get_master_data(dev);
	if (dev->ibi->max_payload_len < len) {
		dev_err_ratelimited(&master->base.dev,
				    "IBI payload len %d greater than max %d\n",
				    len, dev->ibi->max_payload_len);
		goto err_drain;
	}

	slot = i3c_generic_ibi_get_free_slot(data->ibi_pool);
	if (!slot) {
		dev_err_ratelimited(&master->base.dev,
				    "No IBI slots available\n");
		goto err_drain;
	}

	if (len)
		dw_i3c_master_read_ibi_fifo(master, slot->data, len);

	slot->len = len;

	i3c_master_queue_ibi(dev, slot);

	spin_unlock_irqrestore(&master->devs_lock, flags);

	return;

err_drain:
	spin_lock(&master->xferqueue.lock);
	dump_regs_locked(master);
	spin_unlock(&master->xferqueue.lock);

	dw_i3c_master_drain_ibi_queue(master, len);

	spin_unlock_irqrestore(&master->devs_lock, flags);
}

static void dw_i3c_master_xfer_push_cmd_queue_locked(struct dw_i3c_master *master,
						     u32 queue_status_level)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	unsigned int empty_cmds;
	int i;

	/* Per dw_i3c_cmd will push two commands into the command queue */
	empty_cmds = QUEUE_STATUS_LEVEL_CMD(queue_status_level) / 2;

	for (i = 0; i < empty_cmds && xfer->cmd_idx < xfer->ncmds;
	     i++, xfer->cmd_idx++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[xfer->cmd_idx];

		writel(cmd->cmd_hi, master->regs + COMMAND_QUEUE_PORT);
		writel(cmd->cmd_lo, master->regs + COMMAND_QUEUE_PORT);
	}
}

static bool dw_i3c_master_xfer_pull_resp_queue_locked(struct dw_i3c_master *master,
						      u32 queue_status_level)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	unsigned int nresps;
	int i;

	nresps = QUEUE_STATUS_LEVEL_RESP(queue_status_level);

	for (i = 0; i < nresps && xfer->resp_idx < xfer->ncmds; i++, xfer->resp_idx++) {
		struct dw_i3c_cmd *cmd = &xfer->cmds[xfer->resp_idx];

		cmd->resp = readl(master->regs + RESPONSE_QUEUE_PORT);

		if (RESPONSE_PORT_ERR_STATUS(cmd->resp)) {
			/* ENTDAA is expected to end with IBA NACK */
			if (xfer->end_with_iba_nack &&
			    RESPONSE_PORT_ERR_STATUS(cmd->resp) == RESPONSE_ERROR_IBA_NACK)
				continue;
			else {
				dev_err(&master->base.dev,
					"%s: resp err status: %#08x, expect nack %d\n", __func__,
					cmd->resp, xfer->end_with_iba_nack);
				dump_regs_locked(master);
				xfer->ret = -EIO;
				break;
			}
		} else if (RESPONSE_PORT_TID(cmd->resp) != COMMON_COMMAND_CODE_TID &&
			   RESPONSE_PORT_TID(cmd->resp) != COMMAND_PORT_TID_MASK(xfer->resp_idx)) {
			dev_err(&master->base.dev,
				"%s: resp RID: %lu does not match the current TID: %u\n",
				__func__, RESPONSE_PORT_TID(cmd->resp), xfer->resp_idx);
			dump_regs_locked(master);
			xfer->ret = -EIO;
			break;
		} else if (COMMAND_PORT_CMD_ATTR_MASK(cmd->cmd_lo) == COMMAND_PORT_ADDR_ASSGN_CMD &&
			   cmd->rx_len != RESPONSE_PORT_DATA_LEN(cmd->resp)) {
			/* For write transfer, the remaining data length of the transfer if
			 * terminated early.
			 * For Read transfer, this field represents the actual amount of data
			 * received in bytes.
			 * For Address Assignment command, this field represents the remaining
			 * device count.
			 */
			dev_err(&master->base.dev,
				"%s: resp RX LEN: %lu does not match the current RX LEN: %u\n",
				__func__, RESPONSE_PORT_DATA_LEN(cmd->resp), cmd->rx_len);
			dump_regs_locked(master);
			xfer->ret = -EIO;
			break;
		}

		if (dw_i3c_xfer_error(xfer, cmd))
			break;
	}
	return xfer->ret != -ETIMEDOUT;
}

static void dw_i3c_master_xfer_rd_rx_data_locked(struct dw_i3c_master *master,
						 u32 buf_status_level)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	struct dw_i3c_cmd *cur_cmd;
	u16 buf_len, rx_buf_level;
	u8 *rx_bytes;

	if (!xfer) {
		dev_err(&master->base.dev,
			"%s: data remain in DATA buffer with no xfer processing\n",
			__func__);
		dump_regs_locked(master);
		return;
	}

	rx_buf_level = DATA_BUFFER_STATUS_LEVEL_RX(buf_status_level);

	while (rx_buf_level && xfer->rx_cmd_idx < xfer->ncmds) {
		cur_cmd = &xfer->cmds[xfer->rx_cmd_idx];

		if (cur_cmd->rx_len <= xfer->rx_buf_cursor) {
			xfer->rx_cmd_idx++;
			xfer->rx_buf_cursor = 0;
			continue;
		}

		buf_len = min(rx_buf_level * DATA_BUF_ENTRY_SIZE,
			      cur_cmd->rx_len - xfer->rx_buf_cursor);
		rx_bytes = cur_cmd->rx_buf;
		dw_i3c_master_read_rx_fifo(master,
					   rx_bytes + xfer->rx_buf_cursor,
					   buf_len);

		rx_buf_level -= (buf_len + DATA_BUF_ENTRY_SIZE - 1) /
				DATA_BUF_ENTRY_SIZE;
		xfer->rx_buf_cursor += buf_len;
		xfer->res_rx_entries -= (buf_len + DATA_BUF_ENTRY_SIZE - 1) /
					DATA_BUF_ENTRY_SIZE;
	}
}

static void dw_i3c_master_xfer_wr_tx_data_locked(struct dw_i3c_master *master,
						 u32 buf_status_level)
{
	struct dw_i3c_xfer *xfer = master->xferqueue.cur;
	struct dw_i3c_cmd *cur_cmd;
	u16 buf_len, tx_empty_buf_level;
	const u8 *tx_bytes;

	if (!xfer) {
		dev_err(&master->base.dev,
			"%s: data remain in DATA buffer with no xfer processing\n",
			__func__);
		dump_regs_locked(master);
		return;
	}

	tx_empty_buf_level = DATA_BUFFER_STATUS_LEVEL_TX(buf_status_level);

	while (tx_empty_buf_level && xfer->tx_cmd_idx < xfer->ncmds) {
		cur_cmd = &xfer->cmds[xfer->tx_cmd_idx];

		if (cur_cmd->tx_len <= xfer->tx_buf_cursor) {
			xfer->tx_cmd_idx++;
			xfer->tx_buf_cursor = 0;
			continue;
		}

		buf_len = min(tx_empty_buf_level * DATA_BUF_ENTRY_SIZE,
			      cur_cmd->tx_len - xfer->tx_buf_cursor);
		tx_bytes = cur_cmd->tx_buf;
		dw_i3c_master_wr_tx_fifo(master, tx_bytes + xfer->tx_buf_cursor,
					 buf_len);

		tx_empty_buf_level -= (buf_len + DATA_BUF_ENTRY_SIZE - 1) /
				      DATA_BUF_ENTRY_SIZE;
		xfer->tx_buf_cursor += buf_len;
		xfer->res_tx_entries -= (buf_len + DATA_BUF_ENTRY_SIZE - 1) /
					DATA_BUF_ENTRY_SIZE;
	}
}

/* "ibis": referring to In-Band Interrupts, and not
 * https://en.wikipedia.org/wiki/Australian_white_ibis. The latter should
 * not be handled.
 */
static void dw_i3c_master_irq_handle_ibis(struct dw_i3c_master *master)
{
	unsigned int i, len, n_ibis;
	u32 reg;

	reg = readl(master->regs + QUEUE_STATUS_LEVEL);
	n_ibis = QUEUE_STATUS_IBI_STATUS_CNT(reg);
	if (!n_ibis)
		return;

	for (i = 0; i < n_ibis; i++) {
		reg = readl(master->regs + IBI_QUEUE_STATUS);

		if (IBI_TYPE_SIRQ(reg)) {
			dw_i3c_master_handle_ibi_sir(master, reg);
		} else {
			len = IBI_QUEUE_STATUS_DATA_LEN(reg);
			dev_info(&master->base.dev,
				 "unsupported IBI type 0x%lx len %d\n",
				 IBI_QUEUE_STATUS_IBI_ID(reg), len);
			dw_i3c_master_drain_ibi_queue(master, len);
		}
	}
}

static irqreturn_t dw_i3c_master_irq_handler(int irq, void *dev_id)
{
	struct dw_i3c_master *master = dev_id;
	u32 intr_status, buf_status_level, queue_status_level;

	intr_status = readl(master->regs + INTR_STATUS);

	if (!(intr_status & readl(master->regs + INTR_STATUS_EN))) {
		writel(INTR_ALL, master->regs + INTR_STATUS);
		return IRQ_NONE;
	}

	spin_lock(&master->xferqueue.lock);

	if (intr_status & INTR_TRANSFER_ERR_STAT)
		writel(INTR_TRANSFER_ERR_STAT, master->regs + INTR_STATUS);

	if (master->xferqueue.cur) {
		buf_status_level =
			readl(master->regs + DATA_BUFFER_STATUS_LEVEL);
		dw_i3c_master_xfer_wr_tx_data_locked(master, buf_status_level);
		dw_i3c_master_xfer_rd_rx_data_locked(master, buf_status_level);

		queue_status_level = readl(master->regs + QUEUE_STATUS_LEVEL);
		dw_i3c_master_xfer_push_cmd_queue_locked(master,
							 queue_status_level);

		if (dw_i3c_master_xfer_pull_resp_queue_locked(master, queue_status_level))
			dw_i3c_master_end_xfer_locked(master);
		else if (dw_i3c_xfer_finished(master->xferqueue.cur))
			dw_i3c_master_end_xfer_locked(master);
		else
			dw_i3c_master_program_xfer_irq_thld_locked(master);
	}

	spin_unlock(&master->xferqueue.lock);

	if (intr_status & INTR_IBI_THLD_STAT)
		dw_i3c_master_irq_handle_ibis(master);

	return IRQ_HANDLED;
}

static const struct i3c_master_controller_ops dw_mipi_i3c_ops = {
	.bus_init = dw_i3c_master_bus_init,
	.bus_cleanup = dw_i3c_master_bus_cleanup,
	.attach_i3c_dev = dw_i3c_master_attach_i3c_dev,
	.reattach_i3c_dev = dw_i3c_master_reattach_i3c_dev,
	.detach_i3c_dev = dw_i3c_master_detach_i3c_dev,
	.do_daa = dw_i3c_master_daa,
	.supports_ccc_cmd = dw_i3c_master_supports_ccc_cmd,
	.send_ccc_cmd = dw_i3c_master_send_ccc_cmd,
	.priv_xfers = dw_i3c_master_priv_xfers,
	.attach_i2c_dev = dw_i3c_master_attach_i2c_dev,
	.detach_i2c_dev = dw_i3c_master_detach_i2c_dev,
	.i2c_xfers = dw_i3c_master_i2c_xfers,
};

static const struct i3c_master_controller_ops dw_mipi_i3c_ibi_ops = {
	.bus_init = dw_i3c_master_bus_init,
	.bus_cleanup = dw_i3c_master_bus_cleanup,
	.attach_i3c_dev = dw_i3c_master_attach_i3c_dev,
	.reattach_i3c_dev = dw_i3c_master_reattach_i3c_dev,
	.detach_i3c_dev = dw_i3c_master_detach_i3c_dev,
	.do_daa = dw_i3c_master_daa,
	.supports_ccc_cmd = dw_i3c_master_supports_ccc_cmd,
	.send_ccc_cmd = dw_i3c_master_send_ccc_cmd,
	.priv_xfers = dw_i3c_master_priv_xfers,
	.attach_i2c_dev = dw_i3c_master_attach_i2c_dev,
	.detach_i2c_dev = dw_i3c_master_detach_i2c_dev,
	.i2c_xfers = dw_i3c_master_i2c_xfers,
	.request_ibi = dw_i3c_master_request_ibi,
	.free_ibi = dw_i3c_master_free_ibi,
	.enable_ibi = dw_i3c_master_enable_ibi,
	.disable_ibi = dw_i3c_master_disable_ibi,
	.recycle_ibi_slot = dw_i3c_master_recycle_ibi_slot,
};

static int dw_i3c_master_enable_clks(struct dw_i3c_master *master)
{
	int ret = 0;

	ret = clk_prepare_enable(master->core_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(master->apb_clk);
	if (ret) {
		clk_disable_unprepare(master->core_clk);
		return ret;
	}

	return 0;
}

static inline void dw_i3c_master_disable_clks(struct dw_i3c_master *master)
{
	clk_disable_unprepare(master->apb_clk);
	clk_disable_unprepare(master->core_clk);
}

/* default platform ops implementations */
static int dw_i3c_platform_init_nop(struct dw_i3c_master *i3c)
{
	return 0;
}

static void dw_i3c_platform_set_dat_ibi_nop(struct dw_i3c_master *i3c,
					struct i3c_dev_desc *dev,
					bool enable, u32 *dat)
{
}

static u32 dw_i3c_apply_ixc_threshold_property(struct platform_device *pdev, enum ixc ixc_sel)
{
	u32 val;

	if (ixc_sel == IXC_I2C) {
		val = I2C_HYBRID_BUSY_WAIT_BYTES_THLD;
		of_property_read_u32(pdev->dev.of_node, "i2c-hybrid-threshold", &val);
	} else {
		val = I3C_HYBRID_BUSY_WAIT_BYTES_THLD;
		of_property_read_u32(pdev->dev.of_node, "i3c-hybrid-threshold", &val);
	}

	return val;
}

static const struct dw_i3c_platform_ops dw_i3c_platform_ops_default = {
	.init = dw_i3c_platform_init_nop,
	.set_dat_ibi = dw_i3c_platform_set_dat_ibi_nop,
};

int dw_i3c_common_probe(struct dw_i3c_master *master,
			struct platform_device *pdev)
{
	const struct i3c_master_controller_ops *ops;
	bool ext_power_control;
	int ret, irq;

	if (!master->platform_ops)
		master->platform_ops = &dw_i3c_platform_ops_default;

	master->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(master->regs))
		return PTR_ERR(master->regs);

	master->core_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(master->core_clk))
		return PTR_ERR(master->core_clk);

	master->apb_clk = devm_clk_get_optional(&pdev->dev, "apb_clk");
	if (IS_ERR(master->apb_clk))
		return PTR_ERR(master->apb_clk);

	master->core_rst = devm_reset_control_get_optional_exclusive(&pdev->dev,
								    "core_rst");
	if (IS_ERR(master->core_rst))
		return PTR_ERR(master->core_rst);

	ret = dw_i3c_master_enable_clks(master);
	if (ret)
		return ret;

	master->io_vreg_dev = devm_regulator_get_optional(&pdev->dev, "io-vreg");
	if (IS_ERR(master->io_vreg_dev)) {
		if (PTR_ERR(master->io_vreg_dev) == -ENODEV) {
			master->io_vreg_dev = NULL;
		} else {
			dev_err(&pdev->dev, "camio regulator is not ready\n");
			return PTR_ERR(master->io_vreg_dev);
		}
	}

	if (master->io_vreg_dev) {
		ret = regulator_enable(master->io_vreg_dev);
		if (ret) {
			dev_err(&pdev->dev, "Fail to enable camio regulator\n");
			goto err_assert_rst;
		}
	}

	reset_control_deassert(master->core_rst);

	spin_lock_init(&master->xferqueue.lock);
	INIT_LIST_HEAD(&master->xferqueue.list);

	writel(INTR_ALL, master->regs + INTR_STATUS);
	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq,
			       dw_i3c_master_irq_handler, 0,
			       dev_name(&pdev->dev), master);
	if (ret)
		goto err_reg_disable;

	platform_set_drvdata(pdev, master);

	/* Information regarding the FIFOs/QUEUEs depth */
	ret = readl(master->regs + QUEUE_STATUS_LEVEL);
	master->caps.cmdfifodepth = QUEUE_STATUS_LEVEL_CMD(ret);

	ret = readl(master->regs + DATA_BUFFER_STATUS_LEVEL);
	master->caps.datafifodepth = DATA_BUFFER_STATUS_LEVEL_TX(ret);

	ret = readl(master->regs + DEVICE_ADDR_TABLE_POINTER);
	master->datstartaddr = ret;
	master->maxdevs = ret >> 16;
	master->free_pos = GENMASK(master->maxdevs - 1, 0);

	master->ibi_capable = of_property_read_bool(pdev->dev.of_node, "ibi-capable");
	ops = &dw_mipi_i3c_ops;
	if (master->ibi_capable)
		ops = &dw_mipi_i3c_ibi_ops;

	/* Mode of operation Polling/Interrupt mode */
	if (of_property_read_bool(pdev->dev.of_node, "hybrid-busy-wait-mode"))
		master->operation_mode = HYBRID_BUSY_WAIT_MODE;
	else
		master->operation_mode = INTERRUPT_MODE;

	master->i2c_hybrid_threshold = dw_i3c_apply_ixc_threshold_property(pdev, IXC_I2C);
	master->i3c_hybrid_threshold = dw_i3c_apply_ixc_threshold_property(pdev, IXC_I3C);

	/* Increment PM usage to avoid possible spurious runtime suspend */
	pm_runtime_get_noresume(&pdev->dev);

	ext_power_control = of_property_read_bool(pdev->dev.of_node, "external-power-control");
	if (!ext_power_control) {
		pm_runtime_set_autosuspend_delay(&pdev->dev, RPM_AUTOSUSPEND_TIMEOUT_MS);
		pm_runtime_use_autosuspend(&pdev->dev);
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = i3c_master_register(&master->base, &pdev->dev, ops, false);
	if (ret)
		goto err_disable_pm;

	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	if (ext_power_control)
		pm_runtime_suspend(&pdev->dev);

	return 0;

err_disable_pm:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);

err_reg_disable:
	if (master->io_vreg_dev && regulator_disable(master->io_vreg_dev))
		dev_err(&pdev->dev, "failed to disable camio regulator\n");

err_assert_rst:
	reset_control_assert(master->core_rst);

	dw_i3c_master_disable_clks(master);

	return ret;
}
EXPORT_SYMBOL_GPL(dw_i3c_common_probe);

/* base platform implementation */

static int dw_i3c_probe(struct platform_device *pdev)
{
	struct dw_i3c_master *master;

	master = devm_kzalloc(&pdev->dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	master->dev = &pdev->dev;

	return dw_i3c_common_probe(master, pdev);
}

static void dw_i3c_remove(struct platform_device *pdev)
{
	struct dw_i3c_master *master = platform_get_drvdata(pdev);

	i3c_master_unregister(&master->base);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);

	reset_control_assert(master->core_rst);

	dw_i3c_master_disable_clks(master);
}

static void dw_i3c_master_restore_addrs(struct dw_i3c_master *master)
{
	u32 pos, reg_val;

	writel(DEV_ADDR_DYNAMIC_ADDR_VALID | DEV_ADDR_DYNAMIC(master->dev_addr),
	       master->regs + DEVICE_ADDR);

	for (pos = 0; pos < master->maxdevs; pos++) {
		if (master->free_pos & BIT(pos))
			continue;

		if (master->devs[pos].is_i2c)
			reg_val = DEV_ADDR_TABLE_LEGACY_I2C_DEV |
			       DEV_ADDR_TABLE_STATIC_ADDR(master->devs[pos].addr);
		else
			reg_val = DEV_ADDR_TABLE_DYNAMIC_ADDR(master->devs[pos].addr);

		writel(reg_val, master->regs + DEV_ADDR_TABLE_LOC(master->datstartaddr, pos));
	}
}

static void dw_i3c_master_restore_timing_regs(struct dw_i3c_master *master)
{
	writel(master->i3c_pp_timing, master->regs + SCL_I3C_PP_TIMING);
	writel(master->bus_free_timing, master->regs + BUS_FREE_TIMING);
	writel(master->i3c_od_timing, master->regs + SCL_I3C_OD_TIMING);
	writel(master->ext_lcnt_timing, master->regs + SCL_EXT_LCNT_TIMING);

	if (master->base.bus.mode != I3C_BUS_MODE_PURE) {
		writel(master->i2c_fmp_timing, master->regs + SCL_I2C_FMP_TIMING);
		writel(master->i2c_fm_timing, master->regs + SCL_I2C_FM_TIMING);
	}
}

static int __maybe_unused dw_i3c_master_runtime_suspend(struct device *dev)
{
	struct dw_i3c_master *master = dev_get_drvdata(dev);
	int ret = 0;

	dw_i3c_master_disable(master);

	reset_control_assert(master->core_rst);
	dw_i3c_master_disable_clks(master);
	pinctrl_pm_select_sleep_state(dev);

	if (master->io_vreg_dev) {
		ret = regulator_disable(master->io_vreg_dev);
		if (ret)
			dev_err(dev, "failed to disable camio regulator\n");
	}

	return ret;
}

static int __maybe_unused dw_i3c_master_runtime_resume(struct device *dev)
{
	struct dw_i3c_master *master = dev_get_drvdata(dev);
	int ret = 0;

	if (master->io_vreg_dev) {
		ret = regulator_enable(master->io_vreg_dev);
		if (ret)
			dev_err(dev, "Fail to enable camio regulator\n");
	}

	pinctrl_pm_select_default_state(dev);
	dw_i3c_master_enable_clks(master);
	reset_control_deassert(master->core_rst);

	dw_i3c_master_set_intr_regs(master);
	dw_i3c_master_restore_timing_regs(master);
	dw_i3c_master_restore_addrs(master);
	dw_i3c_master_enable(master);

	return ret;
}

static int dw_i3c_master_suspend(struct device *dev)
{
	struct dw_i3c_master *master = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&master->base.i2c);
	return pm_runtime_force_suspend(dev);
}

static int dw_i3c_master_resume(struct device *dev)
{
	struct dw_i3c_master *master = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret < 0)
		return ret;
	i2c_mark_adapter_resumed(&master->base.i2c);

	return 0;
}

static const struct dev_pm_ops dw_i3c_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw_i3c_master_suspend, dw_i3c_master_resume)
	SET_RUNTIME_PM_OPS(dw_i3c_master_runtime_suspend, dw_i3c_master_runtime_resume, NULL)
};

static const struct of_device_id dw_i3c_master_of_match[] = {
	{ .compatible = "snps,dw-i3c-master-1.00a", },
	{},
};
MODULE_DEVICE_TABLE(of, dw_i3c_master_of_match);

static struct platform_driver dw_i3c_driver = {
	.probe = dw_i3c_probe,
	.remove_new = dw_i3c_remove,
	.driver = {
		.name = "dw-i3c-master",
		.of_match_table = dw_i3c_master_of_match,
		.pm = &dw_i3c_pm_ops,
	},
};
module_platform_driver(dw_i3c_driver);

MODULE_AUTHOR("Vitor Soares <vitor.soares@synopsys.com>");
MODULE_DESCRIPTION("DesignWare MIPI I3C driver");
MODULE_LICENSE("GPL v2");
