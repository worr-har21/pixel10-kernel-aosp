/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023-2024 Google LLC
 */

#ifndef _GOOGLE_TIMESTAMP_SYNC_H
#define _GOOGLE_TIMESTAMP_SYNC_H

#if IS_ENABLED(CONFIG_GOOGLE_TIMESTAMP_SYNC)

/**
 * goog_gtc_ticks_to_boottime - Convert GTC tick value to CLOCK_BOOTTIME in nanoseconds
 *
 * @gtc_tick: the Global Timestamp Counter (GTC) counter/tick value to be converted.
 *
 * This API takes a GTC tick value and converts it into the equivalent
 * CLOCK_BOOTTIME in nanoseconds. It can be used to correlate the GTC time
 * with CLOCK_BOOTTIME.
 *
 * Warning:
 * Successive calls to this API may observe backward jumps in the return value
 * due to the dynamic clock drift between CLOCK_BOOTTIME and GTC.
 *
 * Unlike the GTC time, which is calculated by dividing a fixed frequency,
 * kernel CLOCK_BOOTTIME is calculated by bit-shifting based on a dynamic
 * adjusted frequency to synchronize with the external internet clock. This
 * results in a dynamic clock drift between CLOCK_BOOTTIME and GTC. So, the
 * GTC tick and CLOCK_BOOTTIME snapshot is updated to adjust the
 * GTC-to-CLOCK_BOOTTIME conversion for the clock drift.
 *
 * If the CLOCK_BOOTTIME is slower than GTC, the clock drift can make the new
 * snapshot boot time smaller than the expected based on the old snapshot. It
 * leads to subsequent GTC-to-boot time conversions yielding smaller values than
 * the expected based on the old snapshot as well. As a result, a backward time
 * jump can be observed from successive calls, if the change of the input GTC
 * ticks since the last call before the snapshot update is less than the time
 * lost in the snapshot update due to the clock drift.
 *
 * To guarantee the monotonicity from the user's perspective, the user side is
 * suggested to store the last returned value, compare the return value against
 * the stored last value, and only accept the return value if it's larger.
 *
 * Return: The equivalent CLOCK_BOOTTIME in nanoseconds.
 */
u64 goog_gtc_ticks_to_boottime(u64 gtc_tick);

#else /* IS_ENABLED(CONFIG_GOOGLE_TIMESTAMP_SYNC) */

static u64 goog_gtc_ticks_to_boottime(u64 gtc_tick)
{
	return 0;
}

#endif /* IS_ENABLED(CONFIG_GOOGLE_TIMESTAMP_SYNC) */

#endif /* _GOOGLE_TIMESTAMP_SYNC_H */
