/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Google LLC
 */

#ifndef _GOOG_OTP_MAP_H_
#define _GOOG_OTP_MAP_H_

#define LCS_MAJOR_HTEST 0x0000
#define LCS_MAJOR_OPEN 0x0005
#define LCS_MAJOR_DEV 0x00A5
#define LCS_MAJOR_PROD 0x0055

/* this value indicate the max number of fields to be fetched and combined */
#define MAX_REG_INFO 2

struct goog_chip_info_feature;

struct reg_info {
	u32 offset;
	u8 first_bit;
	u8 last_bit;
	u8 shift;
};
struct goog_chip_info_descriptor {
	char *name;
	struct reg_info reg_infos[MAX_REG_INFO];
	bool is_big_endian;
	u64 value;
};

typedef int (*goog_chip_info_translator_cb_t)(struct goog_chip_info_feature *feature,
					      u64 value, int index, char *buffer, int max_size);

struct translator {
	char *name;
	goog_chip_info_translator_cb_t callback;
};

struct goog_chip_info_feature {
	struct device *dev;
	struct regmap *base;
	const char *name;
	struct goog_chip_info_descriptor *descriptor;
	goog_chip_info_translator_cb_t translator_cb;
	int nr_descriptor_entries;
	struct device_attribute dev_attr;
	bool is_serial_codes;
	bool is_device_table;
	bool is_lcs_state;
	bool is_visible_in_prod;
};
#endif /* _GOOG_OTP_MAP__H_ */
