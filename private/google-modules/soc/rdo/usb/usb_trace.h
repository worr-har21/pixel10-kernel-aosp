/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM usb_trace

#if !defined(_TRACE_USB_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_USB_H

#include <linux/tracepoint.h>
#include <linux/api-compat.h>

TRACE_EVENT(platform_usb_suspend_start,
	    TP_PROTO(const char *name),
	    TP_ARGS(name),
	    TP_STRUCT__entry(__string(name, name)),
	    TP_fast_assign(assign_str_wrp(name, name);),
	    TP_printk("[%s]\n", __get_str(name)));

TRACE_EVENT(platform_usb_suspend_end,
	    TP_PROTO(const char *name),
	    TP_ARGS(name),
	    TP_STRUCT__entry(__string(name, name)),
	    TP_fast_assign(assign_str_wrp(name, name);),
	    TP_printk("[%s]\n", __get_str(name)));

TRACE_EVENT(platform_usb_resume_start,
	    TP_PROTO(const char *name),
	    TP_ARGS(name),
	    TP_STRUCT__entry(__string(name, name)),
	    TP_fast_assign(assign_str_wrp(name, name);),
	    TP_printk("[%s]\n", __get_str(name)));

TRACE_EVENT(platform_usb_resume_end,
	    TP_PROTO(const char *name),
	    TP_ARGS(name),
	    TP_STRUCT__entry(__string(name, name)),
	    TP_fast_assign(assign_str_wrp(name, name);),
	    TP_printk("[%s]\n", __get_str(name)));

#endif /* _TRACE_USB_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE usb_trace
