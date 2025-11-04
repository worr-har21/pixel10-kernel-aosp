/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2024 Google LLC
 */

/* This header is internal only.
 *
 * Public APIs are in //private/google-modules/soc/gs/include/linux/gsa/
 *
 * Include via //private/google-modules/soc/gs:gs_soc_headers
 */

#ifndef __LINUX_TZPROT_IPC_H
#define __LINUX_TZPROT_IPC_H

#include <linux/types.h>

#define TZPROT_PORT "com.android.trusty.media_prot"

enum media_prot_cmd {
	MEDIA_PROT_CMD_RESP = (1U << 31),
	MEDIA_PROT_CMD_SET_IP_PROT = 0,
};

struct media_prot_set_ip_prot_req {
	u32 dev_id;
	u32 enable;
};

struct media_prot_req {
	u32 cmd;
	union {
		struct media_prot_set_ip_prot_req set_ip_prot_req;
	};
};

struct media_prot_rsp {
	uint32_t cmd;
	int32_t err;
};

#endif

