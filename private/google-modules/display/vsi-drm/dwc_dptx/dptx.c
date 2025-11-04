// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare DP Tx driver
 *
 */

#include <linux/media-bus-format.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include "dptx.h"
#if IS_ENABLED(CONFIG_DWC_DPTX_HDCP)
#include "hdcp.h"
#endif // CONFIG_DWC_DPTX_HDCP
#include "rst_mng.h"
#include "phy/phy_n621.h"
#include "regmaps/ctrl_fields.h"
#include "regmaps/regfields.h"
#include "intr.h"

#if IS_ENABLED(CONFIG_DWC_DPTX_AUDIO)
#include "dptx_audio/dptx_audio.h"
#endif

static struct dptx *handle;

struct dptx *dptx_get_handle(void)
{
	return handle;
}

struct dptx_clock {
	const char *id;
	const u64 init_rate;
	const u64 deinit_rate;
};

static struct dptx_clock dptx_pixel_clks[DPTX_NUM_PIXEL_CLKS] = {
	{"hsion_pll_dp_clk", 38400000, 38400000},
	{"hsion_pix_clk0", 2, 38400000},
	{"hsion_pix_clk1", 2, 38400000},
	{"dpu_pix_clk0", 2, 38400000},
	{"dpu_pix_clk1", 2, 38400000},
};

static int dptx_parse_clocks(struct dptx *dptx)
{
	int i;
	int ret;

	for (i = 0; i < DPTX_NUM_PIXEL_CLKS; i++)
		dptx->pixel_clks[i].id = dptx_pixel_clks[i].id;

	ret = devm_clk_bulk_get(dptx->dev, DPTX_NUM_PIXEL_CLKS, dptx->pixel_clks);
	if (ret)
		return ret;

	return 0;
}

static int dptx_dpphy_bringup(struct dptx *dptx)
{
	u32 dp_cnfg, dp_cnfg_mask;
	int ret;

	/* Start pclk off */
	ret = google_dpphy_set_maxpclk(dptx->dp_phy, 0, 0);
	if (ret)
		dptx_warn(dptx, "%s, set PIPE clk to 0 failed", __func__);

	/* a. Assert HPD plugin to PHY */
	dp_cnfg = DP_CONFIG_REG_DP_HPD_VAL(1) | DP_CONFIG_DP_ENCRYPTION_MODE_VAL (1);
	dp_cnfg_mask = DP_CONFIG_REG_DP_HPD_MASK | DP_CONFIG_DP_ENCRYPTION_MODE_MASK;
	google_dpphy_config_write(dptx->dp_phy, dp_cnfg_mask, dp_cnfg);

	/* b. Set upcs_pipe_config */
	google_dpphy_init_upcs_pipe_config(dptx->dp_phy);

	/* c. AUX power */
	google_dpphy_aux_powerup(dptx->dp_phy);

	/* d. Indicate refclk selected on pclk */
	ret = google_dpphy_set_maxpclk(dptx->dp_phy, 2, 2);
	if (ret)
		dptx_warn(dptx, "%s, set PIPE clk to 2 failed", __func__);

	/* e. Deassert typec_disable_ack */
	dptx_write_regfield(dptx, dptx->ctrl_fields->field_typec_disable_ack, 0);

	/* f. Indicate maxpclk has pll_clk/operational clock */
	ret = google_dpphy_set_maxpclk(dptx->dp_phy, 1, 1);
	if (ret)
		return ret;

	/* To-do: Find out if this is needed for DSC enable */
	if (0) {
		dp_cnfg = DP_CONFIG_DSC_ENABLE_0_VAL(1) | DP_CONFIG_DSC_ENABLE_1_VAL(1);
		dp_cnfg_mask = DP_CONFIG_DSC_ENABLE_0_MASK | DP_CONFIG_DSC_ENABLE_1_MASK;
		google_dpphy_config_write(dptx->dp_phy, dp_cnfg_mask, dp_cnfg);
	}

	/* g. Change MUX sel to sync powerdown input of PHY */
	google_dpphy_set_pipe_pclk_on(dptx->dp_phy, 1);

	return 0;
}

static void dptx_dpphy_teardown(struct dptx *dptx)
{
	google_dpphy_set_pipe_pclk_on(dptx->dp_phy, 0);
}

static enum hotplug_state dptx_get_hpd_state(struct dptx *dptx)
{
	enum hotplug_state hpd_current_state;

	mutex_lock(&dptx->hpd_state_lock);
	hpd_current_state = dptx->hpd_current_state;
	mutex_unlock(&dptx->hpd_state_lock);

	return hpd_current_state;
}

static void dptx_set_hpd_state(struct dptx *dptx, enum hotplug_state hpd_current_state)
{
	mutex_lock(&dptx->hpd_state_lock);
	dptx_info(dptx, "DP HPD changed to %d\n", hpd_current_state);
	dptx->hpd_current_state = hpd_current_state;
	mutex_unlock(&dptx->hpd_state_lock);
}

static bool dp_audio = true;
module_param(dp_audio, bool, 0664);
MODULE_PARM_DESC(dp_audio, "Enable/disable DP audio");

static void dptx_audio_notify(struct dptx *dptx, unsigned long state)
{
	if (!dp_audio) {
		dptx_info(dptx, "DP audio path is disabled: dp_audio=N\n");
		return;
	}

	if (!dptx->sink_has_pcm_audio)
		return;

	dptx_info(dptx, "call audio notifier (%s)\n",
			state == DPTX_AUDIO_CONNECT?"connect":"disconnect");
	blocking_notifier_call_chain(&dptx->audio_notifier_head, state, dptx->audio_notifier_data);
}

static void dptx_work_hpd(struct dptx *dptx, enum hotplug_state state)
{
	int ret;
	int i;
	u32 dp_cnfg, dp_cnfg_mask;
	u8 irq = 0;

	mutex_lock(&dptx->hpd_lock);
	mutex_lock(&dptx->mutex);

	if (state == HPD_PLUG) {
		dptx_info(dptx, "[HPD_PLUG start]\n");

		device_init_wakeup(dptx->dev, true);
		pm_stay_awake(dptx->dev);

		/* Enable dpu power domain for clock enable */
		ret = pm_runtime_resume_and_get(dptx->pd_dev[SSWRP_DPU_PD]);
		if (ret) {
			dptx_err(dptx, "[HPD_PLUG] DPU: PM get failed (%d)\n", ret);
			goto hpd_plug_fail_dpu_pm;
		}

		/* Enable HSIO_N DP clocks */
		ret = clk_bulk_prepare_enable(DPTX_NUM_PIXEL_CLKS, dptx->pixel_clks);
		if (ret) {
			dptx_err(dptx, "clk prepare enable failed\n");
			goto hpd_plug_fail_clk;
		}

		/* Set initial clock rates */
		for (i = 0; i < DPTX_NUM_PIXEL_CLKS; i++)
			clk_set_rate(dptx->pixel_clks[i].clk, dptx_pixel_clks[i].init_rate);

		/*
		 * Enable DP_TOP power domain
		 * See CPM function: hsio_n_psm5_p1_p0_trans()
		 */
		ret = pm_runtime_resume_and_get(dptx->pd_dev[HSION_DP_PD]);
		if (ret) {
			dptx_err(dptx, "[HPD_PLUG] HSION: PM get failed (%d)\n", ret);
			goto hpd_plug_fail_pm;
		}

		/* Enable DPU_DP power domain */
		ret = pm_runtime_resume_and_get(dptx->pd_dev[DPU_DP_PD]);
		if (ret) {
			dptx_err(dptx, "[HPD_PLUG] DPU_PD: PM get failed (%d)\n", ret);
			goto hpd_plug_fail_pm_dpu_dp;
		}

		ret = dptx_core_init(dptx);
		if (ret)
			dptx_err(dptx, "dptx_core_init() failed\n");

		/* Assert typec_disable_ack to switch into DP Alt Mode */
		dptx_write_regfield(dptx, dptx->ctrl_fields->field_typec_disable_ack, 1);

		/* Combo PHY init */
		ret = phy_init(dptx->dp_phy);
		if (ret) {
			dptx_err(dptx, "dp phy_init() failed\n");
			goto hpd_plug_fail_phy;
		}

		ret = dptx_dpphy_bringup(dptx);
		if (ret) {
			dptx_err(dptx, "dp phy_bringup() failed (%d)\n", ret);
			goto hpd_plug_fail_bringup;
		}

		ret = handle_hotplug(dptx);
		if (ret)
			dptx_err(dptx, "handle_hotplug() failed\n");

		/* check for automated test request and schedule HPD_IRQ to handle it */
		if (dptx->rx_caps[0] <= DP_DPCD_REV_12)
			dptx_read_dpcd(dptx, DP_DEVICE_SERVICE_IRQ_VECTOR, &irq);
		else
			dptx_read_dpcd(dptx, DP_DEVICE_SERVICE_IRQ_VECTOR_ESI0, &irq);

		if (irq & DP_AUTOMATED_TEST_REQUEST)
			queue_work(dptx->dp_wq, &dptx->hpd_irq_work);

		dptx_info(dptx, "[HPD_PLUG done]\n");
	} else if (state == HPD_UNPLUG) {
		dptx_info(dptx, "[HPD_UNPLUG start]\n");

		if (!pm_runtime_get_if_in_use(dptx->pd_dev[HSION_DP_PD])) {
			dptx_info(dptx, "DPTX is already powered off\n");
			goto hpd_unplug_done;
		}

		ret = handle_hotunplug(dptx);
		if (ret)
			dptx_err(dptx, "handle_hotunplug() failed\n");

		dptx_dpphy_teardown(dptx);

		/* Assert typec_disable_ack to switch out of DP Alt Mode */
		dptx_write_regfield(dptx, dptx->ctrl_fields->field_typec_disable_ack, 1);

		phy_exit(dptx->dp_phy);

		/* Decrement DP_TOP ref count to match pm_runtime_get_if_in_use() above */
		ret = pm_runtime_put_sync(dptx->pd_dev[HSION_DP_PD]);
		if (ret)
			dptx_err(dptx, "[HPD_UNPLUG] HSION: PM put failed (%d)\n", ret);

		/* Disable DPU_DP power domain */
		ret = pm_runtime_put_sync(dptx->pd_dev[DPU_DP_PD]);
		if (ret)
			dptx_err(dptx, "[HPD_UNPLUG] DPU_PD: PM put failed (%d)\n", ret);

		/*
		 * Disable DP_TOP power domain
		 * See CPM function: hsio_n_psm5_p0_p1_trans()
		 */
		ret = pm_runtime_put_sync(dptx->pd_dev[HSION_DP_PD]);
		if (ret)
			dptx_err(dptx, "[HPD_UNPLUG] HSION: PM put failed (%d)\n", ret);

		/* Set deinit clock rates, so that next hotplug will force clock rate change */
		for (i = 0; i < DPTX_NUM_PIXEL_CLKS; i++)
			clk_set_rate(dptx->pixel_clks[i].clk, dptx_pixel_clks[i].deinit_rate);

		/* Disable HSIO_N DP clocks */
		clk_bulk_disable_unprepare(DPTX_NUM_PIXEL_CLKS, dptx->pixel_clks);

		/*
		 * Release vote on SSWRP_DPU_PD
		 */
		ret = pm_runtime_put_sync(dptx->pd_dev[SSWRP_DPU_PD]);
		if (ret)
			dptx_err(dptx, "[HPD_UNPLUG] DPU: PM put failed (%d)\n", ret);

		pm_relax(dptx->dev);
		device_init_wakeup(dptx->dev, false);

hpd_unplug_done:
		dptx_info(dptx, "[HPD_UNPLUG done]\n");
	}

	mutex_unlock(&dptx->mutex);
	mutex_unlock(&dptx->hpd_lock);
	return;

hpd_plug_fail_bringup:
	phy_exit(dptx->dp_phy);
hpd_plug_fail_phy:
	ret = pm_runtime_put_sync(dptx->pd_dev[DPU_DP_PD]);
	if (ret)
		dptx_err(dptx, "[HPD_PLUG fail] DPU: PM put failed (%d)\n", ret);
hpd_plug_fail_pm_dpu_dp:
	ret = pm_runtime_put_sync(dptx->pd_dev[HSION_DP_PD]);
	if (ret)
		dptx_err(dptx, "[HPD_PLUG fail] HSION: PM put failed (%d)\n", ret);
hpd_plug_fail_pm:
	for (i = 0; i < DPTX_NUM_PIXEL_CLKS; i++)
		clk_set_rate(dptx->pixel_clks[i].clk, dptx_pixel_clks[i].deinit_rate);
	clk_bulk_disable_unprepare(DPTX_NUM_PIXEL_CLKS, dptx->pixel_clks);
hpd_plug_fail_clk:
	ret = pm_runtime_put_sync(dptx->pd_dev[SSWRP_DPU_PD]);
	if (ret)
		dptx_err(dptx, "[HPD_PLUG fail] DPU: PM put failed (%d)\n", ret);
hpd_plug_fail_dpu_pm:
	pm_relax(dptx->dev);
	device_init_wakeup(dptx->dev, false);
	dptx_set_hpd_state(dptx, HPD_UNPLUG);
	dptx_info(dptx, "[HPD_PLUG fail done]\n");
	mutex_unlock(&dptx->mutex);
	mutex_unlock(&dptx->hpd_lock);
}

static void dptx_work_hpd_plug(struct work_struct *work)
{
	struct dptx *dptx = container_of(work, struct dptx, hpd_plug_work);

	dptx_work_hpd(dptx, HPD_PLUG);
}

static void dptx_work_hpd_unplug(struct work_struct *work)
{
	struct dptx *dptx = container_of(work, struct dptx, hpd_unplug_work);

	dptx_work_hpd(dptx, HPD_UNPLUG);
}

static void dptx_work_hpd_irq(struct work_struct *work)
{
	struct dptx *dptx = container_of(work, struct dptx, hpd_irq_work);

	mutex_lock(&dptx->hpd_lock);
	mutex_lock(&dptx->mutex);
	dptx_info(dptx, "[HPD_IRQ start]\n");

	if (pm_runtime_get_if_in_use(dptx->pd_dev[HSION_DP_PD])) {
		handle_sink_request(dptx);
		pm_runtime_put(dptx->pd_dev[HSION_DP_PD]);
	} else {
		dptx_info(dptx, "DPTX is already powered off\n");
	}

	dptx_info(dptx, "[HPD_IRQ done]\n");
	mutex_unlock(&dptx->mutex);
	mutex_unlock(&dptx->hpd_lock);
}

void dptx_update_link_status(struct dptx *dptx, enum link_training_status link_status)
{
	if (link_status != dptx->typec_link_training_status) {
		dptx->typec_link_training_status = link_status;
		sysfs_notify(&dptx->dev->kobj, "drm-displayport", "link_status");
	}
}

static void dptx_hpd_changed(struct dptx *dptx, enum hotplug_state state)
{
	if (dptx_get_hpd_state(dptx) == state) {
		dptx_dbg(dptx, "DP HPD is same state (%x): Skip\n", state);
		return;
	}

	if (state == HPD_PLUG) {
		dptx_set_hpd_state(dptx, state);
		if (!queue_work(dptx->dp_wq, &dptx->hpd_plug_work))
			dptx_warn(dptx, "DP HPD PLUG work was already queued\n");
	} else if (state == HPD_UNPLUG) {
		dptx_set_hpd_state(dptx, state);
		if (!queue_work(dptx->dp_wq, &dptx->hpd_unplug_work))
			dptx_warn(dptx, "DP HPD UNPLUG work was already queued\n");
	} else
		dptx_err(dptx, "DP HPD changed to abnormal state(%d)\n", state);
}

static bool dp_enabled;
module_param(dp_enabled, bool, 0664);
MODULE_PARM_DESC(dp_enabled, "Enable/disable DP notification processing");

static int dptx_usb_typec_dp_notification_locked(struct dptx *dptx, enum hotplug_state hpd)
{
	if (!dp_enabled) {
		dptx_info(dptx, "%s: DP is disabled, ignoring DP notifications\n", __func__);
		return NOTIFY_OK;
	}

	if (hpd == HPD_PLUG) {
		if (dptx_get_hpd_state(dptx) == HPD_UNPLUG) {
			dptx_info(dptx, "%s: USB Type-C is HPD PLUG status\n", __func__);

			memset(&dptx->hw_config, 0, sizeof(struct dp_hw_config));
			dptx_info(dptx, "%s: disp_orientation = %d\n", __func__,
				  dptx->typec_orientation);
			dptx->hw_config.orient_type = dptx->typec_orientation;

			dptx_info(dptx, "%s: disp_pin_config = %d\n", __func__,
				  dptx->typec_pin_assignment);
			dptx->hw_config.pin_type = dptx->typec_pin_assignment;
			dptx_hpd_changed(dptx, HPD_PLUG);
		}
	} else if (hpd == HPD_IRQ) {
		if (dptx_get_hpd_state(dptx) == HPD_PLUG) {
			dptx_info(dptx, "%s: Service IRQ from sink\n", __func__);
			if (!queue_work(dptx->dp_wq, &dptx->hpd_irq_work))
				dptx_warn(dptx, "DP HPD IRQ work was already queued\n");
		}
	} else {
		dptx_info(dptx,
			  "%s: USB Type-C is HPD UNPLUG status, or not in display ALT mode\n",
			  __func__);

		if (dptx_get_hpd_state(dptx) == HPD_PLUG)
			dptx_hpd_changed(dptx, HPD_UNPLUG);

		/* Mark unknown on HPD UNPLUG */
		dptx_update_link_status(dptx, LINK_TRAINING_UNKNOWN);
	}

	return NOTIFY_OK;
}

void dptx_notify(struct dptx *dptx)
{
	wake_up_interruptible(&dptx->waitq);
}

void dptx_notify_shutdown(struct dptx *dptx)
{
	atomic_set(&dptx->shutdown, 1);
	dptx_notify(dptx);
}

/*
 * DRM bridge functions
 */
static int dptx_bridge_atomic_check(struct drm_bridge *br, struct drm_bridge_state *br_s,
				    struct drm_crtc_state *cr_s, struct drm_connector_state *co_s)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);
	dptx->connector = co_s->connector;

	dptx_dbg_bridge(dptx, "%s: e=%d a=%d a_ch=%d m_ch=%d c_ch=%d\n",
			__func__, cr_s->enable, cr_s->active, cr_s->active_changed,
			cr_s->mode_changed, cr_s->connectors_changed);

	/* if HPD_UNPLUG is pending, do not enable video or allow mode changes */
	if (cr_s->active && drm_atomic_crtc_needs_modeset(cr_s) &&
	    dptx_get_hpd_state(dptx) == HPD_UNPLUG) {
		dptx_dbg_bridge(dptx, "ATOMIC CHECK: HPD_UNPLUG is pending");
		return -ENOTCONN;
	}

	return 0;
}

void dptx_video_disable(struct dptx *dptx)
{
	/* Disable DP audio */
	dptx_audio_notify(dptx, DPTX_AUDIO_DISCONNECT);

#if IS_ENABLED(CONFIG_DWC_DPTX_HDCP)
	/* Disable HDCP */
	dptx_hdcp_disconnect(dptx);
#endif // CONFIG_DWC_DPTX_HDCP

	/* Disable video */
	dptx_disable_default_video_stream(dptx, 0);

	dptx->video_enabled = false;
}

static void dptx_bridge_atomic_disable(struct drm_bridge *br, struct drm_bridge_state *old_br_s)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	mutex_lock(&dptx->mutex);

	dptx_dbg_bridge(dptx, "%s\n", __func__);

	dptx_video_disable(dptx);

	/* signal completion to handle_hotunplug_core() */
	complete(&dptx->video_disable_done);

	mutex_unlock(&dptx->mutex);
}

static void dptx_bridge_atomic_enable(struct drm_bridge *br, struct drm_bridge_state *old_br_s)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);
	struct video_params *vparams = &dptx->vparams;
	struct audio_params *aparams = &dptx->aparams;
	struct drm_atomic_state *old_s = old_br_s->base.state;
	struct drm_crtc *crtc = drm_atomic_get_new_crtc_for_encoder(old_s, br->encoder);
	struct drm_crtc_state *crtc_s = drm_atomic_get_new_crtc_state(old_s, crtc);
	struct drm_display_mode *adj_m = &crtc_s->adjusted_mode;

	mutex_lock(&dptx->mutex);

	dptx_dbg_bridge(dptx, "ENABLE MODE: " DRM_MODE_FMT "\n", DRM_MODE_ARG(adj_m));

	if (drm_mode_is_420_only(&dptx->connector->display_info, adj_m))
		dptx_dbg_bridge(dptx, "Mode is YCbCr 4:2:0 only\n");

	/* Power on the sink */
	dptx_write_dpcd(dptx, DP_SET_POWER, DP_SET_POWER_D0);

	drm_mode_copy(&dptx->current_mode, adj_m);
	dptx_video_params_reset(dptx);

	/* Use 1920x1080@60 DMT mode, refer to dptx_dtd_fill() */
	vparams->mode = 82;
	vparams->refresh_rate = 60000;
	vparams->video_format = DMT;
	vparams->bpc = dptx->link.bpc;
	vparams->pix_enc = RGB;

	/* Mute audio for now */
	aparams->mute = 1;

	/*
	 * Adjust PLL_DP rate to 1/2 of pixel clock
	 *
	 * Support for 1/2 of pixel clock rate must be present in CPM and DT.
	 * dptx_bridge_mode_valid() ensures that non-supported modes are filtered out.
	 */
	clk_set_rate(dptx->pixel_clks[0].clk, dptx->current_mode.clock * 1000 / 2);
	dptx_dbg_bridge(dptx, "%s: %s rate = %lu\n", __func__, dptx->pixel_clks[0].id,
			clk_get_rate(dptx->pixel_clks[0].clk));

	/*
	 * Adjust HSIO_N DP0 pixel clock to use divisor 2 to get 1/4 of pixel clock rate
	 */
	clk_set_rate(dptx->pixel_clks[1].clk, 2);
	dptx_dbg_bridge(dptx, "%s: %s rate = %lu\n", __func__, dptx->pixel_clks[1].id,
			clk_get_rate(dptx->pixel_clks[1].clk));

	/*
	 * Adjust DPU DP0 pixel clock to use divisor 2 to get 1/4 of pixel clock rate
	 */
	clk_set_rate(dptx->pixel_clks[3].clk, 2);
	dptx_dbg_bridge(dptx, "%s: %s rate = %lu\n", __func__, dptx->pixel_clks[3].id,
			clk_get_rate(dptx->pixel_clks[3].clk));

	/* Initiate video mode change */
	dptx_video_mode_change(dptx, vparams->mode, 0);

#if IS_ENABLED(CONFIG_DWC_DPTX_HDCP)
	/* Initiate HDCP */
	dptx_hdcp_connect(dptx, 1);
#endif // CONFIG_DWC_DPTX_HDCP

	/* Initiate DP audio */
	dptx_audio_notify(dptx, DPTX_AUDIO_CONNECT);

	dptx->video_enabled = true;

	mutex_unlock(&dptx->mutex);
}

static int dptx_bridge_attach(struct drm_bridge *br, enum drm_bridge_attach_flags flags)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	dptx_dbg_bridge(dptx, "%s\n", __func__);

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		dptx_err(dptx, "DPTX does not provide drm_connector\n");
		return -EINVAL;
	}

	return 0;
}

static void dptx_bridge_detach(struct drm_bridge *br)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	dptx_dbg_bridge(dptx, "%s\n", __func__);
}

static enum drm_connector_status dptx_bridge_detect(struct drm_bridge *br)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	dptx_dbg_bridge(dptx, "%s\n", __func__);
	return (dptx_get_hpd_state(dptx) == HPD_PLUG && dptx->link.trained) ?
		connector_status_connected : connector_status_disconnected;
}

static const struct drm_edid *dptx_bridge_edid_read(struct drm_bridge *br, struct drm_connector *co)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	dptx_dbg_bridge(dptx, "%s\n", __func__);

	co->ycbcr_420_allowed = dptx->ycbcr_420_en;

	return (dptx_get_hpd_state(dptx) == HPD_PLUG && dptx->link.trained) ?
		drm_edid_alloc(dptx->edid, dptx->edid_size) : NULL;
}

static struct edid *dptx_bridge_get_edid(struct drm_bridge *br, struct drm_connector *co)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);

	dptx_dbg_bridge(dptx, "%s\n", __func__);

	co->ycbcr_420_allowed = dptx->ycbcr_420_en;

	return (dptx_get_hpd_state(dptx) == HPD_PLUG && dptx->link.trained) ?
		kmemdup(dptx->edid, dptx->edid_size, GFP_KERNEL) : NULL;
}

struct dptx_allowed_mode {
	const u16 hdisplay;
	const u16 vdisplay;
	const u32 clock;
};

static struct dptx_allowed_mode dptx_allowed_modes[] = {
	{3840, 2160, 594000},  /* CEA-861 */
	{3840, 2160, 533250},  /* CVT-RB */
	{2560, 1600, 348500},  /* DMT 0x4D */
	{2560, 1440, 241500},  /* CVT-RB */
	{1920, 1200, 193250},  /* DMT 0x45 */
	{1920, 1080, 148500},  /* DMT 0x52 | CEA-861 */
	{1600,  900, 108000},  /* DMT 0x53 */
	{1280,  720,  74250},  /* DMT 0x55 | CEA-861 */
	{1024,  768,  65000},  /* DMT 0x10 */
	{ 800,  600,  40000},  /* DMT 0x9 */
	{ 640,  480,  25175},  /* DMT 0x4 */
};

static unsigned int dp_max_bpc = 8;
module_param(dp_max_bpc, uint, 0664);
MODULE_PARM_DESC(dp_max_bpc, "DP link max BPC");

static enum drm_mode_status dptx_bridge_mode_valid(struct drm_bridge *br,
						   const struct drm_display_info *di,
						   const struct drm_display_mode *dm)
{
	struct dptx *dptx = container_of(br, struct dptx, bridge);
	u32 i;

	/* calculate link BPC: min(display max BPC, dp_max_bpc) */
	if (dptx->link.bpc == COLOR_DEPTH_INVALID) {
		dptx->link.bpc = min(di->bpc > 0 ? di->bpc : COLOR_DEPTH_8, dp_max_bpc);

		switch (dptx->link.bpc) {
		case COLOR_DEPTH_6:
			dptx->link.output_fmt = MEDIA_BUS_FMT_RGB666_1X18;
			break;
		case COLOR_DEPTH_8:
			dptx->link.output_fmt = MEDIA_BUS_FMT_RGB888_1X24;
			break;
		case COLOR_DEPTH_10:
			dptx->link.output_fmt = MEDIA_BUS_FMT_RGB101010_1X30;
			break;
		default:
			dptx->link.bpc = COLOR_DEPTH_8;
			dptx->link.output_fmt = MEDIA_BUS_FMT_RGB888_1X24;
			break;
		}

		dptx_dbg_bridge(dptx, "display BPC=%d, link BPC=%d\n", di->bpc, dptx->link.bpc);
	}

	/* DP link max data rate in Kbps */
	u32 link_rate = drm_dp_bw_code_to_link_rate(dptx_phy_rate_to_bw(dptx->link.rate));
	u32 link_data_rate = link_rate * dptx->link.lanes * 8;

	/* DRM display mode data rate in Kbps */
	u32 mode_data_rate = dm->clock * 3 * dptx->link.bpc;

	if (mode_data_rate > link_data_rate) {
		dptx_dbg_bridge(dptx, "DROP: " DRM_MODE_FMT "\n", DRM_MODE_ARG(dm));
		return MODE_CLOCK_HIGH;
	}

	if (drm_mode_vrefresh(dm) != 60) {
		dptx_dbg_bridge(dptx, "DROP: " DRM_MODE_FMT "\n", DRM_MODE_ARG(dm));
		return MODE_VSYNC;
	}

	for (i = 0; i < ARRAY_SIZE(dptx_allowed_modes); i++) {
		if (dm->hdisplay == dptx_allowed_modes[i].hdisplay &&
		    dm->vdisplay == dptx_allowed_modes[i].vdisplay &&
		    dm->clock == dptx_allowed_modes[i].clock) {
			dptx_dbg_bridge(dptx, "PICK: " DRM_MODE_FMT "\n", DRM_MODE_ARG(dm));
			return MODE_OK;
		}
	}

	dptx_dbg_bridge(dptx, "DROP: " DRM_MODE_FMT "\n", DRM_MODE_ARG(dm));
	return MODE_BAD;
}

static u32 *dptx_bridge_get_output_bus_formats(struct drm_bridge *bridge,
						struct drm_bridge_state *bridge_state,
						struct drm_crtc_state *crtc_state,
						struct drm_connector_state *conn_state,
						unsigned int *num_output_fmts)
{
	struct dptx *dptx = container_of(bridge, struct dptx, bridge);
	u32 *output_fmts;

	output_fmts = kmalloc(sizeof(*output_fmts), GFP_KERNEL);
	if (!output_fmts) {
		*num_output_fmts = 0;
		return NULL;
	}

	*num_output_fmts = 1;
	output_fmts[0] = dptx->link.output_fmt;
	return output_fmts;
}

static const struct drm_bridge_funcs dptx_bridge_funcs = {
	.atomic_check = dptx_bridge_atomic_check,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_post_disable = dptx_bridge_atomic_disable,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_enable = dptx_bridge_atomic_enable,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.attach = dptx_bridge_attach,
	.detach = dptx_bridge_detach,
	.detect = dptx_bridge_detect,
	.edid_read = dptx_bridge_edid_read,
	.get_edid = dptx_bridge_get_edid,
	.mode_valid = dptx_bridge_mode_valid,
	.atomic_get_input_bus_fmts = drm_atomic_helper_bridge_propagate_bus_fmt,
	.atomic_get_output_bus_fmts = dptx_bridge_get_output_bus_formats,
};

static int dptx_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct resource *res;
	struct dptx *dptx;
	int retval, i;

	dev = &pdev->dev;

	dptx = devm_kzalloc(dev, sizeof(*dptx), GFP_KERNEL);
	if (!dptx)
		return -ENOMEM;

	dptx->edid = kzalloc(DPTX_DEFAULT_EDID_BUFLEN, GFP_KERNEL);
	if (!dptx->edid)
		return -ENOMEM;

	dptx->edid_second = kzalloc(DPTX_DEFAULT_EDID_BUFLEN, GFP_KERNEL);
	if (!dptx->edid_second) {
		kfree(dptx->edid);
		return -ENOMEM;
	}

	// Update the device node
	dptx->dev = dev;

	/* Get Clks */
	retval = dptx_parse_clocks(dptx);
	if (retval) {
		dev_err(dev, "failed to parse clocks\n");
		goto fail_dt;
	}

	/* PHY Reference */
	dptx->dp_phy = devm_phy_get(dev, "dp-phy");
	if (IS_ERR(dptx->dp_phy)) {
		retval = PTR_ERR(dptx->dp_phy);
		dev_err(dev, "no DP PHY configured\n");
		goto fail_dt;
	}

	/* Get MEM resources */
	dptx->base[DPTX] = devm_platform_ioremap_resource_byname(pdev, "dptx_regs");
	if (IS_ERR(dptx->base[DPTX])) {
		retval = PTR_ERR(dptx->base[DPTX]);
		dev_err(dev, "no dptx_regs configured\n");
		goto fail_dt;
	}

	dptx->pd_dev[HSION_DP_PD] = dev_pm_domain_attach_by_name(dev, "dp_top_pd");
	if (IS_ERR_OR_NULL(dptx->pd_dev[HSION_DP_PD])) {
		dev_err(dev, "no dp_top_pd power domain\n");
		goto fail_pm;
	}

	dptx->pd_dev[DPU_DP_PD] = dev_pm_domain_attach_by_name(dev, "dpu_dp_pd");
	if (IS_ERR_OR_NULL(dptx->pd_dev[DPU_DP_PD])) {
		dev_err(dev, "no dpu_dp_pd power domain\n");
		goto fail_pm;
	}

	dptx->pd_dev[SSWRP_DPU_PD] = dev_pm_domain_attach_by_name(dev, "sswrp_dpu_pd");
	if (IS_ERR_OR_NULL(dptx->pd_dev[SSWRP_DPU_PD])) {
		dev_err(dev, "no sswrp_dpu_pd power domain\n");
		goto fail_pm;
	}

	retval = pm_runtime_resume_and_get(dptx->pd_dev[HSION_DP_PD]);
	if (retval) {
		dev_err(dev, "%s: pm_runtime_resume_and_get() returned %d\n", __func__, retval);
		goto fail_pm;
	}

	/* Initialize mutexes */
	mutex_init(&dptx->hpd_lock);
	mutex_init(&dptx->hpd_state_lock);
	mutex_init(&dptx->typec_notification_lock);

	/* Create workqueue and works for HPD */
	dptx->dp_wq = create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!dptx->dp_wq) {
		dptx_err(dptx, "DP workqueue creation failed\n");
		goto fail_wq;
	}

	INIT_WORK(&dptx->hpd_plug_work, dptx_work_hpd_plug);
	INIT_WORK(&dptx->hpd_unplug_work, dptx_work_hpd_unplug);
	INIT_WORK(&dptx->hpd_irq_work, dptx_work_hpd_irq);

	dptx->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	dptx->bridge.funcs = &dptx_bridge_funcs;
	dptx->bridge.of_node = dev->of_node;
	dptx->bridge.type = DRM_MODE_CONNECTOR_DisplayPort;
	devm_drm_bridge_add(dptx->dev, &dptx->bridge);

#if 0
	/* TODO: revisit DPTX MEM requirements */
	for (i = 0; i < MAX_MEM_IDX; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			dev_err(dev, "Failed to get memory resource %d\n", i);
			return -ENODEV;
		}

		dptx->base[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(dptx->base[i])) {
			dev_err(dev, "Failed to map memory resource\n");
			return PTR_ERR(dptx->base[i]);
		}
	}
#endif

	retval = init_regfields(dptx);
	if (retval) {
		goto fail;
	}

	if (!dptx_check_dptx_id(dptx)) {
		dev_err(dev, "DPTX_ID not match to 0x%04x:0x%04x\n",
			DPTX_ID_DEVICE_ID, DPTX_ID_VENDOR_ID);
		retval = -ENODEV;
		goto fail;
	}

#if 0
	/* TODO: revisit DPTX IRQ requirements */
	/* Get IRQ numbers from device */
	dev_info(dev, "Get IRQ numbers\n");
	for (i = 0; i < MAX_IRQ_IDX; i++) {
		dptx->irq[i] = platform_get_irq(pdev, i);
		if (dptx->irq[i] < 0)
			break;
		dev_info(dev, "IRQ number %d.\n", dptx->irq[i]);
	}
#endif

	dptx->cr_fail = false;
	dptx->mst = false; // Should be disabled for HDCP.
	dptx->ef_en = true;
	dptx->ssc_en = true;
	dptx->fec_en = false;
	dptx->dsc_en = false;
	dptx->streams = 2;
	dptx->multipixel = DPTX_MP_QUAD_PIXEL;
	dptx->dummy_dtds_present = false;
	dptx->selected_est_timing = NONE;

	init_completion(&dptx->video_disable_done);
	mutex_init(&dptx->mutex);
	dptx_video_params_reset(dptx);
	dptx_audio_params_reset(&dptx->aparams);
	init_waitqueue_head(&dptx->waitq);
	atomic_set(&dptx->sink_request, 0);
	atomic_set(&dptx->shutdown, 0);
	atomic_set(&dptx->c_connect, 0);

	dptx->max_rate = DPTX_DEFAULT_LINK_RATE;
	dptx->max_lanes = DPTX_DEFAULT_LINK_LANES;
	dptx->bstatus = 0;
	dptx->link_test_mode = false;
	dptx->ycbcr_420_en = true;

	platform_set_drvdata(pdev, dptx);
	dev_set_drvdata(dev, dptx);

	//TODO: rst_avp(dptx);

	dptx_global_intr_dis(dptx);

	dptx_debugfs_init(dptx);

	dptx_init_hwparams(dptx);

	retval = dptx_core_init(dptx);
	if (retval)
		goto fail;

#if IS_ENABLED(CONFIG_DWC_DPTX_HDCP)
	retval = dptx_hdcp_probe(dptx);
	if (retval)
		goto fail;
#endif // CONFIG_DWC_DPTX_HDCP

	dptx_audio_config(dptx);
	dptx_video_config(dptx, 0);

	init_completion(&dptx->audio_disable_done);
	BLOCKING_INIT_NOTIFIER_HEAD(&dptx->audio_notifier_head);

	//TODO: phy_n621_power_up(dptx);

	//Add controller reset
	//TODO: rst_dptx_ctrl(dptx);
#if 0
	retval = dptx_core_init(dptx);
	if (retval)
		goto fail;
#endif

#if 0
	/* TODO: revisit DPTX IRQ requirements */
	retval = devm_request_threaded_irq(dptx->dev,
					   dptx->irq[MAIN_IRQ],
					   dptx_irq,
					   dptx_threaded_irq,
					   IRQF_SHARED | IRQ_LEVEL,
					   "dwc_dptx_main_handler",
					   dptx);
	if (retval) {
		dev_err(dev, "Request for irq %d failed\n", dptx->irq[MAIN_IRQ]);
		return retval;
	}
#endif

	retval = pm_runtime_put_sync(dptx->pd_dev[HSION_DP_PD]);
	if (retval)
		dev_err(dev, "%s: pm_runtime_put_sync() returned %d\n", __func__, retval);
	handle = dptx;

#if IS_ENABLED(CONFIG_DWC_DPTX_AUDIO)
	dptx_audio_register();
#endif
	return 0;

fail:
	dptx_debugfs_exit(dptx);
	destroy_workqueue(dptx->dp_wq);
fail_wq:
	pm_runtime_put_sync(dptx->pd_dev[HSION_DP_PD]);
fail_pm:
	if (!IS_ERR_OR_NULL(dptx->pd_dev[SSWRP_DPU_PD]))
		dev_pm_domain_detach(dptx->pd_dev[SSWRP_DPU_PD], false);
	if (!IS_ERR_OR_NULL(dptx->pd_dev[DPU_DP_PD]))
		dev_pm_domain_detach(dptx->pd_dev[DPU_DP_PD], false);
	if (!IS_ERR_OR_NULL(dptx->pd_dev[HSION_DP_PD]))
		dev_pm_domain_detach(dptx->pd_dev[HSION_DP_PD], false);
fail_dt:
	kfree(dptx->edid);
	kfree(dptx->edid_second);
	return retval;
}

static int dptx_remove(struct platform_device *pdev)
{
	struct dptx *dptx = platform_get_drvdata(pdev);

#if IS_ENABLED(CONFIG_DWC_DPTX_AUDIO)
	dptx_audio_unregister();
#endif

	dptx_notify_shutdown(dptx);
	msleep(20);
	kfree(dptx->edid);
	if (dptx->edid_second)
		kfree(dptx->edid_second);
	dptx_core_deinit(dptx);
	dptx_debugfs_exit(dptx);
	cancel_work_sync(&dptx->hpd_plug_work);
	cancel_work_sync(&dptx->hpd_unplug_work);
	cancel_work_sync(&dptx->hpd_irq_work);
	destroy_workqueue(dptx->dp_wq);
#if IS_ENABLED(CONFIG_DWC_DPTX_HDCP)
	dptx_hdcp_remove(dptx);
#endif // CONFIG_DWC_DPTX_HDCP
	dev_pm_domain_detach(dptx->pd_dev[SSWRP_DPU_PD], false);
	dev_pm_domain_detach(dptx->pd_dev[DPU_DP_PD], false);
	dev_pm_domain_detach(dptx->pd_dev[HSION_DP_PD], false);
	return 0;
}

static const struct of_device_id dptx_driver_dt_match[] = {
	{ .compatible = "google,dwc_dptx" },
	{},
};

MODULE_DEVICE_TABLE(of, dptx_driver_dt_match);

static const char *const orientations[] = {
	[PLUG_NONE] = "unknown",
	[PLUG_NORMAL] = "normal",
	[PLUG_FLIPPED] = "reverse",
};

static ssize_t orientation_store(struct device *dev, struct device_attribute *attr, const char *buf,
				 size_t size)
{
	int orientation;
	struct dptx *dptx = dev_get_drvdata(dev);

	orientation = sysfs_match_string(orientations, buf);
	if (orientation < 0)
		return orientation;

	dptx->typec_orientation = (enum plug_orientation)orientation;
	return size;
}

static ssize_t orientation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dptx->typec_orientation);
}
static DEVICE_ATTR_RW(orientation);

static const char *const pin_assignments[] = {
	[PIN_TYPE_A] = "A", [PIN_TYPE_B] = "B", [PIN_TYPE_C] = "C", [PIN_TYPE_D] = "D",
	[PIN_TYPE_E] = "E", [PIN_TYPE_F] = "F",
};

static ssize_t pin_assignment_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t size)
{
	int pin_assign;
	struct dptx *dptx = dev_get_drvdata(dev);

	pin_assign = sysfs_match_string(pin_assignments, buf);

	dptx->typec_pin_assignment = (enum pin_assignment)pin_assign;
	return size;
}

static ssize_t pin_assignment_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dptx->typec_pin_assignment);
}
static DEVICE_ATTR_RW(pin_assignment);

static const char *const hpds[] = {
	[HPD_UNPLUG] = "0",
	[HPD_PLUG] = "1",
};

static ssize_t hpd_store(struct device *dev, struct device_attribute *attr, const char *buf,
			 size_t size)
{
	int hpd;
	struct dptx *dptx = dev_get_drvdata(dev);

	hpd = sysfs_match_string(hpds, buf);
	if (hpd < 0)
		return hpd;

	mutex_lock(&dptx->typec_notification_lock);
	dptx_usb_typec_dp_notification_locked(dptx, (enum hotplug_state)hpd);
	mutex_unlock(&dptx->typec_notification_lock);
	return size;
}

static ssize_t hpd_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dptx->hpd_current_state);
	// return sysfs_emit(buf, "%d\n", dp_get_hpd_state(dptx));
}
static DEVICE_ATTR_RW(hpd);

static ssize_t link_lanes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n",
			  dptx->typec_link_training_status == LINK_TRAINING_SUCCESS ?
			  dptx->link.lanes : -1);
}
static DEVICE_ATTR_RO(link_lanes);

static ssize_t link_rate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n",
			  dptx->typec_link_training_status == LINK_TRAINING_SUCCESS ?
			  dptx->link.rate : -1);
}
static DEVICE_ATTR_RO(link_rate);

static ssize_t link_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dptx->typec_link_training_status);
}
static DEVICE_ATTR_RO(link_status);

static ssize_t irq_hpd_store(struct device *dev, struct device_attribute *attr, const char *buf,
			     size_t size)
{
	struct dptx *dptx = dev_get_drvdata(dev);

	mutex_lock(&dptx->typec_notification_lock);
	dptx_usb_typec_dp_notification_locked(dptx, HPD_IRQ);
	mutex_unlock(&dptx->typec_notification_lock);
	return size;
}
static DEVICE_ATTR_WO(irq_hpd);

static ssize_t usbc_cable_disconnect_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t size)
{
	return size;
}
static DEVICE_ATTR_WO(usbc_cable_disconnect);

static struct attribute *dptx_usbhal_attrs[] = {
	&dev_attr_orientation.attr,
	&dev_attr_pin_assignment.attr,
	&dev_attr_hpd.attr,
	&dev_attr_link_lanes.attr,
	&dev_attr_link_rate.attr,
	&dev_attr_link_status.attr,
	&dev_attr_irq_hpd.attr,
	&dev_attr_usbc_cable_disconnect.attr,
	NULL
};

static const struct attribute_group dptx_usbhal_group = {
	.name = "drm-displayport",
	.attrs = dptx_usbhal_attrs
};

static const struct attribute_group *dptx_groups[] = {
	&dptx_usbhal_group,
	NULL
};

static struct platform_driver dptx_driver = {
	.probe		= dptx_probe,
	.remove		= dptx_remove,
	.driver		= {
		.name	= "dwc_dptx",
		.of_match_table = of_match_ptr(dptx_driver_dt_match),
		.dev_groups = dptx_groups,
	},
};

module_platform_driver(dptx_driver);

MODULE_AUTHOR("Synopsys, Inc");
MODULE_AUTHOR("Petri Gynther <pgynther@google.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Synopsys DesignWare DisplayPort TX Driver");
