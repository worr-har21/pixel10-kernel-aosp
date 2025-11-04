/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint printer services header.
 *
 * This header is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

#ifndef FWTP_PRINTER_H_
#define FWTP_PRINTER_H_

#ifdef __KERNEL__
#include <linux/types.h>

#include "fwtp_platform.h"
#include "tracepoint_ring.h"
#else
#include <lib/tracepoint/tracepoint_ring.h>
#include <lib/utils/compiler.h>
#include <stdbool.h>
#include <stdint.h>
#endif

__BEGIN_CDECLS

/* Forward declarations. */
struct fwtp_printer_ctx;

/*
 * This structure encapsulates a tracepoint data item.
 */
struct fwtp_data_item {
	/* Size of data item data. */
	uint8_t data_size;
	/* Data item data. */
	uint8_t data[];
};

/*
 * This structure encapsulates a list of data items from a tracepoint. It may be
 * used to read data items one by one from the decoded tracepoint.
 *
 * This structure must be initialized with a data buffer in the data_buffer and
 * data_buffer_size fields.
 */
struct fwtp_data_item_list {
	/* Buffer containing data items. */
	uint8_t *data_buffer;
	/* Size of data buffer. */
	unsigned int data_buffer_size;
	/* Total size of all data items in buffer. */
	unsigned int total_data_size;
	/* Count of the number of data items. */
	unsigned int item_count;
	/* Pointer to the location of the next data item to be read or written. */
	uint8_t *p_next_data_item;
};

/*
 * A function of this type returns the string corresponding to the string ID
 * specified by string_id. The printer context is specified by printer_ctx.
 *
 *   printer_ctx            Printer context.
 *   string_id              ID of string to get.
 */
typedef const char *(*fwtp_get_string_func)(struct fwtp_printer_ctx *printer_ctx,
					    uint32_t string_id);

/*
 * A function of this type appends the string specified by str to some output
 * (e.g., UART or log file). The printer context is specified by printer_ctx.
 *
 *   printer_ctx            Printer context.
 *   str                    String to append to output.
 */
typedef void (*fwtp_append_output_func)(struct fwtp_printer_ctx *printer_ctx,
					const char *str);

/*
 * A function of this type runs post-processing using the decoded tracepoint
 * with the timestamp specified by timestamp, title string specified by str, and
 * data items specified by data_items. This function doesn't modify the
 * tracepoint or printer output, but it may collect information from the
 * tracepoint for other uses (e.g., generating a SEM report). The printer
 * context is specified by printer_ctx.
 *
 *   printer_ctx            Printer context.
 *   timestamp              Tracepoint timestamp.
 *   str                    Tracepoint title string.
 *   data_items             Tracepoint data items.
 */
typedef void (*fwtp_post_process_func)(struct fwtp_printer_ctx *printer_ctx,
				       uint64_t timestamp, const char *str,
				       struct fwtp_data_item_list *data_items);

/*
 * This structure contains fields for maintaining context when printing
 * tracepoints.
 *
 * At a minimum, the printer context must be initialized with a get_string and
 * an append_output function with the rest of the fields zeroed out.
 *
 * If output_buffer is not null, then it is used to buffer output for appending.
 * Its size is specified by output_buffer_size. When output_buffer is full or
 * output for one tracepoint is complete, append_output will be invoked with
 * output_buffer.
 *
 * If the size of a tracepoint output is less than output_buffer_size, then
 * append_output is guaranteed to be invoked with the complete output for that
 * tracepoint. This guarantee may be useful when output is being appended using
 * a logging system that expects each log line to be given in full rather than
 * in individual pieces.
 *
 * If post_process is not null, then it is invoked once for each decoded
 * tracepoint. It is provided the tracepoint title string and any tracepoint
 * data items. The data_items.data_buffer and data_items.data_buffer_size fields
 * should be set up with a buffer in which to place the data items.
 *
 * If additional context is required for any of the functions, this structure
 * may be added to a container structure with more context. The container
 * structure may then be obtained using containerof.
 *
 * E.g.,
 *
 *   struct my_printer_ctx {
 *     struct fwtp_printer_ctx base_printer_ctx;
 *     int log_level;
 *   };
 *
 *   void my_append_output(struct fwtp_printer_ctx *printer_ctx,
 *                         const char *str) {
 *     struct my_printer_ctx *my_printer_ctx =
 *         containerof(printer_ctx, struct my_printer_ctx, base_printer_ctx);
 *     LOG(my_printer_ctx->log_level, str);
 *   }
 */
struct fwtp_printer_ctx {
	/* Get string function. */
	fwtp_get_string_func get_string;
	/* Context for get string function. */
	void *get_string_ctx;
	/* Append output function. */
	fwtp_append_output_func append_output;
	/* Context for append output function. */
	void *append_output_ctx;
	/* Tracepoint post-processing function. */
	fwtp_post_process_func post_process;
	/* Context for tracepoint post-processing function. */
	void *post_process_ctx;
	/* Tracepoint data item list for post-processing. */
	struct fwtp_data_item_list data_items;
	/* Timestatmp frequency in Hz. If zero, timestamp frequency is unspecified. */
	uint32_t timestamp_hz;
	/* Latest absolute timestamp. */
	uint64_t absolute_timestamp;
	/* Buffer used for appending output. */
	char *output_buffer;
	/* Size of output buffer. */
	int output_buffer_size;
	/* Index of the end of the output buffer content. */
	int output_buffer_end;
	/* Offset to next tracepoint entry to print. */
	uint32_t head_offset;
	/* If true, don't append new lines when printing tracepoints. */
	bool dont_append_new_line;
};

void fwtp_print_ring_entries(struct fwtp_printer_ctx *printer_ctx,
			     struct tracepoint_ring *ring,
			     int recent_entry_count);

void fwtp_print_entries(struct fwtp_printer_ctx *printer_ctx,
			const uint64_t *entry_words, int num_words);

int fwtp_get_next_data_item(struct fwtp_data_item_list *data_items,
			    void *p_data_item, int max_data_item_size);

__END_CDECLS

#endif /* FWTP_PRINTER_H_ */
