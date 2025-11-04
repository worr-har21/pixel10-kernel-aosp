// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */
#include <interconnect/google_icc_helper.h>

#include <linux/align.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/units.h>

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
#include <linux/regmap.h>
#endif

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/vs_drm.h>

#include <gs_drm/gs_drm_connector.h>
#include <trace/dpu_trace.h>

#include "vs_crtc.h"
#include "vs_dc.h"
#include "vs_dc_pre.h"
#include "vs_dc_post.h"
#include "vs_dc_debugfs.h"
#include "vs_dc_hw.h"
#include "vs_drv.h"
#include "vs_dc_info.h"
#include "vs_writeback.h"
#include "vs_dc_sram.h"
#include "vs_dc_hw.h"
#include "vs_gem.h"
#include "display_compress/vs_dc_dsc.h"
#include "vs_trace.h"
#include "vs_trace_9x00.h"

#include <drm/vs_drm_fourcc.h>
#include <drm/drm_vblank.h>

#define VS_DC_AUTOSUPEND_DELAY_MS 30

static bool disable_crtc_recovery;
module_param(disable_crtc_recovery, bool, 0644);
MODULE_PARM_DESC(disable_crtc_recovery, "Disable crtc recovery on flip done timeout");

static bool disable_hw_reset;
module_param(disable_hw_reset, bool, 0644);
MODULE_PARM_DESC(disable_hw_reset, "Disable hardware reset before power OFF");

static bool disable_urgent;
module_param(disable_urgent, bool, 0644);
MODULE_PARM_DESC(disable_urgent, "Disable QoS urgent level feature");

int vs_dc_power_get(struct device *dev, bool sync)
{
	int ret;

	DPU_ATRACE_BEGIN(__func__);
	if (sync) {
		ret = pm_runtime_get_sync(dev);
	} else {
		ret = pm_runtime_get(dev);
		/* runtime_get might return -EINPROGRESS if there exists deferred runtime_get */
		if (ret == -EINPROGRESS) {
			dev_dbg(dev, "%s: %ps ignore return EINPROGRESS\n", __func__,
				__builtin_return_address(0));
		}
	}

	if (unlikely(ret < 0 && ret != -EINPROGRESS))
		dev_err(dev, "%s: %ps: sync:%d ret:%d\n", __func__, __builtin_return_address(0),
			sync, ret);

	trace_disp_dc_power_get(dev, sync, ret);
	dev_dbg(dev, "%s: %ps: sync:%d power_usage_count:%d\n", __func__,
		__builtin_return_address(0), sync, atomic_read(&dev->power.usage_count));
	DPU_ATRACE_END(__func__);

	return (ret == -EINPROGRESS) ? 0 : ret;
}

int vs_dc_power_put(struct device *dev, bool sync)
{
	int ret;

	DPU_ATRACE_BEGIN(__func__);
	if (sync)
		ret = pm_runtime_put_sync(dev);
	else
		ret = pm_runtime_put(dev);

	/* runtime_put might return -EAGAIN if there exists deferred runtime_get */
	if (ret == -EAGAIN) {
		dev_dbg(dev, "%s: %ps ignore return EAGAIN\n", __func__,
			__builtin_return_address(0));
	}

	if (unlikely(ret < 0 && ret != -EAGAIN))
		dev_err(dev, "%s: %ps: sync:%d ret:%d\n", __func__, __builtin_return_address(0),
			sync, ret);

	trace_disp_dc_power_put(dev, sync, ret);
	dev_dbg(dev, "%s: %ps: sync:%d power_usage_count:%d\n", __func__,
		__builtin_return_address(0), sync, atomic_read(&dev->power.usage_count));
	DPU_ATRACE_END(__func__);

	return (ret == -EAGAIN) ? 0 : ret;
}

bool is_display_cmd_sw_trigger(struct dc_hw_display *display)
{
	return (display->mode.output_mode & VS_OUTPUT_MODE_CMD) &&
	       !(display->mode.output_mode & VS_OUTPUT_MODE_CMD_AUTO);
}

static void vs_dc_enable_irqs(struct vs_dc *dc)
{
	int i;

	for (i = 0; i < dc->irq_num; i++)
		enable_irq(dc->irqs[i]);

	dc->irq_enable_count++;
	trace_disp_dc_enable_irqs(dc);
}

static void vs_dc_disable_irqs(struct vs_dc *dc)
{
	int i;

	for (i = 0; i < dc->irq_num; i++)
		disable_irq(dc->irqs[i]);

	dc->irq_enable_count--;
	trace_disp_dc_disable_irqs(dc);
}

static void vs_dc_do_hw_reset(struct device *dev)
{
	int i;
	bool needs_hw_reset = false;
	struct vs_dc *dc = dev_get_drvdata(dev);

	if (dc->disable_hw_reset)
		return;

	if (!dc_hw_fe_is_all_layers_idle(&dc->hw))
		needs_hw_reset = true;

	if (dc->hw.fe0_has_bus_errors) {
		dc->hw.fe0_has_bus_errors = false;
		dc_hw_do_fe0_reset(&dc->hw);
	}

	if (dc->hw.fe1_has_bus_errors) {
		dc->hw.fe1_has_bus_errors = false;
		dc_hw_do_fe1_reset(&dc->hw);
	}

	if (dc->hw.be_has_bus_errors) {
		dc->hw.be_has_bus_errors = false;
		dc_hw_do_be_reset(&dc->hw);
	}

	for (i = 0; i < DC_DISPLAY_NUM; i++) {
		if (!dc->crtc[i])
			continue;

		if (!dc->crtc[i]->needs_hw_reset)
			continue;

		needs_hw_reset = true;
		dc->crtc[i]->needs_hw_reset = false;
		dev_warn(dev, "%s CRTC-%d needs hardware reset\n", __func__, i);
	}

	if (needs_hw_reset) {
		dev_warn(dev, "%s triggering hardware reset\n", __func__);
		dc_hw_do_reset(&dc->hw);
	}
}

#if IS_ENABLED(CONFIG_PM_SLEEP)
static int vs_dc_res_disable(struct device *dev)
{
	struct vs_dc *dc = dev_get_drvdata(dev);

	mutex_lock(&dc->dc_lock);
	dev_dbg(dev, "%s\n", __func__);

	WARN_ON(!dc->enabled);
	if (!dc->enabled)
		goto end;

	dc_hw_save_status(&dc->hw);

	vs_dc_do_hw_reset(dev);
	dc_hw_reset_all_be_interrupts(&dc->hw);

	vs_qos_clear_qos_configs(dc);

	dc->enabled = false;

	trace_disp_dc_disable(dc);

end:
	mutex_unlock(&dc->dc_lock);

	return 0;
}

static int vs_dc_res_enable(struct device *dev)
{
	int ret = 0;
	struct vs_dc *dc = dev_get_drvdata(dev);

	mutex_lock(&dc->dc_lock);
	dev_dbg(dev, "%s\n", __func__);

	WARN_ON(dc->enabled);
	if (dc->enabled)
		goto end;

	dc->enabled = true;

	trace_disp_dc_enable(dc);

end:
	mutex_unlock(&dc->dc_lock);

	return ret;
}

static int vs_dc_pm_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);
	vs_dc_res_disable(dev);
	DPU_ATRACE_END(__func__);

	return 0;
}

static int vs_dc_pm_runtime_resume(struct device *dev)
{
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);
	ret = vs_dc_res_enable(dev);
	DPU_ATRACE_END(__func__);

	return ret;
}

static const struct dev_pm_ops vs_dc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(vs_dc_pm_runtime_suspend, vs_dc_pm_runtime_resume, NULL)
};
#endif

static void dc_deinit(struct device *dev)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	u32 i = 0;

	for (i = 0; i < dc->hw.info->display_num; i++)
		dc_hw_enable_vblank_irqs(&dc->hw, i, false);

	vs_dpu_sram_pools_deinit();
	dc_hw_deinit(&dc->hw);
}

static int dc_init(struct device *dev)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	int ret;

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
	dc->hw.regmap_config.name = "VerisiliconDRM";
	dc->hw.regmap_config.reg_bits = 32;
	dc->hw.regmap_config.val_bits = 32;
	dc->hw.regmap_config.reg_stride = 4;
	dc->hw.regmap_config.cache_type = REGCACHE_RBTREE;

	dc->hw.regmap = devm_regmap_init_mmio(dev, dc->hw.reg_base, &dc->hw.regmap_config);
	if (IS_ERR(dc->hw.regmap)) {
		dev_err(dev, "Error initialise mmio register map: %ld\n", PTR_ERR(dc->hw.regmap));
		return PTR_ERR(dc->hw.regmap);
	}
#endif

	dc->first_frame = true;

	dc->disable_hw_reset = disable_hw_reset;
	dc->hw_reg_dump_options = DC_HW_REG_DUMP_IN_NONE;
	dc->disable_crtc_recovery = disable_crtc_recovery;
	dc->disable_urgent = disable_urgent;

	dev_info(dev, "disable_hw_reset:%d, hw_reg_dump_options:%d disable_crtc_recovery:%d disable_urgent:%d\n",
		 dc->disable_hw_reset, dc->hw_reg_dump_options, dc->disable_crtc_recovery,
		 dc->disable_urgent);

	ret = dc_hw_init(&dc->hw);
	if (ret) {
		dev_err(dev, "failed to init DC HW\n");
		return ret;
	}

	/*SRAM POOL Init*/
	ret = vs_dpu_sram_pools_init(&dc->hw);
	if (ret) {
		dev_err(dev, "failed to init SRAM POOL\n");
		return ret;
	}

	return ret;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void vs_dc_get_plane_crc(struct vs_dc *dc, struct vs_plane *plane)
{
	struct device *dev = dc->hw.dev;
	struct vs_plane_state *plane_state = to_vs_plane_state(plane->base.state);
	struct vs_plane_info *plane_info;
	struct dc_hw_crc crc;

	dc_hw_get_plane_crc_config(&dc->hw, plane->id, &crc);
	plane_state->crc.enable = crc.enable;
	if (!crc.enable)
		return;

	plane_info = get_plane_info(plane->id, dc->hw.info);
	if (!plane_info) {
		dev_err(dev, "%s: invalid plane id: %d\n", __func__, plane->id);
		return;
	}

	if (!plane_info->crc) {
		dev_info(dev, "%s: vs_plane[%u] does not support crc.\n", __func__, plane->id);
		return;
	}

	plane_state->crc.pos = crc.pos;
	memcpy(&plane_state->crc.seed, &crc.seed, sizeof(crc.seed));

	dc_hw_get_plane_crc(&dc->hw, plane->id, &crc);
	memcpy(&plane_state->crc.result, &crc.result, sizeof(crc.result));
}

static void vs_dc_get_display_crc(struct vs_dc *dc, struct drm_crtc *crtc)
{
	struct device *dev = dc->hw.dev;
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);
	struct drm_crtc_state *state = vs_crtc->base.state;
	struct vs_crtc_state *crtc_state = to_vs_crtc_state(state);
	struct vs_display_info *display_info;
	u8 hw_id;

	struct dc_hw_disp_crc crc;

	display_info = (struct vs_display_info *)&dc->hw.info->displays[vs_crtc->id];
	if (!display_info) {
		dev_err(dev, "%s: Invalid vs_crtc index.\n", __func__);
		return;
	}

	if (!display_info->crc) {
		dev_info(dev, "%s: vs_crtc[%u] does not support crc.\n", __func__, vs_crtc->id);
		return;
	}

	if (crtc_state->crc.pos > VS_DISP_CRC_OFIFO_OUT) {
		dev_err(dev, "%s: Invalid crc pos.\n", __func__);
		return;
	}

	hw_id = display_info->id;
	dc_hw_get_display_crc_config(&dc->hw, hw_id, &crc);

	crtc_state->crc.enable = crc.enable;
	if (!crc.enable)
		return;

	crtc_state->crc.pos = crc.pos;
	memcpy(&crtc_state->crc.seed, &crc.seed, sizeof(crc.seed));

	dc_hw_get_display_crc(&dc->hw, hw_id, &crc);

	memcpy(&crtc_state->crc.result, &crc.result, sizeof(crc.result));

	if (crtc_state->crc.pos == VS_DISP_CRC_OFIFO_OUT)
		vs_crtc_set_last_crc(drm_crtc_index(crtc), crtc_state->crc.result[1]);
	else
		vs_crtc_set_last_crc(drm_crtc_index(crtc), crtc_state->crc.result[0]);
}
#endif /* CONFIG_DEBUG_FS */

static void vs_dc_underrun_workaround(struct vs_dc *dc, struct drm_crtc *crtc,
				      struct dc_hw_display *display, u8 display_id, bool enable)
{
	struct vs_crtc *vs_crtc = to_vs_crtc(crtc);

	if (!(display->mode.output_mode & VS_OUTPUT_MODE_CMD) ||
	    (display->mode.output_mode & VS_OUTPUT_MODE_CMD_AUTO))
		return;

	if (vs_crtc->underrun_enabled != enable) {
		dc_hw_clear_underrun_interrupt(&dc->hw, display_id);
		dc_hw_enable_underrun_interrupt(&dc->hw, display_id, enable);
		vs_crtc->underrun_enabled = enable;
		DPU_ATRACE_INT_PID_FMT(enable, vs_crtc->trace_pid, "underrun_en[%d]", display_id);
	}
}

static void _vs_dc_handle_underrun(struct vs_dc *dc, struct vs_crtc *vs_crtc, u8 display_id,
				   bool cmd_mode_start_scan_now)
{
	struct device *dev = dc->hw.dev;
	struct dc_hw_display *display = &dc->hw.display[vs_crtc->id];

	if (!vs_crtc->frame_transfer_pending) {
		dev_dbg(dev, "display[%d] underrun false positive, no transfer, mode:%#x\n",
			display_id, display->mode.output_mode);
		return;
	}

	if (is_display_cmd_sw_trigger(display) &&
	    (!vs_crtc->ddic_cmd_mode_start_scan || cmd_mode_start_scan_now)) {
		dev_dbg(dev, "display[%d] underrun false positive, %s, mode:%#x\n", display_id,
			(!vs_crtc->ddic_cmd_mode_start_scan) ? "no panel scan out yet" :
							       "panel just start scan out",
			display->mode.output_mode);
		return;
	}

	trace_disp_dpu_underrun(display_id, atomic_read(&vs_crtc->frames_pending),
				atomic_read(&vs_crtc->te_count));
	if (is_display_cmd_sw_trigger(display))
		dev_dbg(dev, "display[%d] underrun detected, mode:%#x (te count %d)\n", display_id,
			display->mode.output_mode, atomic_read(&vs_crtc->te_count));
	else
		dev_warn(dev, "display[%d] underrun detected, mode:%#x (te count %d)\n", display_id,
			 display->mode.output_mode, atomic_read(&vs_crtc->te_count));
}

static void vs_dc_handle_interrupts(struct vs_dc *dc)
{
	struct device *dev = dc->hw.dev;
	const struct vs_dc_info *dc_info = dc->hw.info;
	const struct vs_wb_info *wb_info = dc_info->write_back;
	u32 i;
	struct dc_hw_interrupt_status status = { 0 };

	dc_hw_get_interrupt(&dc->hw, &status);

	dev_dbg(dev,
		"%s: te_r=%#x te_f=%#x frm_start=%#x layer_done=%x frm_done=%#x wb_frm_done=%#x\n",
		__func__, status.display_te_rising, status.display_te_falling,
		status.display_frm_start, status.layer_frm_done, status.display_frm_done,
		status.wb_frm_done);

	dev_dbg(dev, "%s: layer_rst_done=%#x fe0_rst_done=%d fe1_rst_done=%d be_reset_done=%d\n",
		__func__, status.layer_reset_done, status.reset_status[FE0_SW_RESET],
		status.reset_status[FE1_SW_RESET], status.reset_status[BE_SW_RESET]);

	if (!bitmap_empty(status.fe0_bus_errors, DC_HW_FE_BUS_ERROR_COUNT)) {
		dev_warn(dev, "%s: fe0_bus_errors: %#lx\n", __func__, status.fe0_bus_errors[0]);
		trace_disp_bus_err_irqs("fe0_bus_errors", status.fe0_bus_errors[0]);
		dc->hw.fe0_has_bus_errors = true;
	}

	if (!bitmap_empty(status.fe1_bus_errors, DC_HW_FE_BUS_ERROR_COUNT)) {
		dev_warn(dev, "%s: fe1_errors: %#lx\n", __func__, status.fe1_bus_errors[0]);
		trace_disp_bus_err_irqs("fe1_bus_errors", status.fe1_bus_errors[0]);
		dc->hw.fe1_has_bus_errors = true;
	}

	if (!bitmap_empty(status.be_bus_errors, DC_HW_BE_BUS_ERROR_COUNT)) {
		dev_warn(dev, "%s: be_bus_errors: %#lx\n", __func__, status.be_bus_errors[0]);
		trace_disp_bus_err_irqs("be_bus_errors", status.be_bus_errors[0]);
		dc->hw.be_has_bus_errors = true;
	}

	trace_disp_frame_irqs(status.display_te_rising, status.display_te_falling,
			      status.display_frm_start, status.layer_frm_done,
			      status.display_frm_done, status.wb_frm_done);
	trace_disp_err_irqs(status.pvric_decode_err, status.display_underrun, status.wb_datalost);
	trace_disp_reset_irqs(status.layer_reset_done, status.reset_status[FE0_SW_RESET],
			      status.reset_status[FE1_SW_RESET], status.reset_status[BE_SW_RESET]);

	for (i = 0; i < dc_info->display_num; i++) {
		u8 display_id = dc_info->displays[i].id;
		u8 display_mask = BIT(display_id);
		struct vs_crtc *vs_crtc = dc->crtc[i];
		struct drm_crtc *crtc = &vs_crtc->base;
		struct dc_hw_display *display = &dc->hw.display[i];
		pid_t pid;
		bool ddic_cmd_mode_start_scan_now = false;

		if (!vs_crtc)
			continue;

		pid = vs_crtc->trace_pid;

		if (display->mode.output_mode & VS_OUTPUT_MODE_CMD) {
			if (display_mask & status.display_te_rising) {
				DPU_ATRACE_INT_PID_FMT(1, pid, "TE[%d]", display_id);
				if (display->mode.v_sync_polarity) {
					drm_crtc_handle_vblank(crtc);
					atomic_inc(&vs_crtc->te_count);
					vs_crtc->ddic_cmd_mode_start_scan = false;
				} else {
					vs_crtc->ddic_cmd_mode_start_scan = true;
					ddic_cmd_mode_start_scan_now = true;
				}
			}

			if (display_mask & status.display_te_falling) {
				DPU_ATRACE_INT_PID_FMT(0, pid, "TE[%d]", display_id);
				if (!display->mode.v_sync_polarity) {
					drm_crtc_handle_vblank(crtc);
					atomic_inc(&vs_crtc->te_count);
					vs_crtc->ddic_cmd_mode_start_scan = false;
				} else {
					vs_crtc->ddic_cmd_mode_start_scan = true;
					ddic_cmd_mode_start_scan_now = true;
				}
			}
		} else {
			if (display_mask & status.display_frm_start) {
				display->vblank_count++;
				drm_crtc_handle_vblank(crtc);
			}
		}

		if (display_mask & status.display_frm_done) {
			DPU_ATRACE_INT_PID_FMT(0, pid, "frame_tx[%d]", display_id);
			dc_hw_display_frame_done(&dc->hw, i, &status);
			vs_crtc->ltm_hist_query_pending = false;
			if (!atomic_dec_if_positive(&vs_crtc->frames_pending))
				vs_dc_underrun_workaround(dc, crtc, display, display_id, false);
			DPU_ATRACE_INT_PID_FMT(atomic_read(&vs_crtc->frames_pending),
					       vs_crtc->trace_pid, "frames_pending[%u]",
					       crtc->index);
			trace_disp_frame_done(display_id, vs_crtc);
			vs_crtc->frame_transfer_pending = false;
			atomic_inc(&vs_crtc->frame_done_count);
			wake_up_all(&vs_crtc->framedone_waitq);

			if (!dc->disable_hw_reset)
				vs_crtc->needs_hw_reset = false;

			vs_crtc->recovery.count = 0;
		}

		if ((display_mask & status.display_frm_start) && display->config_status) {
			vs_dc_underrun_workaround(dc, crtc, display, display_id, true);
			DPU_ATRACE_INT_PID_FMT(1, pid, "frame_tx[%d]", display_id);
			trace_disp_frame_start(display_id, vs_crtc);
			vs_crtc->frame_transfer_pending = true;
			vs_crtc_handle_frm_start(crtc,
						 (display_mask & status.display_underrun) != 0);
		}

		if (display_mask & status.display_underrun)
			_vs_dc_handle_underrun(dc, vs_crtc, display_id,
					       ddic_cmd_mode_start_scan_now);
	}

	if (status.wb_frm_done || status.wb_datalost) {
		for (i = 0; i < dc_info->wb_num; i++) {
			struct vs_writeback_connector *vs_wb_conn = dc->writeback[i];
			bool wb_stall_support = wb_info[vs_wb_conn->id].wb_stall;
			if ((BIT(vs_wb_conn->id) & status.wb_datalost) && !wb_stall_support) {
				DPU_ATRACE_INSTANT_PID_FMT(current->tgid, "wb_datalost[%d]", i);
				dev_warn(dev, "%s: data lost intr for wb%d\n", __func__,
					 vs_wb_conn->id);
				vs_wb_conn->frame_pending--;
			}

			if (BIT(vs_wb_conn->id) & status.wb_frm_done) {
				DPU_ATRACE_INSTANT_PID_FMT(current->tgid, "wb_frame_done[%d]",
							   vs_wb_conn->id);
				dev_dbg(dev, "%s: frame done for wb%d\n", __func__,
					 vs_wb_conn->id);
				vs_wb_conn->frame_pending--;
				wake_up_all(&vs_wb_conn->framedone_waitq);
				vs_writeback_handle_vblank(vs_wb_conn);
			}
		}
	}

	/* save reset status bits */
	if (status.reset_status[FE0_SW_RESET])
		dc->hw.reset_status[FE0_SW_RESET] = status.reset_status[FE0_SW_RESET];

	if (status.reset_status[FE1_SW_RESET])
		dc->hw.reset_status[FE1_SW_RESET] = status.reset_status[FE1_SW_RESET];

	if (status.reset_status[BE_SW_RESET])
		dc->hw.reset_status[BE_SW_RESET] = status.reset_status[BE_SW_RESET];

#if IS_ENABLED(CONFIG_DEBUG_FS)
	for (i = 0; i < dc_info->plane_num; i++) {
		u32 display_id = dc->hw.plane[i].fb.display_id;
		u8 display_mask = BIT(dc_info->displays[display_id].id);
		u8 plane_mask = 0;
		u8 temp = 0;

		if (i >= dc_info->plane_fe0_num) {
			temp = i - dc_info->plane_fe0_num;
			plane_mask = BIT(dc_info->planes_fe1[temp].id);
		} else {
			plane_mask = BIT(dc_info->planes_fe0[i].id);
		}
		if ((display_mask & status.display_frm_done) ||
		    (plane_mask & status.layer_frm_done))
			vs_dc_get_plane_crc(dc, dc->planes[i].base);
	}
	for (i = 0; i < dc_info->display_num; i++) {
		u8 display_mask = BIT(dc_info->displays[i].id);

		if (dc->hw.display[i].crc.enable && (display_mask & status.display_frm_done))
			vs_dc_get_display_crc(dc, &dc->crtc[i]->base);
	}
#endif
}

static irqreturn_t dc_isr(int irq, void *data)
{
	struct vs_dc *dc = data;

	spin_lock(&dc->int_lock);
	vs_dc_handle_interrupts(dc);
	spin_unlock(&dc->int_lock);

	return IRQ_HANDLED;
}

void vs_dc_check_interrupts(struct device *dev)
{
	struct vs_dc *dc = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&dc->int_lock, flags);
	vs_dc_handle_interrupts(dc);
	spin_unlock_irqrestore(&dc->int_lock, flags);
}

int vs_sw_reset_ioctl(struct drm_device *drm_dev, void *data, struct drm_file *file_priv)
{
	int ret;
	struct vs_drm_private *priv = drm_dev->dev_private;
	struct vs_dc *dc = NULL;
	struct device *dev;

	if (!priv->dc_dev)
		return -EINVAL;

	dev = priv->dc_dev;

	dc = dev_get_drvdata(dev);
	if (!dc)
		return -EINVAL;

	ret = vs_dc_power_get(dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power ON\n", __func__);

	/* reset the hardware */
	dc_hw_do_reset(&dc->hw);

	/* reinitialize the hardware tracking state objects */
	dc_hw_reinit(&dc->hw);

	/* reset the DRM state. */
	drm_mode_config_reset(drm_dev);

#if IS_ENABLED(CONFIG_VERISILICON_REGMAP)
	regmap_reinit_cache(dc->hw.regmap, &dc->hw.regmap_config);
#endif

	ret = vs_dc_power_put(dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power OFF\n", __func__);

	return 0;
}

int vs_get_feature_cap_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_vs_query_feature_cap *args = data;
	struct vs_drm_private *priv = dev->dev_private;
	struct vs_dc *dc = NULL;

	if (!priv->dc_dev)
		return -EINVAL;

	dc = dev_get_drvdata(priv->dc_dev);
	if (!dc)
		return -EINVAL;
	switch (args->type) {
	case VS_FEATURE_CAP_FBC:
		args->cap = dc->hw.info->cap_dec;
		break;
	case VS_FEATURE_CAP_MAX_BLEND_LAYER:
		args->cap = dc->hw.info->max_blend_layer;
		break;
	case VS_FEATURE_CAP_CURSOR_WIDTH:
		args->cap = dev->mode_config.cursor_width;
		break;
	case VS_FEATURE_CAP_CURSOR_HEIGHT:
		args->cap = dev->mode_config.cursor_height;
		break;
	case VS_FEATURE_CAP_LINEAR_YUV_ROTATION:
		args->cap = dc->hw.info->linear_yuv_rotation;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int vs_get_hist_bins_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct vs_drm_private *priv = dev->dev_private;
	struct vs_crtc *vs_crtc;
	struct vs_dc *dc;
	struct dc_hw *hw;
	struct dc_hw_display *display;
	struct drm_mode_object *obj;
	struct drm_vs_hist_bins_query *query = data;
	int ret = 0;
	struct vs_gem_node *gem_node;
	unsigned long flags;

	if (!priv->dc_dev)
		return -EINVAL;

	dc = dev_get_drvdata(priv->dc_dev);
	if (!dc)
		return -EINVAL;
	hw = &dc->hw;

	/* check histogram channel */
	if (query->idx >= VS_HIST_IDX_COUNT)
		return -EINVAL;

	/*
	 * userspace provides crtc object id
	 * therefore, we need to discover matching vs_crtc->id
	 */
	obj = drm_mode_object_find(dev, file_priv, query->crtc_id, DRM_MODE_OBJECT_CRTC);
	if (!obj) {
		dev_err(hw->dev, "failed to find crtc object\n");
		return -ENOENT;
	}

	vs_crtc = to_vs_crtc(obj_to_crtc(obj));
	if (vs_crtc->id > DC_DISPLAY_NUM) {
		drm_mode_object_put(obj);
		dev_err(hw->dev, "invalid crtc object\n");
		return -ENOENT;
	}

	display = &hw->display[vs_crtc->id];
	drm_mode_object_put(obj);

	/* is it supported ? */
	if (!display->info || !display->info->histogram) {
		dev_err(hw->dev, "unsupported histogram\n");
		return -ENXIO;
	}

	/* validate idx */
	if (query->idx > VS_HIST_IDX_COUNT) {
		dev_err(hw->dev, "invalid histogram channel\n");
		return -EFAULT;
	}

	/* handle regular histogram channels */
	if (query->idx < VS_HIST_CHAN_IDX_COUNT) {
		struct dc_hw_hist_chan *hw_hist_chan = &display->hw_hist_chan[query->idx];
		const struct drm_vs_hist_chan *hist_config = &hw_hist_chan->drm_config;

		/* enough space ? */
		if (query->hist_bins_size != (sizeof(struct drm_vs_hist_chan_bins))) {
			dev_err(hw->dev, "invalid buffer length\n");
			return -ENOMEM;
		}

		spin_lock_irqsave(&hw->histogram_slock, flags);

		/* check if enabled or property changed */
		if (!hw_hist_chan->enable || hw_hist_chan->changed) {
			spin_unlock_irqrestore(&hw->histogram_slock, flags);
			return -ENOTCONN;
		}

		/* check if histogram_data is available */
		gem_node = hw_hist_chan->gem_node[VS_HIST_STAGE_READY];
		if (!gem_node) {
			spin_unlock_irqrestore(&hw->histogram_slock, flags);
			return -ENODATA;
		}

		/* increase gem_node use count */
		hw_hist_chan->gem_node[VS_HIST_STAGE_USER] = gem_node;
		vs_gem_pool_node_acquire(&vs_crtc->hist_chan_gem_pool[query->idx], gem_node);

		spin_unlock_irqrestore(&hw->histogram_slock, flags);

		/* copy data to user */
		query->user_data = hist_config->user_data;
		ret = copy_to_user(u64_to_user_ptr(query->hist_bins_ptr),
				   (const void *)gem_node->vaddr, query->hist_bins_size);

		/* release gem_node to mark completion of user request handling */
		spin_lock_irqsave(&hw->histogram_slock, flags);
		/* simply decrease ref count */
		hw_hist_chan->gem_node[VS_HIST_STAGE_USER] = NULL;
		vs_gem_pool_node_release(&vs_crtc->hist_chan_gem_pool[query->idx], gem_node);
		spin_unlock_irqrestore(&hw->histogram_slock, flags);
	} else { /* handle histogram rgb */
		struct dc_hw_hist_rgb *hw_hist_rgb = &display->hw_hist_rgb;

		/* enough space ? */
		if (query->hist_bins_size != (sizeof(struct drm_vs_hist_rgb_bins))) {
			dev_err(hw->dev, "invalid buffer length\n");
			return -ENOMEM;
		}

		spin_lock_irqsave(&hw->histogram_slock, flags);

		/* check if enabled or in the flight */
		if (!hw_hist_rgb->enable || hw_hist_rgb->dirty) {
			spin_unlock_irqrestore(&hw->histogram_slock, flags);
			return -ENOTCONN;
		}

		/* mark handling user request */
		/* check if histogram_data is available */
		gem_node = hw_hist_rgb->gem_node[VS_HIST_STAGE_READY];
		if (!gem_node) {
			spin_unlock_irqrestore(&hw->histogram_slock, flags);
			return -ENODATA;
		}

		/* increase gem_node use count */
		hw_hist_rgb->gem_node[VS_HIST_STAGE_USER] = gem_node;
		vs_gem_pool_node_acquire(&vs_crtc->hist_rgb_gem_pool, gem_node);

		spin_unlock_irqrestore(&hw->histogram_slock, flags);

		/* copy data to user */
		ret = copy_to_user(u64_to_user_ptr(query->hist_bins_ptr),
				   (const void *)gem_node->vaddr, query->hist_bins_size);

		/* release gem_node to mark completion of user request handling */
		spin_lock_irqsave(&hw->histogram_slock, flags);
		/* simply decrease ref count */
		hw_hist_rgb->gem_node[VS_HIST_STAGE_USER] = NULL;
		vs_gem_pool_node_release(&vs_crtc->hist_rgb_gem_pool, gem_node);
		spin_unlock_irqrestore(&hw->histogram_slock, flags);
	}

	return ret;
}

int vs_get_hw_cap_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_vs_query_hw_cap *args = data;
	struct vs_drm_private *priv = dev->dev_private;
	struct vs_dc *dc = NULL;

	if (!priv->dc_dev)
		return -EINVAL;

	dc = dev_get_drvdata(priv->dc_dev);
	if (!dc)
		return -EINVAL;

	return vs_dc_get_hw_cap(dc->hw.info, args->type, &args->cap);
}

int vs_get_ltm_hist_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	int ret;
	struct drm_vs_ltm_histogram_data *args = data;
	struct vs_drm_private *priv = dev->dev_private;
	struct vs_dc *dc = NULL;
	struct drm_mode_object *obj;
	struct vs_crtc *vs_crtc;
	const struct vs_display_info *display_info;

	if (!args->hist_ptr)
		return -ENOMEM;

	if (!priv->dc_dev)
		return -EINVAL;

	dc = dev_get_drvdata(priv->dc_dev);
	if (!dc)
		return -EINVAL;

	obj = drm_mode_object_find(dev, file_priv, args->crtc_id, DRM_MODE_OBJECT_CRTC);
	if (!obj) {
		dev_err(dc->hw.dev, "failed to find crtc object for %u\n", args->crtc_id);
		return -ENOENT;
	}

	vs_crtc = to_vs_crtc(obj_to_crtc(obj));
	drm_mode_object_put(obj);

	if (!vs_crtc || vs_crtc->id >= DC_DISPLAY_NUM) {
		dev_err(dc->hw.dev, "invalid crtc object %p id %u\n", vs_crtc,
			(vs_crtc ? vs_crtc->id : 0));
		return -ENOENT;
	}

	display_info = &dc->hw.info->displays[vs_crtc->id];
	if (!display_info->ltm && !display_info->gtm)
		return -ENODEV;

	ret = pm_runtime_get_if_in_use(priv->dc_dev);
	if (ret > 0) {
		ret = vs_crtc_get_ltm_hist(file_priv, vs_crtc, &dc->hw, args);
		pm_runtime_put_sync(priv->dc_dev);
	}

	return ret;
}

static ssize_t early_wakeup_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t early_wakeup_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t len)
{
	bool wakeup;
	int ret;

	if (!dev || !buf || !len) {
		pr_err("invalid parameter\n");
		return -EINVAL;
	}

	if (kstrtobool(buf, &wakeup) < 0)
		return -EINVAL;

	if (wakeup) {
		DPU_ATRACE_BEGIN(__func__);
		ret = pm_request_resume(dev);
		if (!ret) {
			pm_runtime_mark_last_busy(dev);
			pm_request_autosuspend(dev);
		}
		sysfs_notify(&dev->kobj, NULL, "early_wakeup");
		DPU_ATRACE_END(__func__);
	}

	return len;
}
static DEVICE_ATTR_RW(early_wakeup);

static int dc_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct vs_drm_private *priv = drm_dev->dev_private;
	struct vs_dc *dc = dev_get_drvdata(dev);
	const struct vs_dc_info *dc_info;
	int ret;

	if (!drm_dev || !dc) {
		dev_err(dev, "devices are not created.\n");
		return -ENODEV;
	}

	ret = vs_dc_power_get(dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power ON\n", __func__);

	vs_dc_enable_irqs(dc);

	ret = dc_init(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize DC hardware.\n");
		goto err_init_dc;
	}

	ret = vs_drm_iommu_attach_device(drm_dev, dev);
	if (ret < 0) {
		dev_err(dev, "Failed to attached iommu device.\n");
		goto err_clean_dc;
	}

	dc_info = dc->hw.info;

	drm_dev->mode_config.min_width = 0xffff;
	drm_dev->mode_config.min_height = 0xffff;
	drm_dev->mode_config.max_width = 0x0;
	drm_dev->mode_config.max_height = 0x0;

	priv->dc_dev = dev;

	vs_drm_update_alignment(drm_dev, dc_info->pitch_alignment, dc_info->addr_alignment);

	ret = vs_dc_power_put(dev, true);
	if (ret < 0)
		dev_err(dev, "%s: failed to power OFF\n", __func__);

	device_create_file(dev, &dev_attr_early_wakeup);

	return 0;

err_clean_dc:
	dc_deinit(dev);
err_init_dc:
	if (vs_dc_power_put(dev, true) < 0)
		dev_err(dev, "%s: failed to power OFF during error cleanup\n", __func__);
	return ret;
}

static void dc_unbind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *drm_dev = data;
	struct vs_dc *dc = dev_get_drvdata(dev);

	vs_dc_disable_irqs(dc);

	device_remove_file(dev, &dev_attr_early_wakeup);

	dc_deinit(dev);

	vs_drm_iommu_detach_device(drm_dev, dev);
}

const struct component_ops dc_component_ops = {
	.bind = dc_bind,
	.unbind = dc_unbind,
};

static const struct of_device_id dc_driver_dt_match[] = {
	{
		.compatible = "verisilicon,dc9x00",
	},
	{},
};
MODULE_DEVICE_TABLE(of, dc_driver_dt_match);

static void detach_power_domain(struct device *dev, struct vs_dc *dc, int end_idx)
{
	int i;

	for (i = end_idx - 1; i >= 0; i--) {
		device_link_del(dc->pds[i].devlink);
		if (dc->pds[i].dev && !IS_ERR(dc->pds[i].dev))
			dev_pm_domain_detach(dc->pds[i].dev, true);
	}
}

static int attach_power_domain(struct device *dev, struct vs_dc *dc)
{
	int i;
	int ret = 0;

	dc->num_pds = of_property_count_strings(dev->of_node, "power-domain-names");
	if (dc->num_pds == -EINVAL) {
		// It's possible to have no power-domain.
		dc->num_pds = 0;
		return 0;
	}
	if (dc->num_pds < 0) {
		dev_err(dev, "failed to read power-domain-names property\n");
		return -EINVAL;
	}
	dc->pds = devm_kmalloc_array(dev, dc->num_pds, sizeof(*dc->pds), GFP_KERNEL);
	if (!dc->pds)
		return -ENOMEM;

	for (i = 0; i < dc->num_pds; i++) {
		dc->pds[i].dev = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(dc->pds[i].dev)) {
			dev_err(dev, "failed to attach power domain at index %d\n", i);
			ret = PTR_ERR(dc->pds[i].dev);
			goto clean_up;
		}

		dc->pds[i].devlink = device_link_add(dev, dc->pds[i].dev,
						     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
		if (!dc->pds[i].devlink) {
			dev_err(dev, "failed to create a devlink to the pd at index %d\n", i);
			dev_pm_domain_detach(dc->pds[i].dev, true);
			ret = -EINVAL;
			goto clean_up;
		}
	}

	return ret;

clean_up:
	detach_power_domain(dev, dc, i);
	return ret;
}

static int dc_parse_dt(struct device *dev, struct vs_dc *dc)
{
	if (dc->path) {
		of_property_read_u32(dev->of_node, "min-rd-avg-bw",
				     &dc->min_qos_config.rd_avg_bw_mbps);
		of_property_read_u32(dev->of_node, "min-rd-peak-bw",
				     &dc->min_qos_config.rd_peak_bw_mbps);
		of_property_read_u32(dev->of_node, "min-rd-rt-bw",
				     &dc->min_qos_config.rd_rt_bw_mbps);
		of_property_read_u32(dev->of_node, "min-wr-avg-bw",
				     &dc->min_qos_config.wr_avg_bw_mbps);
		of_property_read_u32(dev->of_node, "min-wr-peak-bw",
				     &dc->min_qos_config.wr_peak_bw_mbps);
		of_property_read_u32(dev->of_node, "min-wr-rt-bw",
				     &dc->min_qos_config.wr_rt_bw_mbps);
	}

	of_property_read_u32(dev->of_node, "min-core-clk",
			     &dc->min_qos_config.core_clk);

	if (of_property_read_u32(dev->of_node, "boost-fabrt-freq", &dc->boost_fabrt_freq))
		dc->boost_fabrt_freq = 0;

	return 0;
}

static int dc_get_trusty_device(struct device *dev, struct vs_dc *dc)
{
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "tzprot-device", 0);
	if (!np) {
		dev_warn(dev, "tzprot-device phandle not found in dts\n");
		dc->tzprot_pdev = NULL;
		return 0;
	}
	dc->tzprot_pdev = of_find_device_by_node(np);
	of_node_put(np);

	if (!dc->tzprot_pdev) {
		dev_err(dev, "tzprot-device phandle doesn't refer to a device\n");
		return -EINVAL;
	}

	return 0;
}

static int dc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_link *link = NULL;
	struct vs_dc *dc;
	int i, irq, ret;
	struct resource *resource = NULL;

	dc = devm_kzalloc(dev, sizeof(*dc), GFP_KERNEL);
	if (!dc)
		return -ENOMEM;

	spin_lock_init(&dc->int_lock);
	mutex_init(&dc->dc_lock);
	mutex_init(&dc->dc_qos_lock);

	ret = attach_power_domain(dev, dc);
	if (ret < 0)
		return ret;

	dc->hw.reg_base = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(dc->hw.reg_base)) {
		ret = PTR_ERR(dc->hw.reg_base);
		goto detach_pd;
	}
	dc->hw.reg_size = resource != NULL ? resource_size(resource) : 0;
	dc->hw.reg_dump_offset = 0;
	dc->hw.reg_dump_size = dc->hw.reg_size;
	dc->hw.dev = dev;

	// TODO(b/240503241): Avoid a bunch of irqs with the same handler when
	// we have better solution from the interrupt parent. e.g. register by
	// mask instead of a single bit
	dc->irq_num = platform_irq_count(pdev);
	if (dc->irq_num <= 0) {
		ret = -EINVAL;
		goto detach_pd;
	}

	dc->irqs = devm_kmalloc_array(dev, dc->irq_num, sizeof(*dc->irqs), GFP_KERNEL);
	if (!dc->irqs) {
		ret = -ENOMEM;
		goto detach_pd;
	}

	for (i = 0; i < dc->irq_num; i++) {
		irq = platform_get_irq(pdev, i);
		ret = devm_request_irq(dev, irq, dc_isr, IRQF_NO_AUTOEN, dev_name(dev), dc);
		if (ret < 0) {
			dev_err(dev, "Failed to install irq:%u.\n", irq);
			goto detach_pd;
		}
		dc->irqs[i] = irq;
	}

	if (of_property_present(dev->of_node, "devfreq")) {
		dc->core_devfreq = devfreq_get_devfreq_by_phandle(dev, "devfreq", 0);
	} else {
		// TODO(b/349042813): devfreq driver integration
		dev_warn(dev, "core_devfreq not presented in dts, skip it\n");
		dc->core_devfreq = NULL;
	}
	if (IS_ERR(dc->core_devfreq)) {
		ret = PTR_ERR(dc->core_devfreq);
		if (ret == -ENODEV)
			ret = -EPROBE_DEFER;
		else
			dev_err(dev, "failed to get core_devfreq source, ret=%d\n", ret);
		goto detach_pd;
	}

	if (dc->core_devfreq) {
		link = device_link_add(dev, dc->core_devfreq->dev.parent,
				       DL_FLAG_AUTOREMOVE_CONSUMER | DL_FLAG_PM_RUNTIME);
		if (!link) {
			dev_err(dev, "failed to add devlink to the devfreq dev\n");
			ret = -EINVAL;
			goto detach_pd;
		}
	}

	dc->path = google_devm_of_icc_get(dev, "sswrp-dpu");
	if (IS_ERR(dc->path)) {
		ret = PTR_ERR(dc->path);
		dev_err(dev, "failed to get icc path: %d\n", ret);
		goto detach_pd;
	}

	/* optional configuration, it's for boosting FABRT freq */
	dc->fabrt_devfreq = devfreq_get_devfreq_by_phandle(dev, "fabrtfreq", 0);
	if (IS_ERR(dc->fabrt_devfreq)) {
		ret = PTR_ERR(dc->fabrt_devfreq);
		dev_dbg(dev, "failed to get fabrt_devfreq source, ret=%d\n", ret);
		/* clear fabrt_devfreq for disabling the boosting mechanism */
		dc->fabrt_devfreq = 0;
	}

	ret = dc_parse_dt(dev, dc);
	if (ret) {
		dev_err(dev, "failed to parse device-tree\n");
		goto detach_pd;
	}

	ret = dc_get_trusty_device(dev, dc);
	if (ret) {
		dev_err(dev, "error getting the trust zone device driver\n");
		goto detach_pd;
	}

	ret = dc_init_debugfs(dc);
	if (ret) {
		dev_err(dev, "failed to init debugfs\n");
		goto detach_pd;
	}

	dev_set_drvdata(dev, dc);

	pm_runtime_set_autosuspend_delay(dev, VS_DC_AUTOSUPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		dev_err(dev, "runtime pm enabled but failed to add disable action: %d\n", ret);

	ret = component_add(dev, &dc_component_ops);
	if (ret)
		dev_err(dev, "failed to add components: %d\n", ret);

	return ret;

detach_pd:
	detach_power_domain(dev, dc, dc->num_pds);
	return ret;
}

static int dc_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vs_dc *dc = platform_get_drvdata(pdev);

	dc_deinit_debugfs(dc);
	component_del(dev, &dc_component_ops);
	detach_power_domain(dev, dc, dc->num_pds);

	dev_set_drvdata(dev, NULL);

	return 0;
}

struct platform_driver dc_platform_driver = {
	.probe = dc_probe,
	.remove = dc_remove,
	.driver = {
		.name = "vs-dc",
		.of_match_table = of_match_ptr(dc_driver_dt_match),
#if IS_ENABLED(CONFIG_PM_SLEEP)
		.pm = &vs_dc_pm_ops,
#endif
	},
};

MODULE_DESCRIPTION("VeriSilicon DC Driver");
MODULE_LICENSE("GPL v2");
