/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Google LLC.
 *
 * Author: Ruofei Ma <ruofeim@google.com>
 */

#ifndef _VPU_COMMON_H_
#define _VPU_COMMON_H_

#include "vpu_priv.h"

struct vpu_intr_queue *vpu_get_intr_queue(struct vpu_core *core, uint32_t inst_idx);
void vpu_update_system_state(struct vpu_core *core);

#endif
