// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */

/*
 *
 * Planes
 *
 * We want to allow for 2 planes to be configured, one for each pipeline.
 * The planes will have 3 KMS properties:
 *  1) Rotation/Flip
 *  2) Region of Interest
 *  3) Input Framebuffer ID and Fence
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>

#include "g2d_drv.h"
#include "g2d_fb.h"
#include "g2d_plane.h"
#include "g2d_gem.h"
#include "g2d_sc.h"

// Todo(b/390272052) Consider creating a sc9000_info.h header for hw capabilities like this.
static const u32 g2d_plane_formats[] = {
	DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_YVU420,
	DRM_FORMAT_YUV420,   DRM_FORMAT_NV12,	  DRM_FORMAT_NV21,
};

static unsigned char g2d_get_plane_number(struct drm_framebuffer *fb)
{
	const struct drm_format_info *info;

	if (!fb)
		return 0;

	info = drm_format_info(fb->format->format);
	if (!info || info->num_planes > MAX_NUM_PLANES)
		return 0;

	return info->num_planes;
}

static void g2d_plane_destroy_state(struct drm_plane *plane, struct drm_plane_state *plane_state)
{
	struct g2d_plane_state *g2d_plane_state = to_g2d_plane_state(plane_state);

	__drm_atomic_helper_plane_destroy_state(plane_state);

	kfree(g2d_plane_state);
}

static int g2d_plane_helper_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state;
	struct drm_framebuffer *fb;
	struct drm_crtc *crtc;
	struct g2d_plane *g2d_plane = to_g2d_plane(plane);
	int ret = 0;

	plane_state = drm_atomic_get_new_plane_state(state, plane);

	if (!plane_state) {
		dev_err(state->dev->dev, "%s: invalid plane_state, shouldn't happen", __func__);
		return -EINVAL;
	}

	fb = plane_state->fb;
	crtc = plane_state->crtc;
	if (!crtc || !fb) {
		dev_err(state->dev->dev, "%s: null crtc or fb, no need to check", __func__);
		return 0;
	}

	ret = g2d_plane->funcs->check(crtc->dev, g2d_plane, plane_state);

	return ret;
}

static void g2d_plane_helper_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_device *drm = plane->dev;
	struct g2d_plane *g2d_plane = to_g2d_plane(plane);
	struct g2d_device *g2d_device = container_of(drm, struct g2d_device, drm);

	/* TODO(b/279522245) The built in assumption here is that the address of plane 0 is
	 * the beginning of the allocated buffer, this may not hold true for all data formats.
	 * Review when PVRIC is implemented.
	 */
	struct drm_plane_state *plane_state = plane->state;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *obj = fb->obj[0];
	struct g2d_bo *g2d_obj = to_g2d_buffer_object(obj);
	int i;

	for (i = 0; i < g2d_get_plane_number(fb); i++)
		g2d_plane->dma_addr[i] = g2d_obj->dma_addr + fb->offsets[i];

	g2d_plane->funcs->update(g2d_device->sc, g2d_plane, state);
}

static void g2d_plane_helper_atomic_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	dev_info(plane->dev->dev, "g2d_plane_atomic_disable");
}

static struct drm_plane_state *g2d_atomic_helper_plane_duplicate_state(struct drm_plane *plane)
{
	struct g2d_plane_state *g2d_plane_state;

	if (WARN_ON(!plane->state))
		return NULL;

	g2d_plane_state = kzalloc(sizeof(*g2d_plane_state), GFP_KERNEL);
	if (!g2d_plane_state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &g2d_plane_state->base);

	/* Duplicate custom properties & blobs here once they're implemented. */

	return &g2d_plane_state->base;
}

static void g2d_atomic_helper_plane_reset(struct drm_plane *plane)
{
	struct g2d_plane_state *g2d_plane_state;

	if (plane->state) {
		plane->funcs->atomic_destroy_state(plane, plane->state);
		plane->state = NULL;
	}

	g2d_plane_state = kzalloc(sizeof(*g2d_plane_state), GFP_KERNEL);
	if (g2d_plane_state == NULL)
		return;

	__drm_atomic_helper_plane_reset(plane, &g2d_plane_state->base);

	/* Store custom plane properties here */
}

static const struct drm_plane_funcs g2d_drm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = g2d_atomic_helper_plane_reset,
	.atomic_duplicate_state = g2d_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = g2d_plane_destroy_state,
};

static const struct drm_plane_helper_funcs g2d_plane_helper_funcs = {
	.atomic_check = g2d_plane_helper_atomic_check,
	.atomic_update = g2d_plane_helper_atomic_update,
	.atomic_disable = g2d_plane_helper_atomic_disable,
};

struct g2d_plane *g2d_plane_init(struct g2d_device *gdevice, unsigned int possible_crtcs)
{
	struct drm_device *drm = &(gdevice->drm);
	struct g2d_plane *g2d_plane;
	int ret;

	g2d_plane = drmm_universal_plane_alloc(drm, struct g2d_plane, base, possible_crtcs,
					       &g2d_drm_plane_funcs, g2d_plane_formats,
					       ARRAY_SIZE(g2d_plane_formats), NULL,
					       DRM_PLANE_TYPE_PRIMARY, NULL);

	if (IS_ERR(g2d_plane))
		goto end;

	dev_dbg(drm->dev, "%s: alloc success", __func__);
	drm_plane_helper_add(&g2d_plane->base, &g2d_plane_helper_funcs);

	/* Standard Properties */
	ret = drm_plane_create_rotation_property(&g2d_plane->base, DRM_MODE_ROTATE_0,
						 DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK);
	if (ret)
		goto error_cleanup;

	sc_plane_init(g2d_plane);

	return g2d_plane;

error_cleanup:
	drm_plane_cleanup(&g2d_plane->base);
end:
	return NULL;
}
