/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint ring services header.
 *
 * This header is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

#ifndef TRACEPOINT_RING_H_
#define TRACEPOINT_RING_H_

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/stddef.h>
#define STATIC_ASSERT static_assert
#else
#include <lib/utils/compiler.h>
#include <lib/utils/size.h>
#include <stdint.h>
#endif

enum tracepoint_ring_version {
	TRACEPOINT_RING_VERSION_INVALID = 0,
	TRACEPOINT_RING_VERSION_LEGACY,
	TRACEPOINT_RING_VERSION_FWTP
};

/*
 * Tracepoint entry ring buffer.
 *
 * New entries are added at the byte offset specified by tail_offset. The next
 * entry to be read (the oldest entry) is located at the byte offset specified
 * by head_offset.
 *
 * When entries are added or read, tail_offset and head_offset are incremented
 * by the entry size. They do not need to wrap back to 0 when they exceed the
 * ring size. If they don't wrap, then overflows can be detected when
 * tail_offset - head_offset is greater than the ring buffer size. For this
 * reason, the offset modulo the ring buffer size must be used to access entries
 * in the ring buffer.
 *
 * Another advantage of not wrapping the offsets is that it allows for multiple
 * head offsets to be used in the future. When tracepoints are added, only the
 * tail offset is needed; the head offset is only needed when the tracepoints
 * are read. The head offset could be moved out of struct tracepoint_ring and
 * into other data structures. This could enable having one head offset for
 * copying tracepoints to a larger DRAM buffer and one head offset for sending
 * tracepoints to the AP.
 *
 * Since the head and tail offsets have a maximum size, they must eventually
 * wrap. The value at which they wrap is the ring size. In order to preserve
 * correct behavior, the ring size must be a multiple of the ring buffer size.
 * If the ring buffer size is a power of 2, then the ring size may be equal to
 * the maximum offset size plus one (e.g., UINT32_MAX + 1). In this case,
 * modulus operations are implicitly performed by unsigned 32-bit operations.
 *
 * If the ring buffer size is not a power of 2, explicit modulus operations must
 * be performed. The ring size is chosen to be N * the ring buffer size where N
 * is some integer and N * the ring buffer size is less than the maximum ring
 * offset size.
 *
 * The ring is shared between different software modules, so it must be packed.
 * Declaring it packed could incur a performance penalty on the compiled code,
 * so a static assertion is used to ensure it's packed. Pointers can be
 * different sizes on different systems, so the buffer field is placed last and
 * is assumed to require 64-bit alignment.
 */
struct tracepoint_ring {
	uint32_t magic;
	uint16_t version;
	uint8_t reserved[2];
	uint32_t timestamp_hz;
	uint32_t size;
	uint32_t head_offset;
	uint32_t tail_offset;
	uint8_t *buffer;
};
STATIC_ASSERT((sizeof_field(struct tracepoint_ring, magic) +
	       sizeof_field(struct tracepoint_ring, version) +
	       sizeof_field(struct tracepoint_ring, reserved) +
	       sizeof_field(struct tracepoint_ring, timestamp_hz) +
	       sizeof_field(struct tracepoint_ring, size) +
	       sizeof_field(struct tracepoint_ring, head_offset) +
	       sizeof_field(struct tracepoint_ring, tail_offset) +
	       sizeof_field(struct tracepoint_ring, buffer)) ==
	      sizeof(struct tracepoint_ring));

#define TRACEPOINT_MAGIC 0x54524350
#define TRACEPOINT_RING_SIZE_MULTIPLE 128

/*
 * Division operations must be avoided on M0 CPUs like CAP. For these CPUs, the
 * ring buffer size must be a power of 2.
 */
#if defined(ARMCM0P_MPU)
#define TRACEPOINT_RING_BUF_SIZE_IS_POWER_OF_2
#endif

#ifdef TRACEPOINT_RING_BUF_SIZE_IS_POWER_OF_2
#define TRACEPOINT_RING_MOD_BUF_SIZE(ring, offset) \
	((offset) & ((ring)->size - 1))
#define TRACEPOINT_RING_SIZE(ring) \
	((1ULL << 32) + (0 * (/*unused*/ ring)->size))
#define TRACEPOINT_RING_MOD_RING_SIZE(ring, offset) \
	((offset) + (0 * (/*unused*/ ring)->size))
#else
#define TRACEPOINT_RING_MOD_BUF_SIZE(ring, offset) ((offset) % (ring)->size)
#define TRACEPOINT_RING_SIZE(ring) \
	(TRACEPOINT_RING_SIZE_MULTIPLE * (ring)->size)
#define TRACEPOINT_RING_MOD_RING_SIZE(ring, offset) \
	((offset) % TRACEPOINT_RING_SIZE(ring))
#endif

#define TRACEPOINT_RING_OFFSET_ADD(ring, offset, add_value) \
	TRACEPOINT_RING_MOD_RING_SIZE(ring, (offset) + (add_value))

#define TRACEPOINT_RING_HEAD_BUF_OFFSET(ring) \
	TRACEPOINT_RING_MOD_BUF_SIZE(ring, (ring)->head_offset)
#define TRACEPOINT_RING_TAIL_BUF_OFFSET(ring) \
	TRACEPOINT_RING_MOD_BUF_SIZE(ring, (ring)->tail_offset)

#define TRACEPOINT_RING_ADVANCE_HEAD(ring, count) \
	((ring)->head_offset =                    \
		 TRACEPOINT_RING_OFFSET_ADD(ring, (ring)->head_offset, count))
#define TRACEPOINT_RING_ADVANCE_TAIL(ring, count) \
	((ring)->tail_offset =                    \
		 TRACEPOINT_RING_OFFSET_ADD(ring, (ring)->tail_offset, count))

#define TRACEPOINT_RING_UNREAD_SIZE(ring, head_offset) \
	TRACEPOINT_RING_MOD_RING_SIZE(ring, (ring)->tail_offset - (head_offset))

#endif /* TRACEPOINT_RING_H_ */
