/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */

#ifndef _G2D_DRV_H_
#define _G2D_DRV_H_

#include <drm/drm.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

struct g2d_sc;
struct g2d_device {
	struct drm_device drm;
	struct g2d_sc *sc;
};

#endif /* _G2D_DRV_H_ */
