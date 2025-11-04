// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>

#include <asm/arch_timer.h>
#include <clocksource/arm_arch_timer.h>

#include <soc/google/google_gtc.h>

static u64 gtc_freq_hz __ro_after_init;

u64 goog_gtc_get_counter(void)
{
	return arch_timer_read_counter();
}
EXPORT_SYMBOL_GPL(goog_gtc_get_counter);

u64 goog_gtc_ticks_to_ns(u64 gtc_tick)
{
	u64 sec, sub_tick, nsec;

	/*
	 * To avoid u64 overflow in calculation of ns, let's
	 * calculate the seconds part and nanoseconds part separately.
	 * In math:
	 *   seconds part     = (tick - (tick % freq)) / freq
	 *   nanoseconds part = ((tick % freq) / freq) * NSEC_PER_SEC
	 * In C code, after considering the rounding of integer division:
	 *   seconds part     = tick / freq
	 *   nanoseconds part = ((tick % freq) * NSEC_PER_SEC) / freq
	 * Then both parts won't cause u64 overflow during calculation.
	 * The final result = (sec * NSEC_PER_SEC) + nsec. It only overflows
	 * after (2^64 / 10^9) seconds ~= 584 years which is long enough, and
	 * is longer than the overflow threshold of hardware GTC counter.
	 */
	sec = gtc_tick / gtc_freq_hz;
	sub_tick = gtc_tick - (sec * gtc_freq_hz); /* gtc_tick % gtc_freq_hz */
	nsec = (sub_tick * NSEC_PER_SEC) / gtc_freq_hz;

	return (sec * NSEC_PER_SEC) + nsec;
}
EXPORT_SYMBOL_GPL(goog_gtc_ticks_to_ns);

u64 goog_gtc_get_time_ns(void)
{
	return goog_gtc_ticks_to_ns(goog_gtc_get_counter());
}
EXPORT_SYMBOL_GPL(goog_gtc_get_time_ns);

static int __init gtc_init(void)
{
	/*
	 * Since the ARM arch timer has the same frequency as GTC,
	 * the API using the ARM arch timer frequency as well.
	 */
	gtc_freq_hz = arch_timer_get_cntfrq();

	return 0;
}

static void __exit gtc_exit(void)
{
}

module_init(gtc_init);
module_exit(gtc_exit);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google global time counter driver");
MODULE_LICENSE("GPL");
