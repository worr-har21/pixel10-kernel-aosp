/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint entry definitions header.
 *
 * This header is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

#ifndef FWTP_ENTRY_H_
#define FWTP_ENTRY_H_

/* Define base entry fields. */
#define FWTP_BASE_ENTRY_TYPE_SHIFT 58
#define FWTP_BASE_ENTRY_TYPE_MASK ((1ULL << 6) - 1ULL)
#define FWTP_ENTRY_TYPE(entry_word)                     \
	(((entry_word) >> FWTP_BASE_ENTRY_TYPE_SHIFT) & \
	 FWTP_BASE_ENTRY_TYPE_MASK)

/* Define tracepoint entry types. */
#define FWTP_ENTRY_TYPE_EMPTY 0ULL
#define FWTP_ENTRY_TYPE_BASIC_TRACE 1ULL
#define FWTP_ENTRY_TYPE_ABSOLUTE_TIMESTAMP 2ULL
#define FWTP_ENTRY_TYPE_TRACE_WITH_DATA 3ULL

/* Define basic entry fields. */
#define FWTP_BASIC_ENTRY_NAME_SHIFT 32
#define FWTP_BASIC_ENTRY_NAME_MASK ((1ULL << 24) - 1ULL)
#define FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_SHIFT 0
#define FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_MASK ((1ULL << 32) - 1ULL)
#define FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_MAX \
	FWTP_BASIC_ENTRY_TIMESTAMP_DELTA_MASK

/* Define absolute timestamp entry fields. */
#define FWTP_ABSOLUTE_TIMESTAMP_ENTRY_SHIFT 0
#define FWTP_ABSOLUTE_TIMESTAMP_ENTRY_MASK ((1ULL << 56) - 1ULL)

/* Define trace with data fields. */
#define FWTP_ENTRY_WITH_DATA_SIZE_SHIFT 0
#define FWTP_ENTRY_WITH_DATA_SIZE_MASK ((1ULL << 16) - 1ULL)
#define FWTP_ENTRY_WITH_DATA_SIZE(data_word)                \
	(((data_word) >> FWTP_ENTRY_WITH_DATA_SIZE_SHIFT) & \
	 FWTP_ENTRY_WITH_DATA_SIZE_MASK)
/* Word count of the data, including the size field. */
#define FWTP_ENTRY_WITH_DATA_WORD_COUNT_FROM_SIZE(size) \
	((sizeof(uint16_t) + (size) + sizeof(uint64_t) - 1) / sizeof(uint64_t))
#define FWTP_ENTRY_WITH_DATA_WORD_COUNT(data_word) \
	FWTP_ENTRY_WITH_DATA_WORD_COUNT_FROM_SIZE( \
		FWTP_ENTRY_WITH_DATA_SIZE(data_word))

/*
 * Returns an absolute timestamp entry with the current timestamp specified by
 * current_timestamp.
 *
 *   current_timestamp      Current timestamp.
 */
#define FWTP_ABSOLUTE_TIMESTAMP_ENTRY(current_timestamp)                      \
	((FWTP_ENTRY_TYPE_ABSOLUTE_TIMESTAMP << FWTP_BASE_ENTRY_TYPE_SHIFT) | \
	 ((current_timestamp) & FWTP_ABSOLUTE_TIMESTAMP_ENTRY_MASK)           \
		 << FWTP_ABSOLUTE_TIMESTAMP_ENTRY_SHIFT)

#endif /* FWTP_ENTRY_H_ */
