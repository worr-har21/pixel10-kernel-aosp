/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024 Google LLC */

#ifndef _CPM_TRACEPOINT_DECODER_H
#define _CPM_TRACEPOINT_DECODER_H

#include <linux/types.h>

enum tracepoint_handle {
	CLIENT_TP_HANDLING_COMPLETE,
	CLIENT_TP_HANDLING_NOT_COMPLETE,
	CLIENT_TP_HANDLING_ERROR,
};

struct client_tracepoint {
	// flag to turn on/off tracepoint decode
	bool enabled;
	// string to match for decode
	const char *tp_string;
	// decode handler to be called for a string match
	enum tracepoint_handle (*handler)(const char *tp_string, u32 payload,
					  u64 timestamp);
	/*
	 * [optional] callback to initialize internal state of the tracepoint
	 * decode functionality
	 */
	void (*init)(void);
	/*
	 * [optional] callback to clean-up internal state of the tracepoint
	 * decode functionality
	 */
	void (*exit)(void);
};

enum tracepoint_handle cpm_tracepoint_decode(const char *tp_string, u32 payload,
					     u64 timestamp);
void client_init_callbacks(void);
void client_exit_callbacks(void);

void initialize_cpm_tracepoint_decoder(void);

void add_cpm_param_trace(char *param_name, unsigned int value,
			 unsigned long timestamp);

#endif /* _CPM_TRACEPOINT_DECODER_H */
