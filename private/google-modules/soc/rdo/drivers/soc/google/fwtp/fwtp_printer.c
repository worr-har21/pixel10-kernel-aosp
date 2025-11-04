// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint printer services.
 *
 * This header is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

/* Always build with FWTP enabled. */
#ifndef FWTP_ENABLED
#define FWTP_ENABLED 1
#endif

/* Self include. */
#ifdef __KERNEL__
#include "fwtp_printer.h"
#else
#include "lib/fwtp/fwtp_printer.h"
#endif

/* System includes. */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>
#define PRIu64 "llu"
#else
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static inline uint64_t div_u64(uint64_t dividend, uint32_t divisor)
{
	return dividend / divisor;
}
#endif

/* Google includes. */
#ifdef __KERNEL__
#include "fwtp_entry.h"
#else
#include "lib/fwtp/fwtp_entry.h"
#endif

/*******************************************************************************
 *
 * Internal services.
 */

/*
 * Adds a data item with the data specified by data and data_size to the data
 * item list specified by data_items.
 *
 *   data_items             Data item list to which to add data item.
 *   data                   Data item data.
 *   data_size              Size of data item data.
 */
static void fwtp_add_data_item(struct fwtp_data_item_list *data_items,
			       void *data, int data_size)
{
	struct fwtp_data_item *data_item;
	int data_item_size;
	int rem_data_buffer_size;

	/* Do nothing if there's no data buffer. */
	if (!data_items->data_buffer)
		return;

	/* Get the data item size and the remaining data buffer size. */
	data_item_size = sizeof(struct fwtp_data_item) + data_size;
	rem_data_buffer_size =
		data_items->data_buffer_size -
		(data_items->p_next_data_item - data_items->data_buffer);

	/* Do nothing if there's no space for the data item. */
	if (rem_data_buffer_size < data_item_size)
		return;

	/* Add the data item. */
	data_item = (struct fwtp_data_item *)(data_items->p_next_data_item);
	data_item->data_size = data_size;
	memcpy(data_item->data, data, data_size);
	data_items->p_next_data_item += data_item_size;
	data_items->total_data_size += data_item_size;
	data_items->item_count++;
}

/*
 * Flushes the output buffer in the printer context specified by printer_ctx.
 *
 *   printer_ctx            Printer context.
 */
static void fwtp_flush_output_buffer(struct fwtp_printer_ctx *printer_ctx)
{
	if (printer_ctx->output_buffer_end > 0) {
		printer_ctx->output_buffer[printer_ctx->output_buffer_end] =
			'\0';
		printer_ctx->append_output(printer_ctx,
					   printer_ctx->output_buffer);
		printer_ctx->output_buffer_end = 0;
	}
}

/*
 * Appends the string specified by str to some output. The printer context is
 * specified by printer_ctx.
 *
 * If printer_ctx has an output buffer, buffers the output before invoking
 * append_output.
 *
 *   printer_ctx            Printer context.
 *   str                    String to append to output.
 */
static void fwtp_buffered_append_output(struct fwtp_printer_ctx *printer_ctx,
					const char *str)
{
	int str_len;
	int i;

	/*
	 * Append output directly if there's no buffer or buffer is not large enough
	 * for one character and one null character.
	 */
	if (printer_ctx->output_buffer_size < 2) {
		printer_ctx->append_output(printer_ctx, str);
		return;
	}

	/* Buffer the output. */
	str_len = strlen(str);
	for (i = 0; i < str_len; ++i) {
		/* If the buffer is full, flush it. */
		if (printer_ctx->output_buffer_end >=
		    (printer_ctx->output_buffer_size - 1)) {
			fwtp_flush_output_buffer(printer_ctx);
		}

		/* Buffer another character. */
		printer_ctx->output_buffer[printer_ctx->output_buffer_end++] =
			str[i];
	}
}

/*
 * Prints the tracepoint with data entry with the tracepoint name string
 * specified by tracepoint_string and data specified by data and data_size.
 * The printer context is specified by printer_ctx.
 *
 *   printer_ctx            Printer context.
 *   tracepoint_string      Tracepoint name string.
 *   data                   Tracepoint data.
 *   data_size              Size of tracepoint data.
 */
static void
fwtp_print_tracepoint_with_data_string(struct fwtp_printer_ctx *printer_ctx,
				       const char *tracepoint_string,
				       uint8_t *data, unsigned int data_size)
{
	const char *p_tp_str = tracepoint_string;
	const char *p_rem_tp_str = p_tp_str;
	uint8_t *p_data = data;
	uint8_t *p_data_end = p_data + data_size;
	char tp_char;
	char string_buffer[32];

	/* Print the tracepoint string with printf formatting. */
	while (1) {
		/* Get the next tracepoint character. */
		tp_char = *p_tp_str++;
		if (tp_char == '\0')
			break;

		/*
		 * If the tracepoint character is '%', process the printf conversion
		 * specification. Otherwise, print the tracepoint character.
		 */
		if (tp_char == '%') {
			/* Get the next tracepoint character. */
			tp_char = *p_tp_str++;
			if (tp_char == '\0')
				break;

			/*
			 * Decode the conversion specification and print the result. If the
			 * conversion specification is not recognized, stop printing.
			 */
			if ((tp_char == 'd') || (tp_char == 'u') ||
			    (tp_char == 'x')) {
				const char conversion_spec[3] = { '%', tp_char,
								  '\0' };
				union {
					int int_val;
					unsigned int uint_val;
				} val;

				/* Get the data value. */
				if ((p_data + sizeof(val)) > p_data_end)
					break;
				memcpy(&val, p_data, sizeof(val));
				p_data += sizeof(val);

				/* Print the formatted data value. */
				if (tp_char == 'd') {
					snprintf(string_buffer,
						 sizeof(string_buffer),
						 conversion_spec, val.int_val);
				} else {
					snprintf(string_buffer,
						 sizeof(string_buffer),
						 conversion_spec, val.uint_val);
				}
				fwtp_buffered_append_output(printer_ctx,
							    string_buffer);
				p_rem_tp_str = p_tp_str;

				/* Add the data item. */
				fwtp_add_data_item(&(printer_ctx->data_items),
						   &val, sizeof(val));
			} else if (tp_char == 's') {
				uint32_t val;

				/* Get the data value. */
				if ((p_data + sizeof(val)) > p_data_end)
					break;
				memcpy(&val, p_data, sizeof(val));
				p_data += sizeof(val);

				/* Print the data value. */
				fwtp_buffered_append_output(
					printer_ctx,
					printer_ctx->get_string(printer_ctx,
								val));
				p_rem_tp_str = p_tp_str;

				/* Add the data item. */
				fwtp_add_data_item(&(printer_ctx->data_items),
						   &val, sizeof(val));
			} else {
				break;
			}
		} else {
			/* Print the tracepoint character. */
			string_buffer[0] = tp_char;
			string_buffer[1] = '\0';
			fwtp_buffered_append_output(printer_ctx, string_buffer);
			p_rem_tp_str = p_tp_str;
		}
	}

	/* Print remaining tracepoint string. */
	if (*p_rem_tp_str != '\0')
		fwtp_buffered_append_output(printer_ctx, p_rem_tp_str);
}

/*
 * Prints and post-processes the tracepoint entry specified by entry_timestamp
 * and name_str using the printer context specified by printer_ctx. Any
 * associated tracepoint data is specified by p_data, data_size, and data_items.
 * If p_skip_count is non-zero, skip printing the tracepoint and decrement
 * p_skip_count; the tracepoint will still be post-processed.
 *
 *   printer_ctx            Printer context.
 *   entry_timestamp        Entry timestamp.
 *   name_str               Tracepoint name string.
 *   p_data                 Pointer to tracepoint data.
 *   data_size              Size of tracepoint data.
 *   data_items             Tracepoint data items for post-processing.
 *   p_skip_count           Count of the number of remaining tracepoints to skip
 *                          printing.
 */
static void fwtp_print_tracepoint(struct fwtp_printer_ctx *printer_ctx,
				  uint64_t entry_timestamp,
				  const char *name_str, uint8_t *p_data,
				  unsigned int data_size,
				  struct fwtp_data_item_list *data_items,
				  int *p_skip_count)
{
	char string_buffer[32];

	/* Print the tracepoint. */
	if (*p_skip_count > 0) {
		--(*p_skip_count);
	} else {
		/* Print the entry timestamp. */
		if (printer_ctx->timestamp_hz > 0) {
			uint64_t timestamp_sec =
				div_u64(entry_timestamp,
					printer_ctx->timestamp_hz);
			uint64_t timestamp_usec =
				div_u64(entry_timestamp * 1000000,
					printer_ctx->timestamp_hz) %
				1000000;
			snprintf(string_buffer, sizeof(string_buffer),
				 "%" PRIu64 ".%06" PRIu64 ": ", timestamp_sec,
				 timestamp_usec);
		} else {
			snprintf(string_buffer, sizeof(string_buffer),
				 "%" PRIu64 ": ", entry_timestamp);
		}
		fwtp_buffered_append_output(printer_ctx, string_buffer);

		/* Print the entry string. */
		if (p_data && data_size) {
			fwtp_print_tracepoint_with_data_string(printer_ctx,
							       name_str, p_data,
							       data_size);
		} else {
			fwtp_buffered_append_output(printer_ctx, name_str);
		}
		if (!printer_ctx->dont_append_new_line)
			fwtp_buffered_append_output(printer_ctx, "\n");
		fwtp_flush_output_buffer(printer_ctx);
	}

	/*
	 * Post-process the tracepoint, even if it's not printed.
	 * Post-processing may use data from multiple tracepoints and generate
	 * output for a tracepoint that is printed (e.g., for SEM).
	 */
	if (printer_ctx->post_process) {
		data_items->p_next_data_item = data_items->data_buffer;
		printer_ctx->post_process(printer_ctx, entry_timestamp,
					  name_str, data_items);
	}
}

/*
 * Returns the number of printable entries in the tracepoint ring specified by
 * ring starting from the ring head offset specified by head_offset.
 *
 *   ring                   Ring from which to get printable entry count.
 *   head_offset            Ring offset from which to start counting.
 */
static int fwtp_get_printable_entry_count_in_ring(struct tracepoint_ring *ring,
						  uint32_t head_offset)
{
	uint32_t unread_size;
	uint32_t printable_entry_count;

	/* Count the number of printable entries. */
	printable_entry_count = 0;
	unread_size = TRACEPOINT_RING_UNREAD_SIZE(ring, head_offset);
	while (unread_size >= sizeof(uint64_t)) {
		const uint64_t *p_entry_words;
		uint32_t entry_size;
		uint32_t buf_end_size;
		uint32_t max_entry_size;
		uint64_t entry_word;
		uint64_t type;

		/*
		 * The entry size cannot be larger than the remaining size to be read and
		 * the size of the data at the end of the ring buffer.
		 */
		buf_end_size = ring->size -
			       TRACEPOINT_RING_MOD_BUF_SIZE(ring, head_offset);
		if (buf_end_size > unread_size)
			max_entry_size = unread_size;
		else
			max_entry_size = buf_end_size;

		/* Get the entry first word. */
		entry_size = sizeof(uint64_t);
		p_entry_words =
			(uint64_t *)(ring->buffer +
				     TRACEPOINT_RING_MOD_BUF_SIZE(ring,
								  head_offset));
		entry_word = p_entry_words[0];
		type = (entry_word >> FWTP_BASE_ENTRY_TYPE_SHIFT) &
		       FWTP_BASE_ENTRY_TYPE_MASK;

		/* Count printable entries. */
		switch (type) {
		case FWTP_ENTRY_TYPE_BASIC_TRACE:
			++printable_entry_count;
			break;
		case FWTP_ENTRY_TYPE_TRACE_WITH_DATA:
			/* Get the first entry data word. */
			entry_size = 2 * sizeof(uint64_t);
			if (entry_size > max_entry_size)
				break;
			entry_word = p_entry_words[1];

			/* Update and validate the entry size. */
			entry_size =
				sizeof(uint64_t) +
				FWTP_ENTRY_WITH_DATA_WORD_COUNT(entry_word) *
					sizeof(uint64_t);
			if (entry_size > max_entry_size)
				break;

			/* Add another printable entry. */
			++printable_entry_count;

			break;
		default:
			break;
		}

		/* If the entry size was bigger than the maximum size, stop counting. */
		if (entry_size > max_entry_size)
			break;

		/* Advance the head offset past the counted entry. */
		unread_size -= entry_size;
		head_offset = TRACEPOINT_RING_OFFSET_ADD(ring, head_offset,
							 entry_size);
	}

	return printable_entry_count;
}

/*******************************************************************************
 *
 * External services.
 */

/*
 * Prints the tracepoint entries in the ring specified by ring. The printer
 * context is specified by printer_ctx. Updates the head_offset in the ring to
 * point to just after the last printed tracepoint entry.
 *
 * If recent_entry_count is greater than 0, prints only the most recent
 * recent_entry_count entries.
 *
 * The latest absolute timestamp is specified by printer_ctx.absolute_timestamp.
 * If an absolute timestamp entry is contained in entry_words,
 * printer_ctx.absolute_timestamp is updated with the last absolute timestamp
 * entry.
 *
 * The function specified by printer_ctx.get_string is used to get a string
 * corresponding to a string ID.
 *
 * The function specified by printer_ctx.append_output is used to append printed
 * tracepoints to some output.
 *
 *   printer_ctx            Printer context.
 *   ring                   Ring from which to print tracepoints.
 *   recent_entry_count     If > 0, print only the most recent entries.
 */
void fwtp_print_ring_entries(struct fwtp_printer_ctx *printer_ctx,
			     struct tracepoint_ring *ring,
			     int recent_entry_count)
{
	uint64_t absolute_timestamp = printer_ctx->absolute_timestamp;
	struct fwtp_data_item_list *data_items = &(printer_ctx->data_items);
	uint64_t entry_timestamp;
	uint64_t name_id;
	const char *name_str;
	uint32_t buf_size;
	uint32_t unread_size;
	uint32_t head_offset;
	uint32_t tail_offset;
	int skip_count;
	uint8_t *p_data;

	/* Get the ring info. */
	buf_size = ring->size;
	tail_offset = ring->tail_offset;

	/*
	 * Get the head offset from which to start printing. On overflow, advance the
	 * head offset to the oldest tracepoint.
	 */
	head_offset = printer_ctx->head_offset;
	unread_size = TRACEPOINT_RING_UNREAD_SIZE(ring, head_offset);
	if (unread_size > buf_size) {
		head_offset = TRACEPOINT_RING_OFFSET_ADD(ring, tail_offset,
							 -buf_size);
		unread_size = buf_size;
	}

	/*
	 * If printing only recent entries, determine the number of printable entries
	 * to skip.
	 */
	skip_count = 0;
	if (recent_entry_count > 0) {
		int printable_entry_count =
			fwtp_get_printable_entry_count_in_ring(ring,
							       head_offset);

		if (printable_entry_count > recent_entry_count)
			skip_count = printable_entry_count - recent_entry_count;
	}

	/* Print the entries. */
	while (unread_size >= sizeof(uint64_t)) {
		const uint64_t *p_entry_words;
		uint32_t entry_size;
		uint32_t buf_end_size;
		uint32_t max_entry_size;
		uint64_t entry_word;
		uint64_t type;
		unsigned int data_size;

		/*
		 * The entry size cannot be larger than the remaining size to be read and
		 * the size of the data at the end of the ring buffer.
		 */
		buf_end_size = buf_size -
			       TRACEPOINT_RING_MOD_BUF_SIZE(ring, head_offset);
		if (buf_end_size > unread_size)
			max_entry_size = unread_size;
		else
			max_entry_size = buf_end_size;

		/* Get the first word of the entry. */
		entry_size = sizeof(uint64_t);
		p_entry_words =
			(uint64_t *)(ring->buffer +
				     TRACEPOINT_RING_MOD_BUF_SIZE(ring,
								  head_offset));
		entry_word = p_entry_words[0];
		type = (entry_word >> FWTP_BASE_ENTRY_TYPE_SHIFT) &
		       FWTP_BASE_ENTRY_TYPE_MASK;

		/* Reset the decoded data items for post-processing. */
		data_items->p_next_data_item = data_items->data_buffer;
		data_items->total_data_size = 0;
		data_items->item_count = 0;

		/* Decode the entry info. */
		switch (type) {
		case FWTP_ENTRY_TYPE_BASIC_TRACE:
		case FWTP_ENTRY_TYPE_TRACE_WITH_DATA:
			/* Get the entry name and timestamp. */
			name_id = (entry_word >> FWTP_BASIC_ENTRY_NAME_SHIFT) &
				  FWTP_BASIC_ENTRY_NAME_MASK;
			name_str =
				printer_ctx->get_string(printer_ctx, name_id);
			entry_timestamp =
				absolute_timestamp +
				((entry_word >>
				  FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_SHIFT) &
				 FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_MASK);
			break;
		case FWTP_ENTRY_TYPE_ABSOLUTE_TIMESTAMP:
			/* Update the absolute timestamp. */
			absolute_timestamp =
				(entry_word >>
				 FWTP_ABSOLUTE_TIMESTAMP_ENTRY_SHIFT) &
				FWTP_ABSOLUTE_TIMESTAMP_ENTRY_MASK;
			break;
		default:
			break;
		}

		/* Print and post-process the entry. */
		switch (type) {
		case FWTP_ENTRY_TYPE_BASIC_TRACE:
			fwtp_print_tracepoint(printer_ctx, entry_timestamp,
					      name_str, NULL, 0, data_items,
					      &skip_count);
			break;

		case FWTP_ENTRY_TYPE_TRACE_WITH_DATA:
			/* Get the first entry data word. */
			entry_size = 2 * sizeof(uint64_t);
			if (entry_size > max_entry_size)
				break;
			entry_word = p_entry_words[1];
			data_size = FWTP_ENTRY_WITH_DATA_SIZE(entry_word);

			/* The start of the data is after the 16-bit size field. */
			p_data = ((uint8_t *)&(p_entry_words[1])) +
				 sizeof(uint16_t);

			/* Get the remaining data words. */
			entry_size =
				sizeof(uint64_t) +
				FWTP_ENTRY_WITH_DATA_WORD_COUNT(entry_word) *
					sizeof(uint64_t);
			if (entry_size > max_entry_size)
				break;

			/* Print and post-process the tracepoint. */
			fwtp_print_tracepoint(printer_ctx, entry_timestamp,
					      name_str, p_data, data_size,
					      data_items, &skip_count);

			break;

		default:
			break;
		}

		/* If the entry size was bigger than the maximum size, stop printing. */
		if (entry_size > max_entry_size)
			break;

		/* Advance the head offset past the printed entry. */
		unread_size -= entry_size;
		head_offset = TRACEPOINT_RING_OFFSET_ADD(ring, head_offset,
							 entry_size);
	}

	/* Update the printer context. */
	printer_ctx->head_offset = head_offset;
	printer_ctx->absolute_timestamp = absolute_timestamp;
}

/*
 * Prints the tracepoint entries in the entry word buffer specified by
 * entry_words. The number of entry words in the buffer is specified by
 * num_words. The printer context is specified by printer_ctx.
 *
 * The latest absolute timestamp is specified by printer_ctx.absolute_timestamp.
 * If an absolute timestamp entry is contained in entry_words,
 * printer_ctx.absolute_timestamp is updated with the last absolute timestamp
 * entry.
 *
 * The function specified by printer_ctx.get_string is used to get a string
 * corresponding to a string ID.
 *
 * The function specified by printer_ctx.append_output is used to append printed
 * tracepoints to some output.
 *
 *   printer_ctx            Printer context.
 *   entry_words            Tracepoint entry buffer.
 *   num_words              Number of words to print from buffer.
 */
void fwtp_print_entries(struct fwtp_printer_ctx *printer_ctx,
			const uint64_t *entry_words, int num_words)
{
	struct tracepoint_ring ring;

	/* Do nothing if nothing to print. */
	if (num_words <= 0)
		return;

	/* Set up a ring record for the entries. */
	ring.buffer = (uint8_t *)entry_words;
	ring.size = num_words * sizeof(uint64_t);
	ring.tail_offset = ring.size;

	/* Print all the entries using the ring record. */
	printer_ctx->head_offset = 0;
	fwtp_print_ring_entries(printer_ctx, &ring, 0);
}

/*
 * Gets the next data item from the list of data items specified by data_items.
 * Returns the size of the data item. Each time fwtp_get_next_data_item returns
 * a data item, it advances the next data item in the data item list.
 *
 * If the data item size is less than or equal to max_data_item_size, the data
 * item is returned in p_data_item; otherwise, p_data_item is not modified, and
 * the next data item in the list is not advanced (i.e., calling
 * fwtp_get_next_data_item again will attempt to return the same data item). If
 * the returned data size is greater than max_data_item_size, then p_data_item
 * was not modified.
 *
 * Returns 0 if there are no more data items to read.
 *
 *   data_items             List of data items.
 *   p_data_item            Returned data item.
 *   max_data_item_size     Maximum size of data item to return.
 */
int fwtp_get_next_data_item(struct fwtp_data_item_list *data_items,
			    void *p_data_item, int max_data_item_size)
{
	struct fwtp_data_item *data_item;
	unsigned int data_item_size;
	unsigned int remaining_data_size;

	/*
	 * Get the total size of data remaining to be read. Return 0 if the end of the
	 * data has been reached.
	 */
	if ((unsigned int)(data_items->p_next_data_item -
			   data_items->data_buffer) >=
	    data_items->total_data_size) {
		return 0;
	}
	remaining_data_size =
		data_items->total_data_size -
		(data_items->p_next_data_item - data_items->data_buffer);

	/*
	 * Get the next data item. Return 0 if there's not enough data left for the
	 * entire data item.
	 */
	data_item = (struct fwtp_data_item *)data_items->p_next_data_item;
	data_item_size = sizeof(struct fwtp_data_item) + data_item->data_size;
	if (data_item_size > remaining_data_size)
		return 0;

	/* Return the data item and advance to the next data item in the list. */
	if (data_item->data_size <= max_data_item_size) {
		memcpy(p_data_item, data_item->data, data_item->data_size);
		data_items->p_next_data_item += data_item_size;
	}

	return data_item->data_size;
}
