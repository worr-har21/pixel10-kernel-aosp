// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_atomic.h>
#include <drm/drm_drv.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>

#include <trace/dpu_trace.h>

#include "vs_crtc.h"
#include "vs_dc_post.h"
#include "vs_recovery.h"
#include "vs_trace.h"

#define VS_RECOVERY_TRY_MAX 3

static void vs_recovery_handler(struct work_struct *work)
{
	struct vs_recovery *recovery = container_of(work, struct vs_recovery, work);
	struct vs_crtc *vs_crtc = container_of(recovery, struct vs_crtc, recovery);
	struct device *dev = vs_crtc->dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *suspend_state;
	int ret = 0;

	DPU_ATRACE_BEGIN("crtc[%s] vs recovery handler", vs_crtc->base.name);
	dev_info(dev, "crtc[%s] vs recovery handler entered\n", vs_crtc->base.name);

	drm_modeset_acquire_init(&ctx, 0);

	suspend_state = vs_crtc_suspend(vs_crtc, &ctx);
	if (!IS_ERR_OR_NULL(suspend_state)) {
		ret = vs_crtc_resume(suspend_state, &ctx);
		drm_atomic_state_put(suspend_state);
	} else {
		dev_err(dev, "crtc[%s] failed to suspend state during recovery (%ld)\n",
			vs_crtc->base.name, PTR_ERR(suspend_state));
		ret = -EINVAL;
	}

	if (ret == 0)
		dev_info(dev, "crtc[%s] recovery is successfully finished(%d)\n",
			 vs_crtc->base.name, recovery->count);
	else
		dev_err(dev, "crtc[%s] Failed to recover display (%d)\n", vs_crtc->base.name, ret);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	atomic_dec(&recovery->recovering);
	DPU_ATRACE_END("crtc[%s] vs recovery handler", vs_crtc->base.name);
}

void vs_recovery_register(struct vs_crtc *vs_crtc)
{
	struct vs_recovery *recovery = &vs_crtc->recovery;

	INIT_WORK(&recovery->work, vs_recovery_handler);
	recovery->count = 0;
	atomic_set(&recovery->recovering, 0);

	dev_dbg(vs_crtc->dev, "crtc[%s] recovery is supported\n", vs_crtc->base.name);
}

void vs_crtc_trigger_recovery(struct vs_crtc *vs_crtc)
{
	struct vs_recovery *recovery = &vs_crtc->recovery;
	struct device *dev = vs_crtc->dev;
	struct vs_dc *dc = dev_get_drvdata(dev);
	u8 display_id = to_vs_display_id(dc, &vs_crtc->base);

	/* only recover if we don't have another active recovery attempt going */
	if (!atomic_add_unless(&recovery->recovering, 1, 1)) {
		dev_dbg(vs_crtc->dev, "crtc[%s] ignoring recovery attempt while one in progress\n",
			vs_crtc->base.name);
		return;
	}

	recovery->count++;
	trace_disp_trigger_recovery(display_id, vs_crtc);
	if (recovery->count >= VS_RECOVERY_TRY_MAX) {
		dev_err(vs_crtc->dev, "crtc[%s] maximum consecutive recovery try reached (%d)\n",
			vs_crtc->base.name, VS_RECOVERY_TRY_MAX);
		atomic_dec(&recovery->recovering);
		return;
	}

	queue_work(system_highpri_wq, &recovery->work);
}
