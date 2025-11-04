/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024 Google LLC */

#ifndef _GOOGLE_CAP_SYSFS_H
#define _GOOGLE_CAP_SYSFS_H

#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/build_bug.h>
#include <linux/types.h>

#define READ_TIME 5
#define MAX_DVFS_LEVELS 24
#define GTC_TICKS_PER_MS 38400

/*
 * Following struct must match the ones in
 * "interfaces/protocols/cap/include/interfaces/protocols/cap/statsbuf.h"
 */

struct cap_statsbuf_power_state_stats {
	u64 last_entry_start_ts;
	u64 last_entry_end_ts;
	u64 last_exit_start_ts;
	u64 last_exit_end_ts;
	u64 last_entry_sched_ts;
	u64 last_exit_sched_ts;
	u64 state_entry_count;
	u64 total_time_in_state_ticks;
} __packed __aligned(8);
static_assert(sizeof(struct cap_statsbuf_power_state_stats) == 64);

struct cap_statsbuf_dvfs_level_stats {
	u64 entry_count;
	u64 total_residency_ticks;
	u64 last_entry_ts;
} __packed __aligned(8);
static_assert(sizeof(struct cap_statsbuf_dvfs_level_stats) == 24);

struct cap_statsbuf_dvfs_stats {
	struct cap_statsbuf_dvfs_level_stats domain_level_stats[MAX_DVFS_LEVELS];
	u8 domain_current_level;
	u8 domain_target_level;
} __packed __aligned(8);
static_assert(sizeof(struct cap_statsbuf_dvfs_stats) == 584);

enum aging_clc_status {
	NO_MEASURE,
	CLC_DISABLED,
	SENSOR_READ_FAILURE,
	CLUSTER_NOT_READY,
	SENSOR_READ_OUT_OF_RANGE,
	MEASUREMENT_EXPIRED,
	SUCCESS,
} __packed;

struct cap_statsbuf_clc_aging_stats {
	u32 last_measurement_ts;
	u32 last_aging_sensor_measurement;
	u8 last_adjustment_pmic_step;
	u8 error_count;
	enum aging_clc_status last_status_code;
} __packed __aligned(8);
static_assert(sizeof(struct cap_statsbuf_clc_aging_stats) == 16);

struct stats_region {
	struct list_head list;
	void *__iomem start_addr;
	u32 number;
	u32 *ids;
	const char **names;
	void *read_first;
	void *read_second;
};

struct dvfs_attribute {
	struct list_head list;
	struct stats_region *dvfs_stats_region;
	struct device_attribute *dev_attr;
	int id;
};

struct aging_clc_attribute {
	struct list_head list;
	struct stats_region *aging_clc_region;
	struct kobject *kobj;
	int id;
};

struct timestamps_buffer {
	u64 last_entry_start_ts;
	u64 last_entry_end_ts;
	u64 last_exit_start_ts;
	u64 last_exit_end_ts;
	u64 last_entry_sched_ts;
	u64 last_exit_sched_ts;
} __packed __aligned(8);
static_assert(sizeof(struct timestamps_buffer) == 48);

int read_cpu_pd_latency_stats(u32 power_state,
			struct timestamps_buffer *ts_buff);

#endif // _GOOGLE_CAP_SYSFS_H
