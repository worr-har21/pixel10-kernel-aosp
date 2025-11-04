/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent configuration for TPU mailboxes.
 *
 * Copyright (C) 2024 Google LLC
 */

#ifndef __BUENOS_CONFIG_MAILBOX_H__
#define __BUENOS_CONFIG_MAILBOX_H__

#include <linux/types.h> /* u32 */

#include "config.h"

#define EDGETPU_NUM_VII_MAILBOXES 38
#define EDGETPU_NUM_P2P_MAILBOXES 0
#define EDGETPU_NUM_EXT_MAILBOXES 2 // 1 DSP and 1 AOC NS
#define EDGETPU_NUM_MAILBOXES (EDGETPU_NUM_VII_MAILBOXES + EDGETPU_NUM_EXT_MAILBOXES + 1)
#define EDGETPU_EXT_DSP_MAILBOX_START (EDGETPU_NUM_VII_MAILBOXES + 1)
#define EDGETPU_EXT_DSP_MAILBOX_END (EDGETPU_EXT_DSP_MAILBOX_START)
#define EDGETPU_EXT_AOC_MAILBOX_START (EDGETPU_EXT_DSP_MAILBOX_START + 1)
#define EDGETPU_EXT_AOC_MAILBOX_END (EDGETPU_EXT_AOC_MAILBOX_START)

#define BUENOS_CSR_MBOX6_CONTEXT_ENABLE 0xc000 /* starting kernel mb */
#define EDGETPU_MBOX_CSRS_SIZE 0x2000 /* CSR size of each mailbox */

#define BUENOS_CSR_MBOX_CMD_QUEUE_DOORBELL_SET_OFFSET 0x1000
#define BUENOS_CSR_MBOX_RESP_QUEUE_DOORBELL_SET_OFFSET 0x1800
#define EDGETPU_MBOX_BASE BUENOS_CSR_MBOX6_CONTEXT_ENABLE

#define EDGETPU_NUM_USE_VII_MAILBOXES 7

#define EDGETPU_USE_IIF_MAILBOX 1

static inline u32 edgetpu_mailbox_get_context_csr_base(u32 index)
{
	return EDGETPU_MBOX_BASE + index * EDGETPU_MBOX_CSRS_SIZE;
}

static inline u32 edgetpu_mailbox_get_cmd_queue_csr_base(u32 index)
{
	return edgetpu_mailbox_get_context_csr_base(index) +
	       BUENOS_CSR_MBOX_CMD_QUEUE_DOORBELL_SET_OFFSET;
}

static inline u32 edgetpu_mailbox_get_resp_queue_csr_base(u32 index)
{
	return edgetpu_mailbox_get_context_csr_base(index) +
	       BUENOS_CSR_MBOX_RESP_QUEUE_DOORBELL_SET_OFFSET;
}

#endif /* __BUENOS_CONFIG_MAILBOX_H__ */
