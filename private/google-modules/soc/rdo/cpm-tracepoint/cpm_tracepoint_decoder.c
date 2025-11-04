// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024 Google LLC */

#include <linux/kernel.h>
#include <linux/string.h>
#include "cpm_tracepoint_decoder.h"
#include "thermal_tracepoints.h"
#include "perf_tracepoints.h"

bool clients_initialized;

/* List of client tracepoints to decode and have customized handling */
struct client_tracepoint *client_tracepoints[] = {
	&thermal_tj_pid_curr_state,
	&domain_freq_tp,
};

/*
 * Function that handles client tracepoint string matching and handling
 * Returns the enum tracepoint_handle that will be used by google_cpm_tracepoint driver
 */
#define NUM_CHAR_TO_MATCH 64
enum tracepoint_handle cpm_tracepoint_decode(const char *tp_string, u32 payload,
					     u64 timestamp)
{
	for (int i = 0; i < ARRAY_SIZE(client_tracepoints); i++) {
		if ((!client_tracepoints[i]->enabled) ||
		    (client_tracepoints[i]->handler == NULL))
			continue;
		if (!strncmp(tp_string, client_tracepoints[i]->tp_string, NUM_CHAR_TO_MATCH)) {
			return client_tracepoints[i]->handler(tp_string,
							      payload,
							      timestamp);
		}
	}
	return CLIENT_TP_HANDLING_NOT_COMPLETE;
}

/*
 * Function that handles client tracepoint functionality initialization
 * Called when tracepoint driver activates and requests CPM tracepoints
 */
void client_init_callbacks(void)
{
	if (clients_initialized)
		return;

	for (int i = 0; i < ARRAY_SIZE(client_tracepoints); i++) {
		if ((client_tracepoints[i]->enabled) &&
		    (client_tracepoints[i]->init))
			client_tracepoints[i]->init();
	}

	clients_initialized = true;
}

/*
 * Function that handles client tracepoint functionality clean-up
 * Called when tracepoint driver deactivates and requests CPM to stop sending tracepoints
 */
void client_exit_callbacks(void)
{
	if (!clients_initialized)
		return;

	for (int i = 0; i < ARRAY_SIZE(client_tracepoints); i++) {
		if ((client_tracepoints[i]->enabled) &&
		    (client_tracepoints[i]->exit))
			client_tracepoints[i]->exit();
	}

	clients_initialized = false;
}

void initialize_cpm_tracepoint_decoder(void)
{
	clients_initialized = false;
}
