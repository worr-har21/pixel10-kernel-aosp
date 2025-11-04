// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Chip Info Driver - Translator functions
 *
 * Copyright (c) 2024 Google LLC
 */

/*
 * Steps to add a translator:
 * 1. Implement a translator callback followed the type of `goog_chip_info_translator_cb`.
 * 2. Add a new entry to `translator_list` with name and pointer.
 */
static int goog_chip_info_dvfs_translator(struct goog_chip_info_feature *feature,
					  u64 value, int index, char *buffer, int max_size)
{
	int size;

	if (feature->descriptor == lga_a0_dvfs_descriptors && index == LGA_A0_DVFS_DVFS_REV)
		return 0;

	if (feature->descriptor == lga_b0_dvfs_descriptors && index == LGA_B0_DVFS_DVFS_REV)
		return 0;

	size = snprintf(buffer, max_size, "%llu mV", value * 5 + 300);

	return size;
}

static inline int goog_chip_info_ids_get_decimal_num(int index)
{
	switch (index) {
	case LGA_A0_IDS_IDS_AMB_LOGIC_RT:
	case LGA_A0_IDS_IDS_AUR_LOGIC_RT:
	case LGA_A0_IDS_IDS_CPU0_LOGIC_RT:
	case LGA_A0_IDS_IDS_CPU1_LOGIC_RT:
	case LGA_A0_IDS_IDS_CPU2_LOGIC_RT:
	case LGA_A0_IDS_IDS_GMC_LOGIC_RT:
	case LGA_A0_IDS_IDS_G3D_LOGIC_RT:
	case LGA_A0_IDS_IDS_INF_LOGIC_RT:
	case LGA_A0_IDS_IDS_MM_LOGIC_RT:
	case LGA_A0_IDS_IDS_TPU_LOGIC_RT:
	case LGA_A0_IDS_IDS_TPU_SRAM_RT:
		return 3;
	case LGA_A0_IDS_IDS_AOC_LOGIC_RT:
	case LGA_A0_IDS_IDS_AOC_SRAM_RT:
	case LGA_A0_IDS_IDS_AUR_SRAM_RT:
	case LGA_A0_IDS_IDS_CPU1_SRAM_RT:
	case LGA_A0_IDS_IDS_CPU2_SRAM_RT:
	case LGA_A0_IDS_IDS_CPU0_SRAM_RT:
	case LGA_A0_IDS_IDS_HSION_LOGIC_RT:
	case LGA_A0_IDS_IDS_HSIOS_LOGIC_RT:
	case LGA_A0_IDS_IDS_SLC_SRAM_RT:
		return 4;
	case LGA_A0_IDS_IDS_G3D_SRAM_RT:
	case LGA_A0_IDS_IDS_AMB_SRAM_RT:
	default:
		return 0;
	}
}

static int goog_chip_info_ids_translator(struct goog_chip_info_feature *feature,
					 u64 value, int index, char *buffer, int max_size)
{
	int size;
	int integer, decimal;
	int nr_dec_bits = goog_chip_info_ids_get_decimal_num(index);

	if (index == LGA_A0_IDS_IDS_REV_CTRL_OTP)
		return 0;

	integer = value >> nr_dec_bits;
	decimal = value & (BIT(nr_dec_bits) - 1);
	decimal = (10000 * decimal) >> nr_dec_bits;

	size = snprintf(buffer, max_size, "%d.%04d mA", integer, decimal);

	return size;
}

static int goog_chip_info_asic_id_translator(struct goog_chip_info_feature *feature,
					     u64 value, int index, char *buffer, int max_size)
{
	const char *chip_name;

	if (value == 0x500)
		chip_name = "LGA A0";
	else if (value == 0x510)
		chip_name = "LGA B0";

	return snprintf(buffer, max_size, "%s", chip_name);
}

/*
 * list of translator
 */
struct translator translator_list[] = {
	{"dvfs-translator", goog_chip_info_dvfs_translator},
	{"ids-translator", goog_chip_info_ids_translator},
	{"asic-id-translator", goog_chip_info_asic_id_translator},
	{},
};
