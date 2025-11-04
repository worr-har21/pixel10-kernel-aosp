// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/dma-fence.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include <trace/dpu_trace.h>

#include "vs_crtc.h"
#include "vs_dc.h"
#include "vs_dc_drm_property.h"
#include "vs_dc_hw.h"
#include "vs_dc_post.h"
#include "vs_trace.h"

#define FRAME_TIMEOUT msecs_to_jiffies(100)
#define FRAME_DEFAULT_FPS 60

#define VBLANK_KTHREAD_SCHED_PRIORITY 19

static const struct drm_prop_enum_list vs_power_off_mode_enum_list[VS_POWER_OFF_MODE_COUNT] = {
	{ VS_POWER_OFF_MODE_FULL, "FULL" },
	{ VS_POWER_OFF_MODE_PSR, "PSR" }
};

static inline unsigned long fps_timeout(u32 fps)
{
	const long frame_time_ms = DIV_ROUND_UP(MSEC_PER_SEC, fps ?: FRAME_DEFAULT_FPS);

	return msecs_to_jiffies(frame_time_ms) + FRAME_TIMEOUT;
}

bool vs_display_get_crtc_scanoutpos(struct drm_device *drm_dev, unsigned int crtc_id,
				    bool in_vblank_irq, int *vpos, int *hpos, ktime_t *stime,
				    ktime_t *etime, const struct drm_display_mode *mode)
{
	struct drm_crtc *crtc = drm_crtc_from_index(drm_dev, crtc_id);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	u32 position;
	int vblank_lines;
	bool ret = false;

	/*
	 * While in vblank, position will be negative counting up
	 * towards 0 at vbl_end. And outside vblank, position will
	 * be positive counting up since vbl_end.
	 */
	if (!in_vblank_irq) {
		/* Get optional system timestamp before query. */
		if (stime)
			*stime = ktime_get();

		if (!vs_crtc->funcs->get_crtc_scanout_position) {
			/*
			 * Return a vpos of zero, which will cause calling code
			 * to just return the etime timestamp uncorrected.
			 * At least this is no worse than the standard fallback.
			 */
			DRM_DEV_ERROR(dev, "get_crtc_scanout_position() isn't implemented!\n");
			*hpos = *vpos = 0;
		} else {
			ret = vs_crtc->funcs->get_crtc_scanout_position(dev, crtc, &position);
			if (ret != 0)
				return false;

			/* Decode into vertical and horizontal scanout position. */
			*hpos = position & 0xffff;
			*vpos = (position >> 16) & 0xffff;
		}

		/* Get optional system timestamp after query. */
		if (etime)
			*etime = ktime_get();
	} else {
		vblank_lines = mode->vtotal - mode->vdisplay;
		/*
		 * Assume the irq handler got called close to first
		 * line of vblank, so HW has about a full vblank
		 * scanlines to go, and as a base timestamp use the
		 * one taken at entry into vblank irq handler, so it
		 * is not affected by random delays due to lock
		 * contention on event_lock or vblank_time lock in
		 * the core.
		 */
		*hpos = 0;
		*vpos = -vblank_lines;

		if (stime)
			*stime = vs_crtc->t_vblank;
		if (etime)
			*etime = vs_crtc->t_vblank;
	}

	return true;
}

static void vs_crtc_atomic_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(state);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	/* destroy standard properties*/
	__drm_atomic_helper_crtc_destroy_state(state);

	/* destroy custom properties */
	drm_property_blob_put(vs_crtc_state->prior_gamma.blob);
	drm_property_blob_put(vs_crtc_state->roi0_gamma.blob);
	drm_property_blob_put(vs_crtc_state->roi1_gamma.blob);

	/* histogram */
	for (int i = VS_HIST_CHAN_IDX_0; i < VS_HIST_CHAN_IDX_COUNT; i++)
		drm_property_blob_put(vs_crtc_state->hist_chan[i].blob);

	if (vs_crtc_state->blur_mask)
		drm_framebuffer_put(vs_crtc_state->blur_mask);

	if (vs_crtc_state->brightness_mask)
		drm_framebuffer_put(vs_crtc_state->brightness_mask);

	vs_dc_destroy_drm_properties(vs_crtc_state->drm_states, &vs_crtc->properties);

	kfree(vs_crtc_state);
}

void vs_crtc_destroy(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	vs_qos_remove_fabrt_devfreq_request(crtc);

	vs_qos_remove_devfreq_request(crtc);

	if (crtc->port)
		of_node_put(crtc->port);

	/* cleanup crtc variables */
	drm_crtc_cleanup(crtc);

	kthread_destroy_worker(vs_crtc->vblank_worker);
	kthread_destroy_worker(vs_crtc->commit_worker);

	/* release: histogram gem objects */
	for (int i = VS_HIST_CHAN_IDX_0; i < VS_HIST_CHAN_IDX_COUNT; i++)
		vs_gem_pool_deinit(&vs_crtc->hist_chan_gem_pool[i]);

	wakeup_source_unregister(vs_crtc->ws);

	kfree(vs_crtc);
}

static void vs_crtc_reset(struct drm_crtc *crtc)
{
	struct vs_crtc_state *state;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	u32 i;

	if (crtc->state) {
		vs_crtc_atomic_destroy_state(crtc, crtc->state);
		crtc->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;

	/* reset standard properties */
	__drm_atomic_helper_crtc_reset(crtc, &state->base);

	/* reset custom properties */
	state->sync_mode = VS_SINGLE_DC;
	state->output_fmt = MEDIA_BUS_FMT_RGB888_1X24;
	state->encoder_type = DRM_MODE_ENCODER_NONE;
	state->seamless_mode_change = false;

	for (i = 0; i < vs_crtc->properties.num; i++) {
		state->drm_states[i].proto = vs_crtc->properties.items[i].proto;
		state->drm_states[i].is_changed = true;
	}

	/* reset internal state variables */
	atomic_set(&vs_crtc->frames_pending, 0);
	DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending), vs_crtc->trace_pid,
			       "frames_pending[%u]", crtc->index);
	vs_crtc->frame_transfer_pending = false;
	atomic_set(&vs_crtc->hw_reset_count, 0);
	vs_crtc->trace_pid = 0;
	vs_crtc->ltm_hist_query_pending = false;
}

static void _vs_crtc_duplicate_blob_state(struct vs_drm_blob_state *state)
{
	if (state->blob)
		drm_property_blob_get(state->blob);

	state->changed = false;
}

static void _vs_crtc_duplicate_blob(struct vs_crtc_state *state)
{
	_vs_crtc_duplicate_blob_state(&state->prior_gamma);
	_vs_crtc_duplicate_blob_state(&state->roi0_gamma);
	_vs_crtc_duplicate_blob_state(&state->roi1_gamma);

	/* histogram */
	for (int i = VS_HIST_CHAN_IDX_0; i < VS_HIST_CHAN_IDX_COUNT; i++)
		_vs_crtc_duplicate_blob_state(&state->hist_chan[i]);

	if (state->blur_mask)
		drm_framebuffer_get(state->blur_mask);
	if (state->brightness_mask)
		drm_framebuffer_get(state->brightness_mask);
}

static int _vs_crtc_set_property_blob_from_id(struct drm_device *dev,
					      struct vs_drm_blob_state  *state, uint64_t blob_id,
					      size_t expected_size)
{
	struct drm_property_blob *new_blob = NULL;
	bool data_changed;

	if (blob_id) {
		new_blob = drm_property_lookup_blob(dev, blob_id);
		if (!new_blob)
			return -EINVAL;

		if (new_blob->length != expected_size) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
	}

	/* compare the ori blob data with the new bolb data wether to changed. */
	if (state->blob && blob_id) {
		if (memcmp(new_blob->data, state->blob->data, expected_size) == 0) {
			drm_property_blob_put(new_blob);
			state->changed = false;
			return 0;
		}
	}

	data_changed = drm_property_replace_blob(&state->blob, new_blob);
	state->changed = data_changed;

	drm_property_blob_put(new_blob);

	return 0;
}

static void _vs_crtc_atomic_duplicate_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *state)
{
	u32 ori_active = crtc->state->active;
	u32 ori_self_refresh_active = crtc->state->self_refresh_active;

	__drm_atomic_helper_crtc_duplicate_state(crtc, state);

	/* restore active & self_refresh_active due to these property will be controlled by user */
	state->active = ori_active;
	state->self_refresh_active = ori_self_refresh_active;
}

static int _vs_crtc_set_fb_from_id(struct drm_device *dev, struct drm_framebuffer **fb,
				uint64_t val)
{
	const uint32_t fb_id = lower_32_bits(val);

	if (unlikely(!fb))
		return -EINVAL;

	if (*fb)
		drm_framebuffer_put(*fb);
	*fb = drm_framebuffer_lookup(dev, NULL, fb_id);

	return 0;
}

static struct drm_crtc_state *vs_crtc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct vs_crtc_state *ori_state;
	struct vs_crtc_state *state;
	const struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	if (WARN_ON(!crtc->state))
		return NULL;

	ori_state = to_vs_crtc_state(crtc->state);

	state = kmemdup(ori_state, sizeof(*ori_state), GFP_KERNEL);
	if (!state)
		return NULL;

	_vs_crtc_duplicate_blob(state);

	_vs_crtc_atomic_duplicate_state(crtc, &state->base);

	state->bld_size_changed = false;

	state->seamless_mode_change = false;

	state->skip_update = true;

	state->force_skip_update = false;

	state->planes_updated = false;

	state->wb_connectors_updated = false;

	state->need_boost_fabrt = false;

	/* dc properties */
	vs_dc_duplicate_drm_properties(state->drm_states, ori_state->drm_states,
				       &vs_crtc->properties);

	return &state->base;
}

static int vs_crtc_atomic_set_property(struct drm_crtc *crtc, struct drm_crtc_state *state,
				       struct drm_property *property, uint64_t val)
{
	struct drm_device *dev = crtc->dev;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(state);
	struct vs_dc *vs_dc = dev_get_drvdata(vs_crtc->dev);
	int ret = 0;

	if (property == vs_crtc->sync_mode_prop) {
		vs_crtc_state->sync_mode = val;
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->panel_sync_prop) {
		vs_crtc_state->sync_enable = val;
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->bld_size_prop) {
		vs_crtc_state->bld_size = val;
		vs_crtc_state->bld_size_changed = true;
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->core_clk) {
		vs_crtc_state->requested_qos_config.core_clk = val;
	} else if (property == vs_crtc->rd_avg_bw_mbps) {
		vs_crtc_state->requested_qos_config.rd_avg_bw_mbps = val;
	} else if (property == vs_crtc->rd_peak_bw_mbps) {
		vs_crtc_state->requested_qos_config.rd_peak_bw_mbps = val;
	} else if (property == vs_crtc->rd_rt_bw_mbps) {
		vs_crtc_state->requested_qos_config.rd_rt_bw_mbps = val;
	} else if (property == vs_crtc->wr_avg_bw_mbps) {
		vs_crtc_state->requested_qos_config.wr_avg_bw_mbps = val;
	} else if (property == vs_crtc->wr_peak_bw_mbps) {
		vs_crtc_state->requested_qos_config.wr_peak_bw_mbps = val;
	} else if (property == vs_crtc->wr_rt_bw_mbps) {
		vs_crtc_state->requested_qos_config.wr_rt_bw_mbps = val;
	} else if (property == vs_crtc->power_off_mode) {
		vs_crtc_state->power_off_mode = val;
	} else if (property == vs_crtc->prior_gamma_prop) {
		ret = _vs_crtc_set_property_blob_from_id(dev, &vs_crtc_state->prior_gamma, val,
							 sizeof(struct drm_vs_gamma_lut));
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->roi0_gamma_prop) {
		ret = _vs_crtc_set_property_blob_from_id(dev, &vs_crtc_state->roi0_gamma, val,
							 sizeof(struct drm_vs_gamma_lut));
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->roi1_gamma_prop) {
		ret = _vs_crtc_set_property_blob_from_id(dev, &vs_crtc_state->roi1_gamma, val,
							 sizeof(struct drm_vs_gamma_lut));
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->blur_mask_prop) {
		ret = _vs_crtc_set_fb_from_id(dev, &vs_crtc_state->blur_mask, val);
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->brightness_mask_prop) {
		ret = _vs_crtc_set_fb_from_id(dev, &vs_crtc_state->brightness_mask, val);
		vs_crtc_state->skip_update = false;
	} else if (property == vs_crtc->expected_present_time) {
		vs_crtc_state->expected_present_time = val;
		vs_crtc_state->skip_update = false;
	} else {
		/* histogram property */
		for (int i = 0; i < VS_HIST_CHAN_IDX_COUNT; i++) {
			if (property != vs_crtc->hist_chan_prop[i])
				continue;

			ret = _vs_crtc_set_property_blob_from_id(dev, &vs_crtc_state->hist_chan[i],
								 val,
								 sizeof(struct drm_vs_hist_chan));
			return ret;
		}

		/* dc property */
		ret = vs_dc_set_drm_property(dev, vs_crtc_state->drm_states, &vs_crtc->properties,
					     property, val);

		if (vs_dc->boost_fabrt_freq && strcmp(property->name, "PRIOR_3DLUT") == 0)
			vs_crtc_state->need_boost_fabrt = true;

		vs_crtc_state->skip_update = false;
	}

	drm_dbg_state(dev, "[CRTC:%u:%s]: SET custom property %s value %llu\n", crtc->base.id,
		      crtc->name, property->name, val);
	trace_disp_set_property(crtc->name, property->name, val);

	return ret;
}

#define GET_BLOB_ID(blob) (blob) ? blob->base.id : 0

static int vs_crtc_atomic_get_property(struct drm_crtc *crtc, const struct drm_crtc_state *state,
				       struct drm_property *property, uint64_t *val)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	const struct vs_crtc_state *vs_crtc_state =
		container_of(state, const struct vs_crtc_state, base);

	if (property == vs_crtc->sync_mode_prop)
		*val = vs_crtc_state->sync_mode;
	else if (property == vs_crtc->panel_sync_prop)
		*val = vs_crtc_state->sync_enable;
	else if (property == vs_crtc->bld_size_prop)
		*val = vs_crtc_state->bld_size;
	else if (property == vs_crtc->core_clk)
		*val = vs_crtc_state->qos_config.core_clk;
	else if (property == vs_crtc->rd_avg_bw_mbps)
		*val = vs_crtc_state->qos_config.rd_avg_bw_mbps;
	else if (property == vs_crtc->rd_peak_bw_mbps)
		*val = vs_crtc_state->qos_config.rd_peak_bw_mbps;
	else if (property == vs_crtc->rd_rt_bw_mbps)
		*val = vs_crtc_state->qos_config.rd_rt_bw_mbps;
	else if (property == vs_crtc->wr_avg_bw_mbps)
		*val = vs_crtc_state->qos_config.wr_avg_bw_mbps;
	else if (property == vs_crtc->wr_peak_bw_mbps)
		*val = vs_crtc_state->qos_config.wr_peak_bw_mbps;
	else if (property == vs_crtc->wr_rt_bw_mbps)
		*val = vs_crtc_state->qos_config.wr_rt_bw_mbps;
	else if (property == vs_crtc->power_off_mode)
		*val = vs_crtc_state->power_off_mode;
	else if (property == vs_crtc->prior_gamma_prop)
		*val = GET_BLOB_ID(vs_crtc_state->prior_gamma.blob);
	else if (property == vs_crtc->roi0_gamma_prop)
		*val = GET_BLOB_ID(vs_crtc_state->roi0_gamma.blob);
	else if (property == vs_crtc->roi1_gamma_prop)
		*val = GET_BLOB_ID(vs_crtc_state->roi1_gamma.blob);
	else if (property == vs_crtc->blur_mask_prop)
		*val = (vs_crtc_state->blur_mask) ? vs_crtc_state->blur_mask->base.id : 0;
	else if (property == vs_crtc->brightness_mask_prop)
		*val = (vs_crtc_state->brightness_mask) ? vs_crtc_state->brightness_mask->base.id :
							  0;
	else if (property == vs_crtc->expected_present_time)
		*val = vs_crtc_state->expected_present_time;
	else {
		/* histogram channels */
		for (int i = VS_HIST_CHAN_IDX_0; i < VS_HIST_CHAN_IDX_COUNT; i++) {
			if (property != vs_crtc->hist_chan_prop[i])
				continue;

			*val = GET_BLOB_ID(vs_crtc_state->hist_chan[i].blob);

			return 0;
		}

		/* dc porperty */
		return vs_dc_get_drm_property(vs_crtc_state->drm_states, &vs_crtc->properties,
					      property, val);
	}
	return 0;
}

int vs_crtc_resume(struct drm_atomic_state *suspend_state, struct drm_modeset_acquire_ctx *ctx)
{
	return drm_atomic_helper_commit_duplicated_state(suspend_state, ctx);
}

/**
 * vs_duplicate_active_crtc_state() - duplicates current state, forces crtc active
 *
 * @vs_crtc: crtc to force active in the duplicated state (meant for suspend)
 * @ctx: lock context for use in grabbing current state
 * Return: a duplication of the current state, but with crtc forced active
 */
static struct drm_atomic_state *vs_duplicate_active_crtc_state(struct vs_crtc *vs_crtc,
							       struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_crtc *crtc = &vs_crtc->base;
	struct drm_device *dev = crtc->dev;
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	struct vs_crtc_state *vs_crtc_state;
	int err;

	state = drm_atomic_state_alloc(dev);
	if (!state)
		return ERR_PTR(-ENOMEM);

	state->acquire_ctx = ctx;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		err = PTR_ERR(crtc_state);
		goto free_state;
	}

	if (!drm_atomic_crtc_effectively_active(crtc_state)) {
		err = -EPERM;
		dev_warn(vs_crtc->dev, "crtc[%s]: skipping duplication of inactive crtc state\n",
			 crtc->name);
		goto free_state;
	}

	err = drm_atomic_add_affected_planes(state, crtc);
	if (err)
		goto free_state;

	err = drm_atomic_add_affected_connectors(state, crtc);
	if (err)
		goto free_state;

	/* Ensure that display fully powers ON on resume */
	vs_crtc_state = to_vs_crtc_state(crtc_state);
	crtc_state->active = true;
	crtc_state->self_refresh_active = false;
	vs_crtc_state->power_off_mode = VS_POWER_OFF_MODE_FULL;

	/* clear the acquire context so that it isn't accidentally reused */
	state->acquire_ctx = NULL;

free_state:
	if (err < 0) {
		drm_atomic_state_put(state);
		state = ERR_PTR(err);
	}

	return state;
}

struct drm_atomic_state *vs_crtc_suspend(struct vs_crtc *vs_crtc,
					 struct drm_modeset_acquire_ctx *ctx)
{
	int ret, i;
	struct drm_crtc *crtc = &vs_crtc->base;
	struct drm_crtc_state *crtc_state;
	struct vs_crtc_state *vs_crtc_state;
	struct drm_atomic_state *state, *suspend_state;
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;

	/* copy the current state to save for resume operation */
	suspend_state = vs_duplicate_active_crtc_state(vs_crtc, ctx);
	if (IS_ERR_OR_NULL(suspend_state))
		return suspend_state;

	/* create a new state to move into */
	state = drm_atomic_state_alloc(crtc->dev);
	if (!state) {
		drm_atomic_state_put(suspend_state);
		return ERR_PTR(-ENOMEM);
	}
	state->acquire_ctx = ctx;

retry:
	/* create a new non-active crtc state */
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}

	/* Ensure that display fully powers OFF on suspend */
	vs_crtc_state = to_vs_crtc_state(crtc_state);
	crtc_state->active = false;
	crtc_state->self_refresh_active = false;
	vs_crtc_state->power_off_mode = VS_POWER_OFF_MODE_FULL;

	/* clear new crtc_state's properties and connections */
	ret = drm_atomic_set_mode_prop_for_crtc(crtc_state, NULL);
	if (ret)
		goto out;

	ret = drm_atomic_add_affected_planes(state, crtc);
	if (ret)
		goto out;

	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (ret)
		goto out;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		ret = drm_atomic_set_crtc_for_connector(conn_state, NULL);
		if (ret)
			goto out;
	}

	for_each_new_plane_in_state(state, plane, plane_state, i) {
		ret = drm_atomic_set_crtc_for_plane(plane_state, NULL);
		if (ret)
			goto out;

		drm_atomic_set_fb_for_plane(plane_state, NULL);
	}

	/* commit the new state */
	ret = drm_atomic_commit(state);
out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(ctx);
		if (!ret)
			goto retry;
	}
	if (ret) {
		drm_atomic_state_put(suspend_state);
		suspend_state = ERR_PTR(ret);
	}

	drm_atomic_state_put(state);

	return suspend_state;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int vs_crtc_debugfs_show(struct seq_file *s, void *data)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;

	seq_printf(s, "crtc[%u]: %s\n", crtc->base.id, crtc->name);
	seq_printf(s, "\tactive = %d\n", crtc->state->active);
	seq_printf(s, "\tsize = %dx%d\n", mode->hdisplay, mode->vdisplay);
	seq_printf(s, "\tbpp = %u\n", vs_crtc_state->bpp);
	seq_printf(s, "\tunderrun = %d\n", vs_crtc_state->underrun);
	seq_printf(s, "\tseamless_mode_change = %d\n", vs_crtc_state->seamless_mode_change);
	seq_printf(s, "\tframe start timeout = %d\n", atomic_read(&vs_crtc->frame_start_timeout));
	seq_printf(s, "\tframe done timeout = %d\n", atomic_read(&vs_crtc->frame_done_timeout));
	seq_printf(s, "\tframe start missing = %d\n", atomic_read(&vs_crtc->frame_start_missing));
	seq_printf(s, "\thardware reset = %d\n", atomic_read(&vs_crtc->hw_reset_count));

	if (vs_crtc->funcs->get_crtc_scanout_position &&
	    pm_runtime_get_if_in_use(vs_crtc->dc_dev) > 0) {
		u32 position;

		vs_crtc->funcs->get_crtc_scanout_position(vs_crtc->dc_dev, crtc, &position);

		seq_printf(s, "\tscanout_pos = %#08x\n", position);

		vs_dc_power_put(vs_crtc->dc_dev, true);
	} else {
		seq_puts(s, "\tscanout_pos = N/A\n");
	}


	return 0;
}

static int vs_crtc_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, vs_crtc_debugfs_show, inode->i_private);
}

static const struct file_operations vs_crtc_debugfs_fops = {
	.open = vs_crtc_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vs_crtc_pattern_show(struct seq_file *s, void *data)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	return vs_crtc->funcs->show_pattern_config(s);
}

static int vs_crtc_pattern_open(struct inode *inode, struct file *file)
{
	return single_open(file, vs_crtc_pattern_show, inode->i_private);
}

static ssize_t vs_crtc_pattern_write(struct file *file, const char __user *ubuf, size_t len,
				     loff_t *offp)
{
	struct seq_file *s = file->private_data;
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	if (vs_crtc->funcs->set_pattern)
		vs_crtc->funcs->set_pattern(crtc, ubuf, len);

	return len;
}

static int vs_crtc_crc_show(struct seq_file *s, void *data)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	if (!vs_crtc->funcs->show_crc)
		return -EINVAL;

	return vs_crtc->funcs->show_crc(s);
}

static ssize_t vs_crtc_crc_write(struct file *file, const char __user *ubuf, size_t len,
				 loff_t *offp)
{
	struct seq_file *s = file->private_data;
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	if (vs_crtc->funcs->set_crc)
		vs_crtc->funcs->set_crc(vs_crtc->dev, crtc, ubuf, len);

	return len;
}

static int vs_crtc_crc_open(struct inode *inode, struct file *file)
{
	return single_open(file, vs_crtc_crc_show, inode->i_private);
}

static const struct file_operations vs_crtc_pattern_fops = {
	.open = vs_crtc_pattern_open,
	.read = seq_read,
	.write = vs_crtc_pattern_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations vs_crtc_crc_fops = {
	.open = vs_crtc_crc_open,
	.read = seq_read,
	.write = vs_crtc_crc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vs_recovery_show(struct seq_file *s, void *data)
{
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	seq_printf(s, "%d\n", vs_crtc->recovery.count);
	return 0;
}

static ssize_t vs_recovery_write(struct file *file, const char __user *ubuf, size_t len,
				 loff_t *offp)
{
	struct seq_file *s = file->private_data;
	struct drm_crtc *crtc = s->private;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	vs_crtc_trigger_recovery(vs_crtc);

	return len;
}

static int vs_recovery_open(struct inode *inode, struct file *file)
{
	return single_open(file, vs_recovery_show, inode->i_private);
}

static const struct file_operations vs_recovery_fops = {
	.owner = THIS_MODULE,
	.open = vs_recovery_open,
	.read = seq_read,
	.write = vs_recovery_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vs_crtc_debugfs_init(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	debugfs_create_file("status", 0444, crtc->debugfs_entry, crtc, &vs_crtc_debugfs_fops);

	debugfs_create_file("pattern", 0644, crtc->debugfs_entry, crtc, &vs_crtc_pattern_fops);

	debugfs_create_file("CRC", 0644, crtc->debugfs_entry, crtc, &vs_crtc_crc_fops);

	debugfs_create_file("recovery", 0644, crtc->debugfs_entry, crtc, &vs_recovery_fops);

	debugfs_create_atomic_t("te_count", 0444, crtc->debugfs_entry, &vs_crtc->te_count);
	debugfs_create_atomic_t("frame_done_count", 0444, crtc->debugfs_entry,
						&vs_crtc->frame_done_count);

	debugfs_create_u32("power_off_mode", 0644, crtc->debugfs_entry,
			   &vs_crtc->power_off_mode_override);

	return 0;
}
#else
static int vs_crtc_debugfs_init(struct drm_crtc *crtc)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

static int vs_crtc_late_register(struct drm_crtc *crtc)
{
	return vs_crtc_debugfs_init(crtc);
}

static void vs_drm_vblank_enable_work_thread(struct kthread_work *work)
{
	struct vs_crtc *vs_crtc = container_of(work, struct vs_crtc, vblank_enable_work);
	unsigned long irqflags;

	vs_dc_power_get(vs_crtc->dc_dev, true);
	spin_lock_irqsave(&vs_crtc->vblank_enable_lock, irqflags);
	if (vs_crtc->vblank_enable_in_progress) {
		vs_crtc->vblank_en = true;
		vs_crtc->funcs->enable_vblank(vs_crtc, true);
		vs_crtc->vblank_enable_in_progress = false;
	} else {
		if (vs_dc_power_put(vs_crtc->dc_dev, false) == -EINPROGRESS)
			dev_warn(vs_crtc->dev, "crtc_enable_vblank: suspend already in progress\n");
	}
	spin_unlock_irqrestore(&vs_crtc->vblank_enable_lock, irqflags);
}

static void queue_vblank_enable_thread(struct vs_crtc *vs_crtc)
{
	if (!kthread_queue_work(vs_crtc->vblank_worker, &vs_crtc->vblank_enable_work))
		dev_err(vs_crtc->dev, "Error queueing enable kthread\n");
}

/**
 * vs_crtc_enable_vblank() - Enables vblank interrupts on crtc
 * @crtc: Reference to base drm_crtc
 *
 * Wraps the process of enabling vblank interrupts on the crtc in a fashion that
 * ensures correct power sequencing. As this may require a synchronous power_get
 * operation, this also handles some of the concurrency risks.
 *
 * Return: Always returns 0
 */
static int vs_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	unsigned long irqflags;

	dev_dbg(dev, "%s: power_state:%d vblank_en:%d\n", __func__, vs_crtc_state->power_state,
		vs_crtc->vblank_en);

	WARN(vs_crtc->vblank_en, "crtc_enable_vblank: vblank already enabled\n");

	if (pm_runtime_get_if_active(vs_crtc->dc_dev, true) > 0) {
		spin_lock_irqsave(&vs_crtc->vblank_enable_lock, irqflags);
		vs_crtc->vblank_en = true;
		vs_crtc->funcs->enable_vblank(vs_crtc, true);
		spin_unlock_irqrestore(&vs_crtc->vblank_enable_lock, irqflags);
	} else {
		spin_lock_irqsave(&vs_crtc->vblank_enable_lock, irqflags);
		vs_crtc->vblank_enable_in_progress = true;
		queue_vblank_enable_thread(vs_crtc);
		spin_unlock_irqrestore(&vs_crtc->vblank_enable_lock, irqflags);
	}

	return 0;
}

/**
 * vs_crtc_disable_vblank() - Disable vblank interrupts on crtc
 * @crtc: Reference to base drm_crtc
 *
 * Wraps the process of disabling vblank interrupts on the crtc in a fashion that
 * ensures correct power sequencing. Designed with knowledge of the
 * concurrency-handling necessary to work with vs_crtc_enable_vblank.
 */
static void vs_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	unsigned long irqflags;

	dev_dbg(dev, "%s: power_state:%d vblank_en:%d\n", __func__, vs_crtc_state->power_state,
		vs_crtc->vblank_en);

	spin_lock_irqsave(&vs_crtc->vblank_enable_lock, irqflags);
	if (vs_crtc->vblank_enable_in_progress) {
		vs_crtc->vblank_enable_in_progress = false;
	} else {
		WARN(!vs_crtc->vblank_en, "crtc_disable_vblank: vblank already disabled\n");
		vs_crtc->vblank_en = false;
		vs_crtc->funcs->enable_vblank(vs_crtc, false);
		if (vs_dc_power_put(vs_crtc->dc_dev, false) == -EINPROGRESS)
			dev_warn(dev, "crtc_disable_vblank: suspend already in progress\n");
	}
	spin_unlock_irqrestore(&vs_crtc->vblank_enable_lock, irqflags);
}

static void vs_crtc_flush_vblank_event(struct drm_crtc *crtc)
{
	unsigned long flags;

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		crtc->state->event = NULL;
	}
}

static uint32_t vs_crtc_get_vblank_count(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	return vs_crtc->funcs->get_vblank_count(vs_crtc);
}

static void vs_crtc_atomic_print_state(struct drm_printer *p,
				       const struct drm_crtc_state *crtc_state)
{
	const struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);
	struct vs_crtc *vs_crtc = to_vs_crtc(vs_crtc_state->base.crtc);

	drm_printf(p, "\tbpp = %u\n", vs_crtc_state->bpp);
	drm_printf(p, "\tencoder_type = %d\n", vs_crtc_state->encoder_type);
	drm_printf(p, "\toutput_id = %u\n", vs_crtc_state->output_id);
	drm_printf(p, "\toutput_mode = %#x\n", vs_crtc_state->output_mode);
	drm_printf(p, "\tunderrun = %d\n", vs_crtc_state->underrun);
	drm_printf(p, "\tseamless_mode_change = %d\n", vs_crtc_state->seamless_mode_change);

	if (test_bit(VS_QOS_OVERRIDE_CORE_CLK, vs_crtc_state->qos_override))
		drm_printf(p, "\tcore_clk = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.core_clk,
			   vs_crtc_state->requested_qos_config.core_clk);
	else
		drm_printf(p, "\tcore_clk = %u\n", vs_crtc_state->qos_config.core_clk);

	if (test_bit(VS_QOS_OVERRIDE_RD_AVG_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\trd_avg_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.rd_avg_bw_mbps,
			   vs_crtc_state->requested_qos_config.rd_avg_bw_mbps);
	else
		drm_printf(p, "\trd_avg_bw_mbps = %u\n", vs_crtc_state->qos_config.rd_avg_bw_mbps);

	if (test_bit(VS_QOS_OVERRIDE_RD_PEAK_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\trd_peak_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.rd_peak_bw_mbps,
			   vs_crtc_state->requested_qos_config.rd_peak_bw_mbps);
	else
		drm_printf(p, "\trd_peak_bw_mbps = %u\n",
			   vs_crtc_state->qos_config.rd_peak_bw_mbps);

	if (test_bit(VS_QOS_OVERRIDE_RD_RT_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\trd_rt_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.rd_rt_bw_mbps,
			   vs_crtc_state->requested_qos_config.rd_rt_bw_mbps);
	else
		drm_printf(p, "\trd_rt_bw_mbps = %u\n", vs_crtc_state->qos_config.rd_rt_bw_mbps);

	if (test_bit(VS_QOS_OVERRIDE_WR_AVG_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\twr_avg_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.wr_avg_bw_mbps,
			   vs_crtc_state->requested_qos_config.wr_avg_bw_mbps);
	else
		drm_printf(p, "\twr_avg_bw_mbps = %u\n", vs_crtc_state->qos_config.wr_avg_bw_mbps);

	if (test_bit(VS_QOS_OVERRIDE_WR_PEAK_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\twr_peak_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.wr_peak_bw_mbps,
			   vs_crtc_state->requested_qos_config.wr_peak_bw_mbps);
	else
		drm_printf(p, "\twr_peak_bw_mbps = %u\n",
			   vs_crtc_state->qos_config.wr_peak_bw_mbps);

	if (test_bit(VS_QOS_OVERRIDE_WR_RT_BW, vs_crtc_state->qos_override))
		drm_printf(p, "\twr_rt_bw_mbps = %u (requested:%u)\n",
			   vs_crtc_state->qos_config.wr_rt_bw_mbps,
			   vs_crtc_state->requested_qos_config.wr_rt_bw_mbps);
	else
		drm_printf(p, "\twr_rt_bw_mbps = %u\n", vs_crtc_state->qos_config.wr_rt_bw_mbps);

	drm_printf(p, "\tpower_off_mode = %s\n",
		   vs_power_off_mode_enum_list[vs_crtc_state->power_off_mode].name);
	drm_printf(p, "\tpower_off_mode_changed = %d\n", vs_crtc_state->power_off_mode_changed);
	drm_printf(p, "\tpower_state = %s\n", power_state_names[vs_crtc_state->power_state]);

	if (vs_crtc->bld_size_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->bld_size_prop->name, vs_crtc_state->bld_size);

	if (vs_crtc->prior_gamma_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->prior_gamma_prop->name,
			   vs_crtc_state->prior_gamma.blob ?
				   vs_crtc_state->prior_gamma.blob->base.id :
				   0);

	if (vs_crtc->roi0_gamma_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->roi0_gamma_prop->name,
			   vs_crtc_state->roi0_gamma.blob ?
				   vs_crtc_state->roi0_gamma.blob->base.id :
				   0);

	if (vs_crtc->roi1_gamma_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->roi1_gamma_prop->name,
			   vs_crtc_state->roi1_gamma.blob ?
				   vs_crtc_state->roi1_gamma.blob->base.id :
				   0);

	if (vs_crtc->blur_mask_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->blur_mask_prop->name,
			   vs_crtc_state->blur_mask ? vs_crtc_state->blur_mask->base.id : 0);

	if (vs_crtc->brightness_mask_prop)
		drm_printf(p, "\t%s=%d\n", vs_crtc->brightness_mask_prop->name,
			   vs_crtc_state->brightness_mask ?
				   vs_crtc_state->brightness_mask->base.id :
				   0);

	vs_dc_print_drm_properties(vs_crtc_state->drm_states, &vs_crtc->properties, p);
}

static const struct drm_crtc_funcs vs_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = vs_crtc_destroy,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = vs_crtc_reset,
	.atomic_duplicate_state = vs_crtc_atomic_duplicate_state,
	.atomic_destroy_state = vs_crtc_atomic_destroy_state,
	.atomic_set_property = vs_crtc_atomic_set_property,
	.atomic_get_property = vs_crtc_atomic_get_property,
	.late_register = vs_crtc_late_register,
	.enable_vblank = vs_crtc_enable_vblank,
	.disable_vblank = vs_crtc_disable_vblank,
	.get_vblank_counter = vs_crtc_get_vblank_count,
	.atomic_print_state = vs_crtc_atomic_print_state,
};

static u8 cal_pixel_bits(u32 bus_format)
{
	u8 bpp;

	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB565_1X16:
	case MEDIA_BUS_FMT_UYVY8_1X16:
		bpp = 16;
		break;
	case MEDIA_BUS_FMT_RGB666_1X18:
	case MEDIA_BUS_FMT_RGB666_1X24_CPADHI:
		bpp = 18;
		break;
	case MEDIA_BUS_FMT_UYVY10_1X20:
		bpp = 20;
		break;
	case MEDIA_BUS_FMT_BGR888_1X24:
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_YUV8_1X24:
		bpp = 24;
		break;
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_YUV10_1X30:
		bpp = 30;
		break;
	default:
		bpp = 24;
		break;
	}

	return bpp;
}

static void vs_crtc_send_vblank_event_locked(struct drm_crtc *crtc)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct drm_pending_event *event = &vs_crtc->event->base;

	DPU_ATRACE_BEGIN(__func__);

	vs_crtc->event = NULL;

	spin_lock(&dev->event_lock);
	drm_send_event_locked(dev, event);
	spin_unlock(&dev->event_lock);

	drm_crtc_vblank_put(crtc);

	DPU_ATRACE_END(__func__);
}

static void vs_crtc_handle_flip_error(struct drm_crtc *crtc, int error)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct drm_printer p = drm_info_printer(dev);
	unsigned long flags;

	if (!dc->disable_hw_reset) {
		vs_crtc->needs_hw_reset = true;
		atomic_inc(&vs_crtc->hw_reset_count);
	}

	if (dc->hw_reg_dump_options == DC_HW_REG_DUMP_IN_CONSOLE)
		dc_hw_reg_dump(&dc->hw, &p, DC_HW_REG_BANK_ACTIVE);
	else if (dc->hw_reg_dump_options == DC_HW_REG_DUMP_IN_TRACE)
		dc_hw_reg_dump(&dc->hw, NULL, DC_HW_REG_BANK_ACTIVE);

	if (!dc->disable_crtc_recovery)
		vs_crtc_trigger_recovery(vs_crtc);

	atomic_set(&vs_crtc->frames_pending, 0);
	DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending), vs_crtc->trace_pid,
			       "frames_pending[%u]", crtc->index);
	vs_crtc->frame_transfer_pending = false;
	vs_crtc->ltm_hist_query_pending = false;

	spin_lock_irqsave(&dc->int_lock, flags);
	if (vs_crtc->event) {
		if (vs_crtc->event->base.fence)
			vs_crtc->event->base.fence->error = error;
		vs_crtc_send_vblank_event_locked(crtc);
	}
	spin_unlock_irqrestore(&dc->int_lock, flags);
}

void vs_crtc_wait_for_flip_done(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_crtc_state *old_crtc_state = drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_crtc_commit *commit = new_crtc_state->commit;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct device *dev = vs_crtc->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	u8 display_id = to_vs_display_id(dc, crtc);
	int min_fps;
	bool missing_fs_int = false;
	int old_te_count, new_te_count;
	int frames_pending;
	unsigned long wait_time_jiffies;

	DPU_ATRACE_BEGIN(__func__);

	if (!commit)
		goto end;

	if (vs_crtc_state->skip_update) {
		DPU_ATRACE_INSTANT_PID_FMT(vs_crtc->trace_pid, "skip_update");
		dev_dbg(dev, "[CRTC:%d:%s] skip update, bypass wait for flip done\n", crtc->base.id,
			crtc->name);
		goto end;
	}

	old_te_count = atomic_read(&vs_crtc->te_count);
	min_fps = drm_mode_vrefresh(&new_crtc_state->mode);
	if (old_crtc_state->active)
		min_fps = min(min_fps, drm_mode_vrefresh(&old_crtc_state->mode));

	wait_time_jiffies = fps_timeout(min_fps);
	if (!wait_for_completion_timeout(&commit->flip_done, wait_time_jiffies)) {
		/*
		 * it's possible that interrupt handler wasn't called and thus status/completion
		 * hasn't been updated. Run the interrupt handler in case there are hw events to
		 * be handled (including frame start).
		 */
		vs_dc_check_interrupts(dev);
		missing_fs_int = true;
	}

	if (!completion_done(&commit->flip_done)) {
		new_te_count = atomic_read(&vs_crtc->te_count);
		frames_pending = atomic_read(&vs_crtc->frames_pending);

		if (frames_pending > 1) {
			trace_disp_frame_done_timeout(display_id, vs_crtc);
			atomic_inc(&vs_crtc->frame_done_timeout);
		} else {
			trace_disp_frame_start_timeout(display_id, vs_crtc);
			atomic_inc(&vs_crtc->frame_start_timeout);
		}

		DRM_DEV_ERROR(dev,
			      "%s: frame %s timed out after %u ms, vrefresh at (%u Hz)%s, frames pending %d, transfer pending %d",
			      crtc->name, (frames_pending > 1) ? "done" : "start",
			      jiffies_to_msecs(wait_time_jiffies),
			      drm_mode_vrefresh(&new_crtc_state->mode),
			      (new_te_count == old_te_count) ? "" : ", TE count changed",
			      frames_pending, vs_crtc->frame_transfer_pending);

		vs_crtc_handle_flip_error(crtc, -ETIMEDOUT);
	} else if (missing_fs_int) {
		trace_disp_frame_start_missing(display_id, vs_crtc);
		dev_warn(dev, "%s: frame start interrupt handler didn't run, dpu is %s\n",
			 crtc->name, dc->enabled ? "enabled" : "disabled");
		atomic_inc(&vs_crtc->frame_start_missing);
	} else {
		dc_hw_display_flip_done(&dc->hw, display_id);
	}

end:
	DPU_ATRACE_END(__func__);
}

static bool vs_crtc_was_powered_off(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct vs_crtc_state *old_vs_crtc_state = to_vs_crtc_state(old_state);

	if (!old_vs_crtc_state)
		return true;

	if (old_vs_crtc_state->power_state == VS_POWER_STATE_PSR)
		return true;

	if (old_vs_crtc_state->power_state == VS_POWER_STATE_OFF)
		return true;

	return false;
}

static void vs_crtc_enter_power_on(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	int ret;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct vs_dc *vs_dc = dev_get_drvdata(dev);

	DPU_ATRACE_BEGIN(__func__);
	dev_dbg(dev, "%s: enter ON vblank_en:%d\n", __func__, vs_crtc->vblank_en);

	ret = vs_dc_power_get(vs_crtc->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power ON\n", __func__);

	if (vs_crtc_state->base.active_changed && vs_dc->boost_fabrt_freq) {
		/* boost FABRT freq during register configuration */
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_PENDING);
	}

	vs_crtc_state->bpp = cal_pixel_bits(vs_crtc_state->output_fmt);
	vs_crtc->funcs->enable(vs_crtc->dev, crtc, state);

	drm_crtc_vblank_on(crtc);

	DPU_ATRACE_END(__func__);
}

static void vs_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	pid_t pid = vs_crtc ? vs_crtc->trace_pid : 0;

	DPU_ATRACE_BEGIN(__func__);
	DPU_ATRACE_INT_PID("power_state", vs_crtc_state->power_state, pid);
	trace_disp_enable(vs_crtc->id, crtc->state);

	__pm_stay_awake(vs_crtc->ws);
	if (vs_crtc_state->power_state != VS_POWER_STATE_ON)
		dev_err(dev, "%s: invalid power_state:%d\n", __func__, vs_crtc_state->power_state);

	vs_crtc_enter_power_on(crtc, state);
	DPU_ATRACE_END(__func__);
}

static void vs_crtc_disable_pipeline(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;

	drm_atomic_helper_disable_planes_on_crtc(old_state, false);
	vs_crtc->funcs->disable(dev, crtc);
}

static void vs_crtc_enter_power_off(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	int ret;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	bool was_powered_off = vs_crtc_was_powered_off(crtc, old_state);

	DPU_ATRACE_BEGIN(__func__);
	dev_dbg(dev, "%s: enter OFF vblank_en:%d was_powered_off:%d\n", __func__,
		vs_crtc->vblank_en, was_powered_off);

	if (was_powered_off) {
		dev_info(dev, "%s: was powered OFF, skipping pipeline disable", __func__);
	} else {
		vs_crtc_disable_pipeline(crtc, old_state);
	}

	drm_crtc_vblank_off(crtc);
	vs_crtc_flush_vblank_event(crtc);

	if (!was_powered_off) {
		ret = vs_dc_power_put(vs_crtc->dc_dev, true);
		if (ret < 0)
			dev_err(dev, "failed to power OFF\n");
	}

	DPU_ATRACE_END(__func__);
}

static void vs_crtc_enter_psr(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	int ret;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	bool was_powered_off = vs_crtc_was_powered_off(crtc, old_state);

	DPU_ATRACE_BEGIN(__func__);
	dev_dbg(dev, "%s: enter PSR vblank_en:%d was_powered_off:%d\n", __func__,
		vs_crtc->vblank_en, was_powered_off);

	if (was_powered_off) {
		dev_info(dev, "%s: DPU was powered OFF, needs to power it ON\n", __func__);
		ret = vs_dc_power_get(vs_crtc->dc_dev, true);
		if (ret < 0)
			dev_err(dev, "failed to power ON\n");

	} else {
		vs_crtc_disable_pipeline(crtc, old_state);
	}

	vs_crtc_flush_vblank_event(crtc);

	ret = vs_dc_power_put(vs_crtc->dc_dev, true);
	if (ret < 0)
		dev_err(dev, "failed to power OFF\n");

	DPU_ATRACE_END(__func__);
}

static void vs_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct drm_crtc_state *old_state = drm_atomic_get_old_crtc_state(state, crtc);
	pid_t pid = vs_crtc ? vs_crtc->trace_pid : 0;

	DPU_ATRACE_BEGIN(__func__);
	DPU_ATRACE_INT_PID("power_state", vs_crtc_state->power_state, pid);
	trace_disp_disable(vs_crtc->id, crtc->state);

	/* reset the fabrt boost state */
	WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_INIT);

	switch (vs_crtc_state->power_state) {
	case VS_POWER_STATE_OFF:
		vs_crtc_enter_power_off(crtc, old_state);
		break;
	case VS_POWER_STATE_ON:
		/* by default called during full mode change */
		vs_crtc_enter_power_off(crtc, old_state);
		break;
	case VS_POWER_STATE_PSR:
		vs_crtc_enter_psr(crtc, old_state);
		break;
	default:
		dev_err(dev, "%s: invalid power_state:%d\n", __func__, vs_crtc_state->power_state);
		vs_crtc_enter_power_off(crtc, old_state);
		break;
	}
	__pm_relax(vs_crtc->ws);
	DPU_ATRACE_END(__func__);
}

static void vs_crtc_atomic_begin(struct drm_crtc *crtc, struct drm_atomic_state *old_state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct device *dev = vs_crtc->dev;

	if (vs_crtc_state->need_boost_fabrt)
		WRITE_ONCE(vs_crtc->fboost_state, VS_FABRT_BOOST_PENDING);

	if (vs_crtc->funcs->config)
		vs_crtc->funcs->config(dev, crtc);
}

static void vs_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *old_state)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct device *dev = vs_crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (vs_crtc->event)
		dev_err(dev, "[CRTC:%d:%s] %s: pending vblank event %p\n", crtc->base.id,
			crtc->name, __func__, vs_crtc->event);

	if (event && !crtc->state->no_vblank)
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

	if (!vs_crtc->trace_pid)
		vs_crtc->trace_pid = current->tgid;

	if (vs_crtc_state->skip_update) {
		DPU_ATRACE_INSTANT_PID_FMT(vs_crtc->trace_pid, "skip_update");
		dev_dbg(dev, "[CRTC:%d:%s] skip update, bypass commit\n", crtc->base.id,
			crtc->name);
	} else {
		vs_crtc->funcs->commit(dev, crtc, old_state);
	}

	/* if event is still set and wasn't handled by now, arm it */
	if (crtc->state->event && !crtc->state->no_vblank) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_arm_vblank_event(crtc, event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		crtc->state->event = NULL;
	}
}

static bool is_self_refresh_aware(struct device *dev, struct drm_atomic_state *state,
				  struct drm_crtc_state *crtc_state)
{
	int i;
	bool self_refresh_aware = false;
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!(crtc_state->connector_mask & drm_connector_mask(conn)))
			continue;

		if (!conn_state->self_refresh_aware) {
			self_refresh_aware = false;
			break;
		}

		self_refresh_aware = true;
	}

	if (!self_refresh_aware)
		dev_err(dev, "self-refresh not supported on [CONN:%d:%s]\n", conn->base.id,
			conn->name);

	return self_refresh_aware;
}

int vs_crtc_check_power_state(struct drm_atomic_state *state, struct drm_crtc *crtc,
			      struct drm_crtc_state *crtc_state)
{
	int ret;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct device *dev = vs_crtc->dev;
	struct drm_crtc_state *old_crtc_state = crtc->state;
	struct drm_crtc_state *new_crtc_state = crtc_state;
	struct vs_crtc_state *old_vs_crtc_state = to_vs_crtc_state(old_crtc_state);
	struct vs_crtc_state *new_vs_crtc_state = to_vs_crtc_state(new_crtc_state);
	enum drm_vs_power_off_mode power_off_mode;
	bool self_refresh_aware;

	/* override power off mode with debugFS node value */
	if (vs_crtc->power_off_mode_override >= VS_POWER_OFF_MODE_FULL &&
	    vs_crtc->power_off_mode_override < VS_POWER_OFF_MODE_COUNT) {
		power_off_mode = vs_crtc->power_off_mode_override;
		dev_dbg(dev, "[CRTC:%d:%s] overriding power off mode to %d\n", crtc->base.id,
			crtc->name, power_off_mode);
	} else {
		power_off_mode = new_vs_crtc_state->power_off_mode;
	}

	new_vs_crtc_state->power_off_mode_changed =
		(old_vs_crtc_state->power_off_mode != power_off_mode);
	new_crtc_state->active_changed = (old_crtc_state->active != new_crtc_state->active);

	dev_dbg(dev, "[CRTC:%d:%s] enable=%d->%d active=%d->%d sra=%d->%d power_off_mode=%d->%d\n",
		crtc->base.id, crtc->name, old_crtc_state->enable, new_crtc_state->enable,
		old_crtc_state->active, new_crtc_state->active, old_crtc_state->self_refresh_active,
		new_crtc_state->self_refresh_active, old_vs_crtc_state->power_off_mode,
		new_vs_crtc_state->power_off_mode);

	/* check if either POWER_OFF_MODE or ACTIVE has changed */
	if (!new_vs_crtc_state->power_off_mode_changed && !new_crtc_state->active_changed)
		return 0;

	/* when CRTC is disable, power state is always OFF */
	if (!new_crtc_state->enable) {
		new_crtc_state->self_refresh_active = false;
		new_vs_crtc_state->power_state = VS_POWER_STATE_OFF;
		goto end;
	}

	/* when CRTC is active, power state is always ON */
	if (new_crtc_state->active) {
		new_crtc_state->self_refresh_active = false;
		new_vs_crtc_state->power_state = VS_POWER_STATE_ON;
		goto end;
	}

	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (ret)
		return 0;

	/* determine the DPU POWER STATE based on POWER_OFF_MODE and ACTIVE values */
	switch (power_off_mode) {
	case VS_POWER_OFF_MODE_FULL:
		new_crtc_state->self_refresh_active = false;
		new_vs_crtc_state->power_state = VS_POWER_STATE_OFF;
		break;

	case VS_POWER_OFF_MODE_PSR:
		self_refresh_aware = is_self_refresh_aware(dev, state, new_crtc_state);
		if (!self_refresh_aware)
			return -EINVAL;

		new_crtc_state->self_refresh_active = true;
		new_vs_crtc_state->power_state = VS_POWER_STATE_PSR;
		break;
	default:
		dev_err(dev, "invalid power_off_mode %d on [CRTC:%d:%s]\n", power_off_mode,
			crtc->base.id, crtc->name);
		return -EINVAL;
	};

end:
	if (old_vs_crtc_state->power_state != new_vs_crtc_state->power_state)
		new_crtc_state->active_changed = true;

	dev_dbg(dev, "[CRTC:%d:%s] power_state=%s->%s\n", crtc->base.id, crtc->name,
		power_state_names[old_vs_crtc_state->power_state],
		power_state_names[new_vs_crtc_state->power_state]);

	return 0;
}

static int vs_crtc_atomic_check_skip_update(struct device *dev, struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state)
{
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc_state);

	if (vs_crtc_state->force_skip_update) {
		vs_crtc_state->skip_update = true;
		dev_dbg(dev, "[CRTC:%d:%s] force skip_update=1\n", crtc->base.id, crtc->name);
		return 0;
	}

	if (!vs_crtc_state->skip_update)
		goto end;

	if (vs_crtc_state->planes_updated || vs_crtc_state->wb_connectors_updated ||
	    crtc_state->color_mgmt_changed || crtc_state->mode_changed ||
	    crtc_state->connectors_changed)
		vs_crtc_state->skip_update = false;

end:

	if (vs_crtc_state->skip_update && (!crtc_state->event ||
	    crtc_state->active_changed))
		crtc_state->no_vblank = true;

	dev_dbg(dev,
		"[CRTC:%d:%s] skip_update=%d planes_updated=%d color_mgmt_changed=%d mode_changed=%d active_changed=%d connectors_changed=%d wb_connectors_updated=%d no_vblank=%d event=%pK\n",
		crtc->base.id, crtc->name, vs_crtc_state->skip_update,
		vs_crtc_state->planes_updated, crtc_state->color_mgmt_changed,
		crtc_state->mode_changed, crtc_state->active_changed,
		crtc_state->connectors_changed, vs_crtc_state->wb_connectors_updated,
		crtc_state->no_vblank, crtc_state->event);

	return 0;
}

static int vs_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	int ret;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct drm_crtc_state *crtc_state = NULL;
	struct device *dev = vs_crtc->dev;

	if (!crtc)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	ret = vs_crtc_atomic_check_skip_update(dev, crtc, crtc_state);
	if (ret)
		return ret;

	ret = vs_crtc->funcs->check(dev, crtc, crtc_state);
	if (ret)
		return ret;

	return vs_qos_check_qos_config(crtc, crtc_state);
}

static bool vs_crtc_get_scanout_position(struct drm_crtc *crtc, bool in_vblank_irq, int *vpos,
					 int *hpos, ktime_t *stime, ktime_t *etime,
					 const struct drm_display_mode *mode)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;

	return vs_display_get_crtc_scanoutpos(dev, pipe, in_vblank_irq, vpos, hpos, stime, etime,
					      mode);
}

static const struct drm_crtc_helper_funcs vs_crtc_helper_funcs = {
	.atomic_enable = vs_crtc_atomic_enable,
	.atomic_disable = vs_crtc_atomic_disable,
	.atomic_begin = vs_crtc_atomic_begin,
	.atomic_flush = vs_crtc_atomic_flush,
	.atomic_check = vs_crtc_atomic_check,
	.get_scanout_position = vs_crtc_get_scanout_position,
};

static const struct drm_prop_enum_list vs_sync_mode_enum_list[] = {
	{ VS_SINGLE_DC, "single dc mode" },
	{ VS_MULTI_DC_PRIMARY, "primary dc for multi dc mode" },
	{ VS_MULTI_DC_SECONDARY, "secondary dc for multi dc mode" },
};

static int vs_crtc_create_hw_capability_blob(struct drm_device *drm_dev, struct vs_crtc *crtc,
					     struct vs_display_info *display_info)
{
	struct drm_vs_crtc_hw_caps *hw_caps;
	struct drm_property_blob *blob;

	crtc->hw_caps_prop = drm_property_create(
		drm_dev, DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_BLOB, "HW_CAPS", 0);
	if (!crtc->hw_caps_prop)
		return -EINVAL;

	blob = drm_property_create_blob(drm_dev, sizeof(struct drm_vs_crtc_hw_caps), 0);
	if (!blob)
		return -EINVAL;

	hw_caps = blob->data;
	hw_caps->hw_id = display_info->id;
	hw_caps->max_width = display_info->max_width;
	hw_caps->max_height = display_info->max_height;
	hw_caps->min_scale = display_info->min_scale;
	hw_caps->max_scale = display_info->max_scale;

	drm_object_attach_property(&crtc->base.base, crtc->hw_caps_prop, blob->base.id);

	return 0;
}

struct vs_crtc *vs_crtc_create(const struct dc_hw_display *display, struct drm_device *drm_dev,
				struct vs_dc *dc, const struct vs_dc_info *info, u8 index)
{
	struct vs_crtc *crtc;
	struct vs_display_info *display_info = NULL;
	int ret;
	struct sched_param param = { .sched_priority = VBLANK_KTHREAD_SCHED_PRIORITY };

	if (!info)
		return NULL;

	display_info = (struct vs_display_info *)&info->displays[index];
	if (!display_info)
		return NULL;

	crtc = kzalloc(sizeof(*crtc), GFP_KERNEL);
	if (!crtc)
		return NULL;

	spin_lock_init(&crtc->slock_ltm_hist);
	ret = drm_crtc_init_with_planes(drm_dev, &crtc->base, NULL, NULL, &vs_crtc_funcs, "%s",
					display_info->name);
	if (ret)
		goto err_free_crtc;

	drm_crtc_helper_add(&crtc->base, &vs_crtc_helper_funcs);

	crtc->commit_worker = kthread_create_worker(0, "dpu_kthread%u", index);
	if (IS_ERR(crtc->commit_worker)) {
		ret = PTR_ERR(crtc->commit_worker);
		goto err_free_crtc;
	}

	crtc->vblank_worker = kthread_create_worker(0, "crtc_vblank%u", index);
	if (IS_ERR(crtc->vblank_worker)) {
		ret = PTR_ERR(crtc->vblank_worker);
		goto err_free_crtc;
	}
	sched_setscheduler_nocheck(crtc->vblank_worker->task, SCHED_FIFO, &param);
	kthread_init_work(&crtc->vblank_enable_work, vs_drm_vblank_enable_work_thread);
	spin_lock_init(&crtc->vblank_enable_lock);

	/* Set up the crtc properties */
	if (info->pipe_sync) {
		crtc->sync_mode_prop = drm_property_create_enum(drm_dev, 0, "SYNC_MODE",
								vs_sync_mode_enum_list,
								ARRAY_SIZE(vs_sync_mode_enum_list));

		if (!crtc->sync_mode_prop)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->sync_mode_prop, VS_SINGLE_DC);
	}

	if (display_info->gamma) {
		if (info->std_color_lut) {
			ret = drm_mode_crtc_set_gamma_size(&crtc->base, info->max_gamma_size);
			if (ret)
				goto err_cleanup_crtc;

			drm_crtc_enable_color_mgmt(&crtc->base, 0, display_info->ccm_linear,
						   info->max_gamma_size);
		} else {
			crtc->prior_gamma_prop =
				drm_property_create(drm_dev, DRM_MODE_PROP_BLOB, "PRIOR_GAMMA", 0);

			if (!crtc->prior_gamma_prop)
				goto err_cleanup_crtc;

			drm_object_attach_property(&crtc->base.base, crtc->prior_gamma_prop, 0);
		}

		if (display_info->lut_roi) {
			crtc->roi0_gamma_prop =
				drm_property_create(drm_dev, DRM_MODE_PROP_BLOB, "ROI0_GAMMA", 0);

			if (!crtc->roi0_gamma_prop)
				goto err_cleanup_crtc;

			drm_object_attach_property(&crtc->base.base, crtc->roi0_gamma_prop, 0);

			crtc->roi1_gamma_prop =
				drm_property_create(drm_dev, DRM_MODE_PROP_BLOB, "ROI1_GAMMA", 0);

			if (!crtc->roi1_gamma_prop)
				goto err_cleanup_crtc;

			drm_object_attach_property(&crtc->base.base, crtc->roi1_gamma_prop, 0);
		}
	}

	if (info->panel_sync) {
		crtc->panel_sync_prop = drm_property_create_bool(drm_dev, 0, "SYNC_ENABLED");

		if (!crtc->panel_sync_prop)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->panel_sync_prop, 0);
	}

	if (display_info->min_scale != 1 << 16 || display_info->max_scale != 1 << 16) {
		crtc->bld_size_prop =
			drm_property_create_range(drm_dev, 0, "BLD_SIZE", 0, 0xFFFFFFFF);

		if (!crtc->bld_size_prop)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->bld_size_prop, 0);
	}

	/* histogram channel properties */
	if (display_info->histogram) {
		for (int i = VS_HIST_CHAN_IDX_0; i < VS_HIST_CHAN_IDX_COUNT; i++) {
			char name[32];
			const int ncount = VS_HIST_STAGE_COUNT + 1; /* number of buffers */
			const int nsize = ALIGN(sizeof(struct drm_vs_hist_chan_bins), 64);

			/*
			 * create histogram property: controls histogram channel configuration
			 */
			snprintf(name, sizeof(name), "HISTOGRAM_%d", i);
			crtc->hist_chan_prop[i] =
				drm_property_create(drm_dev, DRM_MODE_PROP_BLOB, name, 0);

			if (!crtc->hist_chan_prop[i])
				goto err_cleanup_crtc;

			drm_object_attach_property(&crtc->base.base, crtc->hist_chan_prop[i], 0);

			/*
			 * allocate gem_pool
			 */
			if (vs_gem_pool_init(drm_dev, &crtc->hist_chan_gem_pool[i], ncount, nsize))
				goto err_cleanup_crtc;
		}
	}

	/* histogram rgb */
	if (display_info->rgb_hist) {
		const int ncount = VS_HIST_STAGE_COUNT + 1; /* number of buffers */
		const int nsize = ALIGN(sizeof(struct drm_vs_hist_rgb_bins), 64);

		/*
		 * allocate gem_pool
		 */
		if (vs_gem_pool_init(drm_dev, &crtc->hist_rgb_gem_pool, ncount, nsize))
			goto err_cleanup_crtc;
	}

	if (display_info->blur) {
		crtc->blur_mask_prop = drm_property_create_object(drm_dev, DRM_MODE_PROP_ATOMIC,
								  "BLUR_MASK", DRM_MODE_OBJECT_FB);
		if (!crtc->blur_mask_prop)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->blur_mask_prop, 0);
	}

	if (display_info->brightness) {
		crtc->brightness_mask_prop = drm_property_create_object(
			drm_dev, DRM_MODE_PROP_ATOMIC, "BRIGHTNESS_MASK", DRM_MODE_OBJECT_FB);
		if (!crtc->brightness_mask_prop)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->brightness_mask_prop, 0);
	}

	if (dc->core_devfreq != NULL) {
		if (vs_qos_add_devfreq_request(dc, &crtc->base, DEV_PM_QOS_MIN_FREQUENCY,
					       PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE))
			goto err_cleanup_crtc;

		crtc->core_clk = drm_property_create_range(drm_dev, 0, "CORE_CLOCK_HZ", 0,
							   dc->core_devfreq->scaling_max_freq);
		if (!crtc->core_clk)
			goto err_cleanup_crtc;

		drm_object_attach_property(&crtc->base.base, crtc->core_clk,
					   dc->core_devfreq->scaling_min_freq);
	}

	if (dc->fabrt_devfreq != NULL) {
		if (vs_qos_add_fabrt_devfreq_request(dc, &crtc->base, DEV_PM_QOS_MIN_FREQUENCY,
						     PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE))
			goto err_cleanup_crtc;
	}

	/* TODO(b/364560647): set suitable range for bandwidth properties */
	crtc->rd_avg_bw_mbps = drm_property_create_range(drm_dev, 0, "RD_AVG_BW_MBPS",
							 0, UINT_MAX);
	if (!crtc->rd_avg_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->rd_avg_bw_mbps, 0);

	crtc->rd_peak_bw_mbps = drm_property_create_range(drm_dev, 0, "RD_PEAK_BW_MBPS",
							  0, UINT_MAX);
	if (!crtc->rd_peak_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->rd_peak_bw_mbps, 0);

	crtc->rd_rt_bw_mbps = drm_property_create_range(drm_dev, 0, "RD_RT_BW_MBPS",
							  0, UINT_MAX);
	if (!crtc->rd_rt_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->rd_rt_bw_mbps, 0);

	crtc->wr_avg_bw_mbps = drm_property_create_range(drm_dev, 0, "WR_AVG_BW_MBPS",
							 0, UINT_MAX);
	if (!crtc->wr_avg_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->wr_avg_bw_mbps, 0);

	crtc->wr_peak_bw_mbps = drm_property_create_range(drm_dev, 0, "WR_PEAK_BW_MBPS",
							  0, UINT_MAX);
	if (!crtc->wr_peak_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->wr_peak_bw_mbps, 0);
	crtc->power_off_mode = drm_property_create_enum(drm_dev, 0, "POWER_OFF_MODE",
							vs_power_off_mode_enum_list,
							ARRAY_SIZE(vs_power_off_mode_enum_list));
	if (!crtc->power_off_mode)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->power_off_mode, 0);
	crtc->power_off_mode_override = VS_POWER_OFF_MODE_COUNT;

	crtc->wr_rt_bw_mbps = drm_property_create_range(drm_dev, 0, "WR_RT_BW_MBPS",
							  0, UINT_MAX);
	if (!crtc->wr_rt_bw_mbps)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->wr_rt_bw_mbps, 0);

	crtc->expected_present_time = drm_property_create_range(drm_dev, 0, "EXPECTED_PRESENT_TIME",
								0, (uint64_t)(~((uint64_t)0)));
	if (!crtc->expected_present_time)
		goto err_cleanup_crtc;

	drm_object_attach_property(&crtc->base.base, crtc->expected_present_time, 0);

	ret = vs_crtc_create_hw_capability_blob(drm_dev, crtc, display_info);
	if (ret)
		goto err_cleanup_crtc;

	if (display != NULL && vs_dc_create_drm_properties(drm_dev, &crtc->base.base,
							   &display->states, &crtc->properties)) {
		goto err_cleanup_crtc;
	}

	crtc->max_bpc = info->max_bpc;
	crtc->color_formats = display_info->color_formats;
	crtc->id = index;
	crtc->ws = wakeup_source_register(drm_dev->dev, crtc->base.name);

	init_waitqueue_head(&crtc->framedone_waitq);

	vs_recovery_register(crtc);

	return crtc;

err_cleanup_crtc:
	drm_crtc_cleanup(&crtc->base);

err_free_crtc:
	kfree(crtc);
	return NULL;
}

void vs_crtc_handle_frm_start(struct drm_crtc *crtc, bool underrun)
{
	struct vs_crtc_state *vs_crtc_state = to_vs_crtc_state(crtc->state);
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	unsigned long flags;

	vs_crtc->t_vblank = ktime_get();
	if (vs_crtc->event)
		vs_crtc_send_vblank_event_locked(crtc);

	spin_lock_irqsave(&vs_crtc->slock_ltm_hist, flags);
	vs_crtc->ltm_hist_dma_addr_cur = vs_crtc->ltm_hist_dma_addr_nxt;
	vs_crtc->ltm_hist_dma_addr_nxt = 0;
	spin_unlock_irqrestore(&vs_crtc->slock_ltm_hist, flags);

	vs_crtc_state->underrun = underrun;
}

int vs_crtc_get_ltm_hist(struct drm_file *file_priv, struct vs_crtc *vs_crtc, struct dc_hw *hw,
			 struct drm_vs_ltm_histogram_data *data)
{
	int ret;

	if (!data)
		return -ENOMEM;

	DPU_ATRACE_BEGIN("wait_ltm_framedone");
	vs_crtc->ltm_hist_query_pending = true;
	ret = wait_event_timeout(vs_crtc->framedone_waitq,
				 !vs_crtc->ltm_hist_query_pending,
				 msecs_to_jiffies(data->timeout_ms));
	DPU_ATRACE_END("wait_ltm_framedone");
	if (!ret)
		return -ETIMEDOUT;

	return dc_hw_get_ltm_hist(hw, vs_crtc->id, data, vs_crtc_get_ltm_hist_dma_addr);
}

void vs_crtc_store_ltm_hist_dma_addr(struct device *dev, u8 hw_id, dma_addr_t addr)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	struct vs_crtc *crtc = dc->crtc[hw_id];
	unsigned long flags;

	spin_lock_irqsave(&crtc->slock_ltm_hist, flags);
	crtc->ltm_hist_dma_addr_nxt = addr;
	spin_unlock_irqrestore(&crtc->slock_ltm_hist, flags);
}

dma_addr_t vs_crtc_get_ltm_hist_dma_addr(struct device *dev, u8 hw_id)
{
	struct vs_dc *dc;
	struct vs_crtc *crtc;
	dma_addr_t addr;
	unsigned long flags;

	if (!dev || hw_id >= DC_DISPLAY_NUM) {
		dev_err(dev, "%s invalid input with device %p and hw_id %d\n", __func__, dev,
			hw_id);
		return 0;
	}

	dc = dev_get_drvdata(dev);
	if (!dc) {
		dev_err(dev, "%s null dc\n", __func__);
		return 0;
	}

	crtc = dc->crtc[hw_id];
	spin_lock_irqsave(&crtc->slock_ltm_hist, flags);
	addr = crtc->ltm_hist_dma_addr_cur;
	spin_unlock_irqrestore(&crtc->slock_ltm_hist, flags);

	return addr;
}
