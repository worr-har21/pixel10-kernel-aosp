// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "g2d_drv.h"
#include "g2d_crtc.h"
#include "g2d_sc.h"

/*
 *
 * CRTC
 *
 * We want to allow for 2 CRTCs to be configured, one for each pipeline.
 * In this design CRTCs are passthroughs.
 */

static void g2d_crtc_atomic_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
}

static int g2d_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	return 0;
}

static void g2d_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *old_crtc_state)
{
	struct g2d_crtc *g2d_crtc = to_g2d_crtc(crtc);

	if (!g2d_crtc->dev) {
		pr_err("%s: invalid dev", __func__);
		return;
	}
	g2d_crtc->funcs->commit(g2d_crtc->dev, crtc);
}

static void g2d_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct g2d_crtc *g2d_crtc = to_g2d_crtc(crtc);

	if (!g2d_crtc->dev) {
		pr_err("%s: invalid dev", __func__);
		return;
	}

	g2d_crtc->funcs->enable(g2d_crtc->dev, crtc);
}

static void g2d_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct g2d_crtc *g2d_crtc = to_g2d_crtc(crtc);

	if (!g2d_crtc->dev) {
		pr_err("%s: invalid dev", __func__);
		return;
	}

	g2d_crtc->funcs->disable(g2d_crtc->dev, crtc);
}

static const struct drm_crtc_helper_funcs g2d_crtc_helper_funcs = {
	.atomic_check = g2d_crtc_atomic_check,
	.atomic_flush = g2d_crtc_atomic_flush,
	.atomic_enable = g2d_crtc_atomic_enable,
	.atomic_disable = g2d_crtc_atomic_disable,
};

static const struct drm_crtc_funcs g2d_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = g2d_crtc_atomic_destroy_state,
};

int g2d_crtc_init(struct g2d_device *gdevice)
{
	struct drm_device *drm = &(gdevice->drm);
	int i;
	struct g2d_crtc *g2d_crtc;

	for (i = 0; i < NUM_PIPELINES; i++) {
		g2d_crtc = drmm_crtc_alloc_with_planes(drm, struct g2d_crtc, base, NULL, NULL,
						       &g2d_crtc_funcs, "g2d_core%d", i);

		if (IS_ERR(g2d_crtc))
			return PTR_ERR(g2d_crtc);

		gdevice->sc->crtc[i] = g2d_crtc;

		g2d_crtc->dev = drm->dev;

		drm_crtc_helper_add(&g2d_crtc->base, &g2d_crtc_helper_funcs);

		sc_crtc_init(g2d_crtc);
	}

	return 0;
}
