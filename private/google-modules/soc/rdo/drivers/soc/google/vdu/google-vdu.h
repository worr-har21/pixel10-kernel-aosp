/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Google LLC.
 *
 */

#ifndef GOOGLE_VDU_H
#define GOOGLE_VDU_H

struct gvdu_base {
	struct gdmc_iface *gdmc_iface;

	char *grant_buffer;
	ssize_t grant_length;

	char *delegate_buffer;
	ssize_t delegate_length;
};

#endif /* GOOGLE_VDU_H */
