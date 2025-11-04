// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024 Google LLC */

#include <linux/kernel.h>
#include "cpm_tracepoint_decoder.h"
#include "thermal_tracepoints.h"

union curr_state_u {
	struct {
		uint8_t cdev_state;
		uint8_t tzid;
		uint8_t temp;
		uint8_t ctrl_temp;
	};
	uint32_t word32;
};

enum tz_id {
	TZ_BIG = 0,
	TZ_BIG_MID = 1,
	TZ_MID = 2,
	TZ_LIT = 3,
	TZ_GPU = 4,
	TZ_TPU = 5,
	TZ_AUR = 6,
	TZ_ISP = 7,
	TZ_MEM = 8,
	TZ_AOC = 9,
};

static const char *const tzid_string[] = {
	[TZ_BIG] = "BIG", [TZ_BIG_MID] = "BIG_MID", [TZ_MID] = "MID",
	[TZ_LIT] = "LIT", [TZ_GPU] = "GPU",	    [TZ_TPU] = "TPU",
	[TZ_AUR] = "AUR", [TZ_ISP] = "ISP",	    [TZ_MEM] = "MEM",
	[TZ_AOC] = "AOC",
};

#define CURR_STATE_STR_LEN 13
enum tracepoint_handle curr_state_handler(const char *tp_string, u32 payload,
					  u64 timestamp)
{
	union curr_state_u state;
	char clock_name[CURR_STATE_STR_LEN];

	state.word32 = payload;

	if (state.tzid >= ARRAY_SIZE(tzid_string))
		return CLIENT_TP_HANDLING_ERROR;

	scnprintf(clock_name, sizeof(clock_name), "%s_temp",
		  tzid_string[state.tzid]);
	add_cpm_param_trace(clock_name, (unsigned int)state.temp, timestamp);

	scnprintf(clock_name, sizeof(clock_name), "%s_cdev",
		  tzid_string[state.tzid]);
	add_cpm_param_trace(clock_name, (unsigned int)state.cdev_state,
			    timestamp);

	return CLIENT_TP_HANDLING_NOT_COMPLETE;
}

struct client_tracepoint thermal_tj_pid_curr_state = {
	.enabled = true,
	.tp_string = "GOVcurr",
	.init = NULL,
	.handler = curr_state_handler,
	.exit = NULL
};
