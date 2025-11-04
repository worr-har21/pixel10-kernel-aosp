/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2025 Google LLC.
 *
 * Google firmware tracepoint ftrace services source.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM fwtp_ftrace

#if !defined(FWTP_FTRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define FWTP_FTRACE_H_

#include <linux/api-compat.h>
#include <linux/tracepoint.h>

/* Define FWTP ftrace event. */
TRACE_EVENT(fwtp, TP_PROTO(const char *fwtp_string), TP_ARGS(fwtp_string),
	    TP_STRUCT__entry(__string(fwtp_string, fwtp_string)),
	    TP_fast_assign(assign_str_wrp(fwtp_string, fwtp_string)),
	    TP_printk("%s", __get_str(fwtp_string)));

#endif /* FWTP_FTRACE_H_ */

/* This part must be outside protection. */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fwtp_ftrace
#include <trace/define_trace.h>
