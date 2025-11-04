/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */

#ifndef _G2D_WRITEBACK_HW_H_
#define _G2D_WRITEBACK_HW_H_

#include "g2d_sc.h"

void wb_set_fb(struct sc_hw *hw, u8 hw_id, struct sc_hw_fb *fb);

#endif /* _G2D_WRITEBACK_HW_H_ */
