// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024 Google LLC */

#include <linux/kernel.h>
#include <linux/units.h>

#include <dvfs-helper/google_dvfs_helper.h>

#include "cpm_tracepoint_decoder.h"
#include "perf_tracepoints.h"

#define TRACE_OP_LVL_BITFIELD GENMASK(15, 0)
#define EXTRACT_DOMAIN_ID(payload) (payload >> 16)
#define EXTRACT_DOMAIN_OP_LVL(payload) (payload & TRACE_OP_LVL_BITFIELD)

#define DOMAIN_TRACE_STR_LEN (MAX_DVFS_NAME_LEN + sizeof("_freq"))

enum tracepoint_handle domain_freq_handler(const char *tp_string, u32 payload,
					  u64 timestamp)
{
	u16 domain_id;
	const char *domain_name;
	char trace_name[DOMAIN_TRACE_STR_LEN];
	s64 freq;

	domain_id = EXTRACT_DOMAIN_ID(payload);
	domain_name = dvfs_helper_domain_id_to_name(domain_id);
	if (!domain_name)
		return CLIENT_TP_HANDLING_ERROR;

	freq = dvfs_helper_get_domain_opp_frequency_mhz(domain_id,
				EXTRACT_DOMAIN_OP_LVL(payload));
	if (freq < 0)
		return CLIENT_TP_HANDLING_ERROR;

	scnprintf(trace_name, sizeof(trace_name), "%s_freq", domain_name);
	add_cpm_param_trace(trace_name, KHZ_PER_MHZ * freq, timestamp);

	return CLIENT_TP_HANDLING_COMPLETE;
}

struct client_tracepoint domain_freq_tp = {
	.enabled = true,
	.tp_string = "DvfsTar",
	.init = NULL,
	.handler = domain_freq_handler,
	.exit = NULL
};
