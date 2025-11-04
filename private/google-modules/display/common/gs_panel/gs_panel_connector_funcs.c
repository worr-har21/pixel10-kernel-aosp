/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel_internal.h"

#include <linux/mutex.h>
#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <drm/drm_vblank.h>

#include "gs_panel/gs_panel.h"
#include "trace/panel_trace.h"

/* drm_connector_helper_funcs */

static int gs_panel_connector_modes(struct drm_connector *connector)
{
	struct gs_drm_connector *gs_connector = to_gs_connector(connector);
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	struct device *dev = ctx->dev;
	int ret;

	ret = drm_panel_get_modes(&ctx->base, connector);
	if (ret < 0) {
		dev_err(dev, "failed to get panel display modes\n");
		return ret;
	}

	return ret;
}

static void gs_panel_connector_attach_touch(struct gs_panel *ctx,
					    const struct drm_connector_state *connector_state)
{
	struct drm_encoder *encoder = connector_state->best_encoder;
	struct drm_bridge *bridge;

	if (!encoder) {
		dev_warn(ctx->dev, "%s encoder is null\n", __func__);
		return;
	}

	bridge = of_drm_find_bridge(ctx->touch_bridge_data.touch_dev);
	ctx->touch_bridge_data.retry_count++;
	if (!bridge)
		return;

	drm_bridge_attach(encoder, bridge, &ctx->bridge, 0);
	dev_info(ctx->dev, "attach bridge %p to encoder %p after %u tries\n", bridge, encoder,
		 ctx->touch_bridge_data.retry_count);
	ctx->touch_bridge_data.attached = true;
}

/*
 * this atomic check is called before adjusted mode is populated, this can be used to check only
 * connector state (without adjusted mode), or to decide if modeset may be required
 */
static int gs_panel_connector_atomic_check(struct drm_connector *connector,
					   struct drm_atomic_state *state)
{
	struct gs_drm_connector *gs_connector = to_gs_connector(connector);
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	struct drm_connector_state *old_conn_state, *new_conn_state, *conn_state;

	old_conn_state = drm_atomic_get_old_connector_state(state, connector);
	new_conn_state = drm_atomic_get_new_connector_state(state, connector);

	if (new_conn_state->crtc)
		conn_state = new_conn_state;
	else if (old_conn_state->crtc)
		conn_state = old_conn_state;
	else
		return 0; /* connector is/was unused */

	if (!ctx->touch_bridge_data.attached && ctx->touch_bridge_data.touch_dev)
		gs_panel_connector_attach_touch(ctx, conn_state);

	return 0;
}

static const struct drm_connector_helper_funcs drm_connector_helper_funcs = {
	.atomic_check = gs_panel_connector_atomic_check,
	.get_modes = gs_panel_connector_modes,
};

/* gs_drm_connector_funcs */

/**
 * fill_export_mode() - Fills the output memory with data from the panel mode
 * @out: Output buffer location to fill
 * @pmode: gs_panel_mode to translate to the exported version
 *
 * Return: 0 on success, negative value on error
 */
static int fill_export_mode(struct gs_panel_mode_export *out, const struct gs_panel_mode *pmode)
{
	drm_mode_convert_to_umode(&out->modeinfo, &pmode->mode);

	if (pmode->gs_mode.is_lp_mode)
		out->mode_usage_flags |= MODE_USAGE_LP_BIT;

	if (pmode->mode.vscan > 0)
		out->mode_usage_flags |= MODE_USAGE_VRR_ENABLED_BIT;

	out->mode_type.dsc_enabled = pmode->gs_mode.dsc.enabled;
	out->mode_type.sw_trigger = pmode->gs_mode.sw_trigger;
	out->mode_type.video_mode = (pmode->gs_mode.mode_flags & MIPI_DSI_MODE_VIDEO) != 0;
	out->mode_type.clock_non_continuous =
			(pmode->gs_mode.mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS) != 0;

	return 0;
}

/**
 * gs_panel_get_all_modes_blob() - Gets property containing all modes
 * @gs_connector: handle for gs_drm_connector
 * @val: Blob ID (output parameter)
 *
 * Creates a blob property on the connector containing all the possible panel
 * modes and fills in that property's id. Alternatively, if the property already
 * exists, just fills in the id.
 *
 * Return: 0 on success, negative value on error
 */
static int gs_panel_get_all_modes_prop(struct gs_drm_connector *gs_connector, uint64_t *val)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	const struct gs_panel_desc *desc = ctx->desc;
	struct drm_property_blob *blob = ctx->all_modes_blob;
	u16 num_exported_modes;
	u8 *mode_blob_tmp;
	struct gs_panel_mode_export_header *header;
	int i;
	struct gs_panel_mode_export *out_umode;

	if (unlikely(!desc->modes || desc->modes->num_modes == 0))
		return -EINVAL;

	if (blob) {
		*val = blob->base.id;
		return 0;
	}

	num_exported_modes = desc->modes->num_modes;
	if (desc->lp_modes && desc->lp_modes->num_modes)
		num_exported_modes += desc->lp_modes->num_modes;

	mode_blob_tmp = kzalloc(EXPORTED_MODE_BLOB_SIZE(num_exported_modes), GFP_KERNEL);
	if (!mode_blob_tmp)
		return -ENOMEM;
	header = (struct gs_panel_mode_export_header *)(mode_blob_tmp);

	/* fill out data about sizes, number of modes */
	header->header_size = sizeof(struct gs_panel_mode_export_header);
	header->mode_size = sizeof(struct gs_panel_mode_export);
	header->num_modes = num_exported_modes;

	/* move mode information into temp data */
	out_umode =
		(struct gs_panel_mode_export *)(mode_blob_tmp + EXPORTED_MODE_OFFSET(header, 0));
	for (i = 0; i < desc->modes->num_modes; ++i, out_umode++) {
		const struct gs_panel_mode *pmode = &desc->modes->modes[i];

		fill_export_mode(out_umode, pmode);
	}
	if (desc->lp_modes && desc->lp_modes->num_modes) {
		out_umode = (struct gs_panel_mode_export *)(mode_blob_tmp +
			EXPORTED_MODE_OFFSET(header, desc->modes->num_modes));
		for (i = 0; i < desc->lp_modes->num_modes; ++i, out_umode++) {
			const struct gs_panel_mode *pmode = &desc->lp_modes->modes[i];

			fill_export_mode(out_umode, pmode);
		}
	}

	/* create property blob, free tmp data */

	blob = drm_property_create_blob(gs_connector->base.dev,
					EXPORTED_MODE_BLOB_SIZE(num_exported_modes), mode_blob_tmp);
	kfree(mode_blob_tmp);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	ctx->all_modes_blob = blob;
	*val = blob->base.id;

	return 0;
}

/**
 * is_umode_lp_compatible - check switching between provided modes can be seamless during LP
 * @pmode: initial display mode
 * @umode: target display mode
 *
 * Returns true if the switch to target mode can be seamless during LP
 */
static inline bool is_umode_lp_compatible(const struct gs_panel_mode *pmode,
					  const struct drm_mode_modeinfo *umode)
{
	return pmode->mode.vdisplay == umode->vdisplay && pmode->mode.hdisplay == umode->hdisplay;
}

static int gs_panel_get_lp_mode(struct gs_drm_connector *gs_connector,
				const struct gs_drm_connector_state *gs_state, uint64_t *val)
{
	const struct drm_connector_state *conn_state = &gs_state->base;
	const struct drm_crtc_state *crtc_state = conn_state->crtc ? conn_state->crtc->state : NULL;
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	const struct gs_panel_desc *desc = ctx->desc;
	struct drm_property_blob *blob = ctx->lp_mode_blob;
	const struct gs_panel_mode *cur_mode;
	struct drm_mode_modeinfo umode;

	if (crtc_state)
		cur_mode = gs_panel_get_mode(ctx, &crtc_state->mode);
	else
		cur_mode = READ_ONCE(ctx->current_mode);

	if (unlikely(!desc->lp_modes))
		return -EINVAL;

	if (blob) {
		if (!cur_mode || is_umode_lp_compatible(cur_mode, blob->data)) {
			dev_dbg(ctx->dev, "%s: returning existing lp mode blob\n", __func__);
			*val = blob->base.id;
			return 0;
		}
		ctx->lp_mode_blob = NULL;
		drm_property_blob_put(blob);
	}

	/* when mode count is 0, assume driver is only providing single LP mode */
	if ((desc->lp_modes && desc->lp_modes->num_modes <= 1) || !cur_mode) {
		dev_dbg(ctx->dev, "%s: only single LP mode available\n", __func__);
		drm_mode_convert_to_umode(&umode, &desc->lp_modes->modes[0].mode);
	} else if (desc->lp_modes) {
		int i;

		for (i = 0; i < desc->lp_modes->num_modes; i++) {
			const struct gs_panel_mode *lp_mode = &desc->lp_modes->modes[i];

			drm_mode_convert_to_umode(&umode, &lp_mode->mode);

			if (is_umode_lp_compatible(cur_mode, &umode)) {
				dev_dbg(ctx->dev, "%s: found lp mode: %s for mode:%s\n", __func__,
					lp_mode->mode.name, cur_mode->mode.name);
				break;
			}
		}

		if (i == desc->lp_modes->num_modes) {
			dev_warn(ctx->dev, "%s: unable to find compatible LP mode for mode: %s\n",
				 __func__, cur_mode->mode.name);
			return -ENOENT;
		}
	} else {
		return -ENOENT;
	}

	blob = drm_property_create_blob(gs_connector->base.dev, sizeof(umode), &umode);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	ctx->lp_mode_blob = blob;
	*val = blob->base.id;

	return 0;
}

static void gs_panel_connector_print_state(struct drm_printer *p,
					   const struct gs_drm_connector_state *state)
{
	const struct gs_drm_connector *gs_connector = to_gs_connector(state->base.connector);
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	const struct gs_panel_desc *desc = ctx->desc;
	int ret;

	/*TODO(b/267170999): MODE*/
	ret = mutex_lock_interruptible(&ctx->mode_lock);
	if (ret)
		return;

	drm_printf(p, "\tpanel_state: %d\n", ctx->panel_state);
	drm_printf(p, "\tidle: %d (%d)\n",
		   ctx->idle_data.panel_idle_vrefresh, ctx->idle_data.panel_idle_enabled);

	if (ctx->current_mode) {
		const struct drm_display_mode *m = &ctx->current_mode->mode;

		drm_printf(p, " \tcurrent mode: %s te@%d\n", m->name, gs_drm_mode_te_freq(m));
	}
	drm_printf(p, "\tpanel_id: %08x\n", ctx->panel_rev_id.id);
	drm_printf(p, "\tluminance: [%u, %u] avg: %u\n", desc->brightness_desc->min_luminance,
		   desc->brightness_desc->max_luminance, desc->brightness_desc->max_avg_luminance);
	drm_printf(p, "\thdr_formats: 0x%x\n", desc->hdr_formats);
	drm_printf(p, "\tglobal_hbm_mode: %u\n", state->global_hbm_mode);
	drm_printf(p, "\tdimming_on: %d\n", state->dimming_on);
	drm_printf(p, "\tis_partial: %d\n", desc->is_partial);
	drm_printf(p, "\tmin_refresh_rate: %u\n", ctx->sw_status.idle_vrefresh);
	drm_printf(p, "\tauto_mode: %d\n", state->auto_fi);
	drm_printf(p, "\tpwm_mode: %u\n", state->pwm_mode);
	drm_printf(p, "\tpanel_power_state: %d\n", state->panel_power_state);

	drm_printf(p, "\tbrightness_level: %d\n", state->brightness_level);
	drm_printf(p, "\tlocal_hbm_on: %d\n", state->local_hbm_on);
	drm_printf(p, "\toperation_rate: %u\n", state->operation_rate);
	drm_printf(p, "\tmipi_sync: 0x%lx\n", state->mipi_sync);
	drm_printf(p, "\trefresh_ctl_min_refresh_rate: %u\n", state->min_refresh_rate);
	drm_printf(p, "\tinsert_frames: %u\n", state->insert_frames);
	drm_printf(p, "\tframe_interval_us: %u us\n", state->frame_interval_us);
	drm_printf(p, "\tpending_update_flags: 0x%x\n", state->pending_update_flags);

	/*TODO(b/267170999): MODE*/
	mutex_unlock(&ctx->mode_lock);
}

static void gs_panel_update_connector_state(const struct gs_drm_connector *gs_connector,
					    struct gs_drm_connector_state *state)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	state->brightness_level = ctx->bl->props.brightness;
	state->global_hbm_mode = ctx->hbm_mode;
	state->local_hbm_on = ctx->lhbm.effective_state;
	state->dimming_on = ctx->dimming_on;
	state->operation_rate = ctx->op_hz;
	state->min_refresh_rate = GS_PANEL_REFRESH_CTRL_MIN_REFRESH_RATE(ctx->refresh_ctrl);
	state->insert_frames = GS_PANEL_REFRESH_CTRL_FI_FRAME_COUNT(ctx->refresh_ctrl);
	state->auto_fi = (ctx->refresh_ctrl & GS_PANEL_REFRESH_CTRL_FI_AUTO);
	state->pwm_mode = ctx->pwm_mode;
	state->frame_interval_us = ctx->frame_interval_us;
	state->panel_power_state =  ctx->panel_power_state;
}

static int gs_panel_connector_get_property(struct gs_drm_connector *gs_connector,
					   const struct gs_drm_connector_state *gs_state,
					   struct drm_property *property, uint64_t *val)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(gs_connector);
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	if (property == p->brightness_level) {
		*val = gs_state->brightness_level;
		dev_dbg(ctx->dev, "conn_get_prop: brt(%llu)\n", *val);
	} else if (property == p->global_hbm_mode) {
		*val = gs_state->global_hbm_mode;
		dev_dbg(ctx->dev, "conn_get_prop: global_hbm_mode(%llu)\n", *val);
	} else if (property == p->local_hbm_on) {
		*val = gs_state->local_hbm_on;
		dev_dbg(ctx->dev, "conn_get_prop: local_hbm_on(%s)\n", *val ? "true" : "false");
	} else if (property == p->dimming_on) {
		*val = gs_state->dimming_on;
		dev_dbg(ctx->dev, "conn_get_prop: dimming_on(%s)\n", *val ? "true" : "false");
	} else if (property == p->operation_rate) {
		*val = gs_state->operation_rate;
		dev_dbg(ctx->dev, "conn_get_prop: operation_rate(%llu)\n", *val);
	} else if (property == p->lp_mode) {
		return gs_panel_get_lp_mode(gs_connector, gs_state, val);
	} else if (property == p->all_modes) {
		return gs_panel_get_all_modes_prop(gs_connector, val);
	} else if (property == p->mipi_sync) {
		*val = gs_state->mipi_sync;
		dev_dbg(ctx->dev, "conn_get_prop: mipi_sync(0x%llx)\n", *val);
	} else if (property == p->frame_interval) {
		*val = gs_state->frame_interval_us * NSEC_PER_USEC;
		dev_dbg(ctx->dev, "gs_conn get prop: frame_interval(%llu)\n", *val);
	} else if (property == p->refresh_ctl_min_refresh_rate) {
		*val = gs_state->min_refresh_rate;
		dev_dbg(ctx->dev, "gs_conn get prop: min_rr(%llu)\n", *val);
	} else if (property == p->refresh_ctl_auto_frame_enabled) {
		*val = gs_state->auto_fi;
		dev_dbg(ctx->dev, "gs_conn get prop: auto_fi(%s)\n", *val ? "true" : "false");
	} else if (property == p->refresh_ctl_insert_frames) {
		*val = 0;
		dev_dbg(ctx->dev, "gs_conn get prop: insert_frames\n");
	} else if (property == p->pwm_mode) {
		*val = gs_state->pwm_mode;
		dev_dbg(ctx->dev, "gs_conn get prop: pwm_mode(%llu)\n", *val);
	} else if (property == p->panel_power_state) {
		*val = gs_state->panel_power_state;
		dev_dbg(ctx->dev, "gs_conn get prop: panel_power_state(%llu)\n", *val);
	} else {
		dev_err(ctx->dev, "gs_conn get prop: unrecognized property\n");
		return -EINVAL;
	}

	return 0;
}

static int gs_panel_connector_set_property(struct gs_drm_connector *gs_connector,
					   struct gs_drm_connector_state *gs_state,
					   struct drm_property *property, uint64_t val)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(gs_connector);
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	dev_dbg(ctx->dev, "%s+\n", __func__);

	if (property == p->brightness_level) {
		gs_state->pending_update_flags |= GS_HBM_FLAG_BL_UPDATE;
		gs_state->brightness_level = val;
		dev_dbg(ctx->dev, "conn_set_prop: brt(%u)\n", gs_state->brightness_level);
	} else if (property == p->global_hbm_mode) {
		gs_state->pending_update_flags |= GS_HBM_FLAG_GHBM_UPDATE;
		gs_state->global_hbm_mode = val;
		dev_dbg(ctx->dev, "conn_set_prop: global_hbm_mode(%u)\n",
			gs_state->global_hbm_mode);
	} else if (property == p->local_hbm_on) {
		gs_state->pending_update_flags |= GS_HBM_FLAG_LHBM_UPDATE;
		gs_state->local_hbm_on = val;
		dev_dbg(ctx->dev, "conn_set_prop: local_hbm_on(%s)\n",
			gs_state->local_hbm_on ? "true" : "false");
	} else if (property == p->dimming_on) {
		gs_state->pending_update_flags |= GS_HBM_FLAG_DIMMING_UPDATE;
		gs_state->dimming_on = val;
		dev_dbg(ctx->dev, "conn_set_prop: dimming_on(%s)\n",
			gs_state->dimming_on ? "true" : "false");
	} else if (property == p->operation_rate) {
		gs_state->pending_update_flags |= GS_FLAG_OP_RATE_UPDATE;
		gs_state->operation_rate = val;
		gs_state->update_operation_rate_to_bts = true;
		dev_dbg(ctx->dev, "conn_set_prop: operation_rate(%u)\n", gs_state->operation_rate);
	} else if (property == p->mipi_sync) {
		gs_state->mipi_sync = val;
		dev_dbg(ctx->dev, "conn_set_prop: mipi_sync(0x%lx)\n", gs_state->mipi_sync);
	} else if (property == p->refresh_ctl_min_refresh_rate) {
		gs_state->min_refresh_rate = val;
		gs_state->pending_update_flags |= GS_FLAG_MIN_RR_UPDATE;
		dev_dbg(ctx->dev, "conn_set_prop: min_rr(%llu)\n", val);
	} else if (property == p->refresh_ctl_insert_frames) {
		gs_state->insert_frames = val;
		gs_state->pending_update_flags |= GS_FLAG_INSERT_FRAMES;
		dev_dbg(ctx->dev, "conn_set_prop: insert_frames(%llu)\n", val);
	} else if (property == p->refresh_ctl_auto_frame_enabled) {
		gs_state->auto_fi = val;
		gs_state->pending_update_flags |= GS_FLAG_AUTO_FI_UPDATE;
		dev_dbg(ctx->dev, "conn_set_prop: auto_fi(%s)\n", val ? "true" : "false");
	} else if (property == p->frame_interval) {
		if (val != 0)
			do_div(val, NSEC_PER_USEC);
		gs_state->frame_interval_us = val;
		PANEL_ATRACE_INT("prop_frame_interval", val);
		dev_dbg(ctx->dev, "conn_set_prop: frame interval(%u)us\n",
			gs_state->frame_interval_us);
	} else if (property == p->pwm_mode) {
		gs_state->pwm_mode = val;
		gs_state->pending_update_flags |= GS_FLAG_PWM_MODE_UPDATE;
		dev_dbg(ctx->dev, "conn_set_prop: pwm_mode(%llu)\n", val);
	} else if (property == p->panel_power_state) {
		gs_state->panel_power_state = val;
		gs_state->pending_update_flags |= GS_FLAG_POWER_STATE_UPDATE;
		dev_dbg(ctx->dev, "conn_set_prop: panel_power_state(%llu)\n", val);
	} else {
		dev_err(ctx->dev, "property not recognized within %s- \n", __func__);
		return -EINVAL;
	}

	dev_dbg(ctx->dev, "%s-\n", __func__);
	return 0;
}

static int gs_panel_connector_late_register(struct gs_drm_connector *gs_connector)
{
	gs_panel_node_attach(gs_connector);
	return 0;
}

/**
 * gs_panel_get_mipi_datarate_for_mode() - calculate min datarate for mode
 * @ctx: Handle for gs_panel
 * @pmode: panel mode to consider
 *
 * Calculates the minimum mipi data rate per lane for the given mode
 *
 * Return: minimum mipi datarate, in Mbps, or negative value on error
 */
static int gs_panel_get_mipi_datarate_for_mode(struct gs_panel *ctx,
					       const struct gs_panel_mode *pmode)
{
	int num_lanes = ctx->desc->data_lane_cnt;
	int bits_per_pixel, width, height, fps;
	const struct drm_dsc_config *dsc_cfg = pmode->gs_mode.dsc.cfg;
	const struct drm_display_mode *mode = &pmode->mode;
	int64_t lane_speed_bps; /* for handling overflow issues */

	if (!mode || !dsc_cfg || num_lanes <= 0)
		return -EINVAL;

	/* note: contains 4 fractional bits */
	bits_per_pixel = dsc_cfg->bits_per_pixel;
	width = mode->hdisplay;
	height = mode->vdisplay;
	fps = drm_mode_vrefresh(mode);

	/* calculate value */
	/*
	 * Performing in stages because of overflow issues
	 * Functionally right-shift for bits_per_pixel encoding with divide-by-16
	 * Also, adding 10% blanking factor (thus, the 11/10)
	 */
	lane_speed_bps = width * height * fps;
	lane_speed_bps = mult_frac(lane_speed_bps, bits_per_pixel, num_lanes * 16);
	lane_speed_bps = mult_frac(lane_speed_bps, 11, 10);
	dev_dbg(ctx->dev,
		"Calculated mipi datarate of %lldbps for panel mode %s, num_lanes %d, bpp raw %d\n",
		lane_speed_bps, mode->name, num_lanes, bits_per_pixel);

	/* return value in mbps */
	return lane_speed_bps / (1000 * 1000);
}

static int
gs_panel_get_max_mipi_datarate_for_mode_array(struct gs_panel *ctx,
					      const struct gs_panel_mode_array *mode_array)
{
	int i;
	const struct gs_panel_mode *pmode;
	int max_datarate = -EINVAL;

	if (!mode_array)
		return -EINVAL;

	for (i = 0; i < mode_array->num_modes; ++i, pmode = &mode_array->modes[i]) {
		int current_datarate = gs_panel_get_mipi_datarate_for_mode(ctx, pmode);

		if (current_datarate > max_datarate)
			max_datarate = current_datarate;
	}
	return max_datarate;
}

static int gs_panel_get_max_mipi_datarate(struct gs_drm_connector *gs_connector, bool is_lp)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	if (is_lp)
		return gs_panel_get_max_mipi_datarate_for_mode_array(ctx, ctx->desc->lp_modes);
	return gs_panel_get_max_mipi_datarate_for_mode_array(ctx, ctx->desc->modes);
}

static int gs_panel_get_mipi_allowed_datarates(struct gs_drm_connector *gs_connector,
			struct gs_mipi_clks *mipi_clk)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	*mipi_clk = ctx->allowed_hs_clks;
	return 0;
}

static void gs_panel_update_dev_stat(const struct gs_drm_connector *gs_connector, u32 *dev_stat)
{
	const struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	_gs_panel_update_dev_stat(dev_stat, ctx);
}

static int gs_panel_register_op_hz_notifier(struct gs_drm_connector *gs_connector,
					    struct notifier_block *nb)
{
	int retval;
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	retval = blocking_notifier_chain_register(&ctx->op_hz_notifier_head, nb);
	if (retval != 0)
		dev_warn(ctx->dev, "register notifier failed(%d)\n", retval);
	else
		blocking_notifier_call_chain(&ctx->op_hz_notifier_head, GS_PANEL_NOTIFIER_SET_OP_HZ,
					     &ctx->op_hz);

	return retval;
}

static int gs_panel_unregister_op_hz_notifier(struct gs_drm_connector *gs_connector,
					      struct notifier_block *nb)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	return blocking_notifier_chain_unregister(&ctx->op_hz_notifier_head, nb);
}

static const struct gs_drm_connector_funcs gs_drm_connector_funcs = {
	.atomic_print_state = gs_panel_connector_print_state,
	.atomic_get_property = gs_panel_connector_get_property,
	.atomic_set_property = gs_panel_connector_set_property,
	.late_register = gs_panel_connector_late_register,
	.get_max_mipi_datarate = gs_panel_get_max_mipi_datarate,
	.register_op_hz_notifier = gs_panel_register_op_hz_notifier,
	.unregister_op_hz_notifier = gs_panel_unregister_op_hz_notifier,
	.panel_update_connector_state = gs_panel_update_connector_state,
	.get_mipi_allowed_datarates = gs_panel_get_mipi_allowed_datarates,
	.panel_update_dev_stat = gs_panel_update_dev_stat,
};

/* gs_drm_connector_helper_funcs */

int gs_panel_set_op_hz(struct gs_panel *ctx, unsigned int hz)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_funcs *funcs = ctx->desc->gs_panel_func;
	int ret = 0;
	bool need_update = false;

	if (!gs_is_panel_initialized(ctx))
		return -EAGAIN;

	if (!gs_panel_has_func(ctx, set_op_hz))
		return -ENOTSUPP;

	/*TODO(tknelms) DPU_ATRACE_BEGIN("set_op_hz");*/
	dev_dbg(dev, "%s: set op_hz to %d\n", __func__, hz);

	/*TODO(b/267170999): MODE*/
	mutex_lock(&ctx->mode_lock);
	if (ctx->op_hz != hz) {
		ret = funcs->set_op_hz(ctx, hz);
		if (ret)
			dev_err(dev, "failed to set op rate: %u Hz\n", hz);
		else
			need_update = true;
	} else {
		dev_dbg(dev, "%s: skip the same op rate: %u Hz\n", __func__, hz);
	}
	/*TODO(b/267170999): MODE*/
	mutex_unlock(&ctx->mode_lock);

	if (need_update) {
		/*TODO(b/333697598): Use async notify or work queue to notify.*/
		PANEL_ATRACE_BEGIN("notify_op_hz");
		blocking_notifier_call_chain(&ctx->op_hz_notifier_head, GS_PANEL_NOTIFIER_SET_OP_HZ,
					     &ctx->op_hz);
		PANEL_ATRACE_END("notify_op_hz");
		sysfs_notify(&dev->kobj, NULL, "op_hz");
	}

	/*TODO(tknelms) DPU_ATRACE_END("set_op_hz");*/

	return ret;
}

int gs_panel_set_pwm_mode(struct gs_panel *ctx, enum gs_pwm_mode mode)
{
	int ret;

	if (!gs_panel_has_func(ctx, set_pwm_mode))
		return -EOPNOTSUPP;

	if (!gs_is_panel_active(ctx))
		return -EPERM;

	if (mode == ctx->pwm_mode)
		return 0;

	mutex_lock(&ctx->mode_lock);
	ret = ctx->desc->gs_panel_func->set_pwm_mode(ctx, mode);
	mutex_unlock(&ctx->mode_lock);
	if (ret)
		return ret;

	sysfs_notify(&ctx->dev->kobj, NULL, "pwm_mode");

	return 0;
}

int gs_panel_set_panel_power_state(struct gs_panel *ctx, enum gs_panel_power_state new_state)
{
	bool mp_en = (new_state == GS_PANEL_POWER_STATE_MP);
	if (!gs_is_panel_active(ctx))
		return -EPERM;

	PANEL_ATRACE_BEGIN("set_panel_power_state");
	dev_dbg(ctx->dev, "new panel power state: %d\n", new_state);

	/* TODO: b/402868084 - handle other power states to be controlled by HWC */
	mutex_lock(&ctx->mode_lock);
	if (gs_panel_has_func(ctx, set_mp_mode_en))
		ctx->desc->gs_panel_func->set_mp_mode_en(ctx, mp_en);
	else
		dev_info(ctx->dev, "MP mode: %d\n", mp_en);

	PANEL_ATRACE_INSTANT("MP mode: %d", mp_en);
	ctx->panel_power_state = new_state;
	notify_panel_mode_changed(ctx);
	mutex_unlock(&ctx->mode_lock);
	PANEL_ATRACE_END("set_panel_power_state");

	return 0;
}

static int gs_panel_run_detect_fault(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	int ret;
	ktime_t now;
	s64 delta_ms;
	u32 fault_detect_interval_ms = ctx->desc->fault_detect_interval_ms;

	if (!fault_detect_interval_ms)
		return 0;

	if (!gs_is_panel_active(ctx)) {
		dev_dbg(dev, "skip fault detection (panel is not active)\n");
		return 0;
	}

	/* Check elapsed interval against minimum */
	now = ktime_get();
	delta_ms = ktime_ms_delta(now, ctx->timestamps.last_panel_fault_check_ts);
	if (delta_ms < fault_detect_interval_ms) {
		dev_dbg(dev, "skip fault detection (%lldms since last check, <%ums)\n", delta_ms,
			fault_detect_interval_ms);
		return 0;
	}

	/* Read fault registers */
	ret = ctx->desc->gs_panel_func->detect_fault(ctx);

	/* Update last fault check timestamp */
	ctx->timestamps.last_panel_fault_check_ts = now;

	return ret;
}

static void gs_panel_commit_properties(struct gs_panel *ctx,
					   struct gs_drm_connector_state *conn_state)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	const struct gs_panel_funcs *gs_panel_func = ctx->desc->gs_panel_func;
	bool mipi_sync;
	bool ghbm_updated = false;

	mutex_lock(&ctx->mode_lock);
	if (conn_state->frame_interval_us)
		ctx->frame_interval_us = conn_state->frame_interval_us;

	/* assign target present timestamp to panel context */
	ctx->timestamps.conn_last_present_ts = conn_state->crtc_last_present_ts;

	mutex_unlock(&ctx->mode_lock);

	if (!conn_state->pending_update_flags)
		return;

	dev_dbg(ctx->dev, "%s: mipi_sync(0x%lx) pending_update_flags(0x%x)\n", __func__,
		conn_state->mipi_sync, conn_state->pending_update_flags);
	/*TODO(tknelms) DPU_ATRACE_BEGIN(__func__);*/
	mipi_sync = conn_state->mipi_sync &
		    (GS_MIPI_CMD_SYNC_LHBM | GS_MIPI_CMD_SYNC_GHBM | GS_MIPI_CMD_SYNC_BL |
		     GS_MIPI_CMD_SYNC_PWM_MODE);

	if ((conn_state->mipi_sync & (GS_MIPI_CMD_SYNC_LHBM | GS_MIPI_CMD_SYNC_GHBM)) &&
	    ctx->current_mode->gs_mode.is_lp_mode) {
		conn_state->pending_update_flags &= ~(
			GS_HBM_FLAG_LHBM_UPDATE | GS_HBM_FLAG_GHBM_UPDATE | GS_HBM_FLAG_BL_UPDATE);
		dev_warn(ctx->dev, "%s: avoid LHBM/GHBM/BL updates during lp mode\n", __func__);
	}

	if (mipi_sync) {
		gs_panel_wait_for_cmd_tx_window(conn_state->base.crtc, ctx->current_mode,
						ctx->current_mode, ctx);
		gs_dsi_dcs_write_buffer_force_batch_begin(dsi);
	}

	if ((conn_state->pending_update_flags & GS_HBM_FLAG_GHBM_UPDATE) &&
	    gs_panel_has_func(ctx, set_hbm_mode) &&
	    (ctx->hbm_mode != conn_state->global_hbm_mode)) {
		PANEL_ATRACE_BEGIN("set_hbm");
		/*TODO(b/267170999): MODE*/
		mutex_lock(&ctx->mode_lock);
		gs_panel_func->set_hbm_mode(ctx, conn_state->global_hbm_mode);
		notify_panel_mode_changed(ctx);
		/*TODO(b/267170999): MODE*/
		mutex_unlock(&ctx->mode_lock);
		PANEL_ATRACE_END("set_hbm");
		ghbm_updated = true;
	}

	if ((conn_state->pending_update_flags & GS_HBM_FLAG_BL_UPDATE) &&
	    (ctx->bl->props.brightness != conn_state->brightness_level)) {
		PANEL_ATRACE_BEGIN("set_bl");
		/*
		 * backlight update happens at the same time that atomic_commit is taking
		 * place, any delays can be avoided by command alignment.
		 */
		mutex_lock(&ctx->mode_lock);
		ctx->skip_cmd_align = true;
		mutex_unlock(&ctx->mode_lock);
		ctx->bl->props.brightness = conn_state->brightness_level;
		backlight_update_status(ctx->bl);
		mutex_lock(&ctx->mode_lock);
		ctx->skip_cmd_align = false;
		mutex_unlock(&ctx->mode_lock);
		PANEL_ATRACE_END("set_bl");
	}

	if ((conn_state->pending_update_flags & GS_HBM_FLAG_LHBM_UPDATE) &&
	    gs_panel_has_func(ctx, set_local_hbm_mode)) {
		/* TODO(b/261073288) PANEL_ATRACE_BEGIN("set_lhbm"); */
		dev_dbg(ctx->dev, "%s: set LHBM to %d\n", __func__, conn_state->local_hbm_on);
		/* TODO(b/267170999): MODE */
		mutex_lock(&ctx->mode_lock);
		ctx->lhbm.requested_state = conn_state->local_hbm_on ? GLOCAL_HBM_ENABLED :
								       GLOCAL_HBM_DISABLED;
		panel_update_lhbm(ctx);
		/* TODO(b/267170999): MODE */
		mutex_unlock(&ctx->mode_lock);
		/* TODO(b/261073288) PANEL_ATRACE_END("set_lhbm"); */
	}

	if ((conn_state->pending_update_flags & GS_HBM_FLAG_DIMMING_UPDATE) &&
	    gs_panel_has_func(ctx, set_dimming) && (ctx->dimming_on != conn_state->dimming_on)) {
		/* TODO(b/261073288) PANEL_ATRACE_BEGIN("set_dimming"); */
		gs_panel_set_dimming(ctx, conn_state->dimming_on);
		/* TODO(b/261073288) PANEL_ATRACE_END("set_dimming"); */
	}

	if (conn_state->pending_update_flags & GS_FLAG_OP_RATE_UPDATE)
		gs_panel_set_op_hz(ctx, conn_state->operation_rate);

	if (conn_state->pending_update_flags & GS_FLAG_PWM_MODE_UPDATE)
		gs_panel_set_pwm_mode(ctx, conn_state->pwm_mode);

	if (conn_state->pending_update_flags & GS_FLAG_POWER_STATE_UPDATE)
		gs_panel_set_panel_power_state(ctx, conn_state->panel_power_state);

	if (mipi_sync)
		gs_dsi_dcs_write_buffer_force_batch_end(dsi);

	if (((GS_MIPI_CMD_SYNC_GHBM | GS_MIPI_CMD_SYNC_BL) & conn_state->mipi_sync) &&
	    !(GS_MIPI_CMD_SYNC_LHBM & conn_state->mipi_sync) && ctx->desc->dbv_extra_frame) {
		/**
		* panel needs one extra VSYNC period to apply GHBM/dbv. The frame
		* update should be delayed.
		*/
		/*TODO(tknelms) DPU_ATRACE_BEGIN("dbv_wait");*/
		if (!drm_crtc_vblank_get(conn_state->base.crtc)) {
			drm_crtc_wait_one_vblank(conn_state->base.crtc);
			drm_crtc_vblank_put(conn_state->base.crtc);
		} else {
			pr_warn("%s failed to get vblank for dbv wait\n", __func__);
		}
		/*TODO(tknelms) DPU_ATRACE_END("dbv_wait");*/
	}

	if (gs_panel_has_func(ctx, refresh_ctrl)) {
		bool update_refresh_ctrl = false;
		u32 refresh_ctrl;

		mutex_lock(&ctx->mode_lock);
		refresh_ctrl = ctx->refresh_ctrl;
		if (conn_state->pending_update_flags & GS_FLAG_MIN_RR_UPDATE) {
			update_refresh_ctrl = true;
			GS_PANEL_REFRESH_CTRL_SET_MIN_REFRESH_RATE(refresh_ctrl,
								   conn_state->min_refresh_rate);
		}
		if (conn_state->pending_update_flags & GS_FLAG_INSERT_FRAMES) {
			update_refresh_ctrl = true;
			GS_PANEL_REFRESH_CTRL_SET_FI_FRAME_COUNT(refresh_ctrl,
								 conn_state->insert_frames);
		}
		if (conn_state->pending_update_flags & GS_FLAG_AUTO_FI_UPDATE) {
			update_refresh_ctrl = true;
			if (conn_state->auto_fi)
				refresh_ctrl |= GS_PANEL_REFRESH_CTRL_FI_AUTO;
			else
				refresh_ctrl &= ~GS_PANEL_REFRESH_CTRL_FI_AUTO;
		}

		if (update_refresh_ctrl)
			ctx->refresh_ctrl = refresh_ctrl;
		mutex_unlock(&ctx->mode_lock);
	}

	if (ghbm_updated)
		sysfs_notify(&ctx->bl->dev.kobj, NULL, "hbm_mode");

	/*TODO(tknelms) DPU_ATRACE_END(__func__);*/
}

static void gs_panel_connector_atomic_pre_commit(struct gs_drm_connector *gs_connector,
						 struct gs_drm_connector_state *gs_old_state,
						 struct gs_drm_connector_state *gs_new_state)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	struct gs_panel_idle_data *idle_data = &ctx->idle_data;

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (idle_data->panel_update_idle_mode_pending)
		panel_update_idle_mode_locked(ctx, false);
	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
}

static void gs_panel_connector_atomic_commit(struct gs_drm_connector *gs_connector,
					     struct gs_drm_connector_state *gs_old_state,
					     struct gs_drm_connector_state *gs_new_state)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);

	/* send mipi_sync commands at the time close to the expected present time */
	gs_panel_commit_properties(ctx, gs_new_state);

	/*TODO(b/267170999): MODE*/
	mutex_lock(&ctx->mode_lock);
	if (gs_panel_has_func(ctx, commit_done))
		ctx->desc->gs_panel_func->commit_done(ctx);
	if (gs_panel_has_func(ctx, detect_fault))
		gs_panel_run_detect_fault(ctx);

	ctx->mode_in_progress = MODE_DONE;
	/*TODO(b/267170999): MODE*/
	mutex_unlock(&ctx->mode_lock);

	ctx->timestamps.last_commit_ts = ktime_get();

	/*
	 * TODO: Identify other kinds of errors and ensure detection is debounced
	 *	 correctly
	 */
	if (gs_old_state->is_recovering &&
	    !((ctx->current_mode->gs_mode.mode_flags & MIPI_DSI_MODE_VIDEO) != 0)) {
		mutex_lock(&ctx->mode_lock);
		ctx->error_counter.te++;
		sysfs_notify(&ctx->dev->kobj, NULL, "error_count_te");
		mutex_unlock(&ctx->mode_lock);
	}

	if (gs_panel_has_func(ctx, refresh_ctrl) &&
	    gs_new_state->pending_update_flags & GS_FLAG_REFRESH_CTRL_UPDATE)
		gs_panel_refresh_ctrl(ctx, gs_new_state->frame_start_ts);
	return;
}

static const struct gs_drm_connector_helper_funcs gs_drm_connector_helper_funcs = {
	.atomic_pre_commit = gs_panel_connector_atomic_pre_commit,
	.atomic_commit = gs_panel_connector_atomic_commit,
};

/* Initialization */

static int gs_panel_attach_brightness_capability(struct gs_drm_connector *gs_conn,
						 const struct brightness_capability *brt_capability)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(gs_conn);
	struct drm_property_blob *blob;

	blob = drm_property_create_blob(gs_conn->base.dev, sizeof(struct brightness_capability),
					brt_capability);
	if (IS_ERR(blob))
		return PTR_ERR(blob);
	drm_object_attach_property(&gs_conn->base.base, p->brightness_capability, blob->base.id);

	return 0;
}

static int gs_panel_connector_attach_properties(struct gs_panel *ctx)
{
	struct gs_drm_connector_properties *p = gs_drm_connector_get_properties(ctx->gs_connector);
	struct drm_mode_object *obj = &ctx->gs_connector->base.base;
	const struct gs_panel_desc *desc = ctx->desc;
	int ret = 0;

	if (!p || !desc)
		return -ENOENT;

	dev_dbg(ctx->dev, "%s+\n", __func__);

	drm_object_attach_property(obj, p->min_luminance, desc->brightness_desc->min_luminance);
	drm_object_attach_property(obj, p->max_luminance, desc->brightness_desc->max_luminance);
	drm_object_attach_property(obj, p->max_avg_luminance,
				   desc->brightness_desc->max_avg_luminance);
	drm_object_attach_property(obj, p->hdr_formats, desc->hdr_formats);
	drm_object_attach_property(obj, p->brightness_level, 0);
	drm_object_attach_property(obj, p->global_hbm_mode, 0);
	drm_object_attach_property(obj, p->local_hbm_on, 0);
	drm_object_attach_property(obj, p->dimming_on, 0);
	drm_object_attach_property(obj, p->mipi_sync, 0);
	drm_object_attach_property(obj, p->is_partial, desc->is_partial);
	drm_object_attach_property(obj, p->panel_idle_support, desc->is_idle_supported);
	drm_object_attach_property(obj, p->panel_orientation, ctx->orientation);
	drm_object_attach_property(obj, p->rr_switch_duration, desc->rr_switch_duration);
	drm_object_attach_property(obj, p->operation_rate, 0);
	drm_object_attach_property(obj, p->refresh_on_lp, desc->refresh_on_lp);
	drm_object_attach_property(obj, p->frame_interval, desc->frame_interval_us);
	drm_object_attach_property(obj, p->refresh_ctl_insert_frames, 0);
	drm_object_attach_property(obj, p->refresh_ctl_min_refresh_rate, 0);
	drm_object_attach_property(obj, p->refresh_ctl_auto_frame_enabled, false);
	drm_object_attach_property(obj, p->pwm_mode, 0);
	drm_object_attach_property(obj, p->panel_power_state, 0);
	drm_object_attach_property(obj, p->all_modes, 0);

	if (desc->brightness_desc->brt_capability) {
		ret = gs_panel_attach_brightness_capability(ctx->gs_connector,
							    desc->brightness_desc->brt_capability);
		if (ret)
			dev_err(ctx->dev, "Failed to attach brightness capability (%d)\n", ret);
	}

	if (desc->lp_modes && desc->lp_modes->num_modes > 0)
		drm_object_attach_property(obj, p->lp_mode, 0);

	dev_dbg(ctx->dev, "%s-\n", __func__);

	return ret;
}

int gs_panel_initialize_gs_connector(struct gs_panel *ctx, struct drm_device *drm_dev,
				     struct gs_drm_connector *gs_connector)
{
	struct device *dev = ctx->dev;
	struct drm_connector *connector = &gs_connector->base;
	int ret = 0;

	/* Initialize drm_connector */
	if (!gs_connector->base.funcs) {
		gs_connector_bind(gs_connector->kdev, NULL, drm_dev);
	}
	ret = drm_connector_init(drm_dev, connector, gs_connector->base.funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret) {
		dev_err(dev, "Error initializing drm_connector (%d)\n", ret);
		return ret;
	}

	/* Attach functions */
	gs_connector->funcs = &gs_drm_connector_funcs;
	gs_connector->helper_private = &gs_drm_connector_helper_funcs;
	drm_connector_helper_add(connector, &drm_connector_helper_funcs);

	/* Attach properties */
	ret = gs_panel_connector_attach_properties(ctx);
	if (ret) {
		dev_err(dev, "Error attaching connector properties (%d)\n", ret);
		return ret;
	}

	/* Reset, mark as connected */
	connector->funcs->reset(connector);
	connector->status = connector_status_connected;

	return 0;
}
