/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */

#ifndef G2D_PLANE_HW_H_
#define G2D_PLANE_HW_H_

#define FRAC_16_16(mult, div) (((mult) << 16) / (div))

#define G2D_MIN_SCALE (FRAC_16_16(1, 4))
#define G2D_MAX_SCALE (FRAC_16_16(8, 1))

struct sc_hw;
void plane_commit(struct sc_hw *hw, u8 layer_id);

#endif //G2D_PLANE_HW_H_
