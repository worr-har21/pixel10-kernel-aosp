/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2025 Google LLC
 */

#ifndef MBFS_BASE_H
#define MBFS_BASE_H

#include <perf/mbfs.h>
#include <linux/device.h>

#include "mbfs_backend.h"

extern struct mbfs_client_backend *mbfs_find_backend(const char *name);

#endif /* MBFS_BASE_H */
