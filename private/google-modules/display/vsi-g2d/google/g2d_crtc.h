/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */
#ifndef _G2D_CRTC_H_
#define _G2D_CRTC_H_

#include <drm/drm.h>

#define to_g2d_crtc(crtc) container_of(crtc, struct g2d_crtc, base)

struct g2d_crtc {
	struct drm_crtc base;
	u8 id;
	struct device *dev;
	unsigned int max_bpc;
	unsigned int color_formats; /* supported color format */

	const struct g2d_crtc_funcs *funcs;
};

struct g2d_crtc_funcs {
	void (*commit)(struct device *dev, struct drm_crtc *crtc);
	void (*enable)(struct device *dev, struct drm_crtc *crtc);
	void (*disable)(struct device *dev, struct drm_crtc *crtc);
};

int g2d_crtc_init(struct g2d_device *gdevice);

#endif // _G2D_CRTC_H_
