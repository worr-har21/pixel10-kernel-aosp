/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM vs_drm

#if !defined(_VS_TRACE_9x00_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _VS_TRACE_9x00_H_

#include <linux/pm_runtime.h>
#include <linux/tracepoint.h>
#include <linux/timekeeping.h>
#include <vs_dc.h>
#include <vs_dc_hw.h>

#ifndef __9x00_VS_HW_TRACE_API_DEF__
#define __9x00_VS_HW_TRACE_API_DEF__

/*
 * Use enum to reduce some degree of function complexity explosion,
 * particularly in cases where performance is less critical
 */

enum hw_trace_component_9x00 {
	HW_TRACE_LAYER_9x00 = 0,
	HW_TRACE_OUT_CTRL_9x00,
	HW_TRACE_WRITEBACK_9x00,
};

#endif /* __9x00_VS_HW_TRACE_API_DEF__ */

TRACE_DEFINE_ENUM(HW_TRACE_LAYER_9x00);
TRACE_DEFINE_ENUM(HW_TRACE_OUT_CTRL_9x00);
TRACE_DEFINE_ENUM(HW_TRACE_WRITEBACK_9x00);

#define show_trace_component_9x00(x) \
	__print_symbolic(x, \
		 { HW_TRACE_LAYER_9x00, "Layer" }, \
		 { HW_TRACE_OUT_CTRL_9x00, "Out_ctrl" }, \
		 { HW_TRACE_WRITEBACK_9x00, "Writeback" })

/* Specific HW Programming Traces */

TRACE_EVENT(disp_set_mode,
	TP_PROTO(u32 hw_id, struct dc_hw_display_mode *mode),
	TP_ARGS(hw_id, mode),
	TP_STRUCT__entry(
			__field(u32, hw_id)
			__field(u32, bus_format)
			__field(u32, output_mode)
			__field(u16, h_active)
			__field(u16, v_active)
			__field(int, fps)
			__field(bool, en)
			__field(bool, vrr_enable)
	),
	TP_fast_assign(
			__entry->hw_id = hw_id;
			__entry->en = mode->enable;
			__entry->bus_format = mode->bus_format;
			__entry->vrr_enable = mode->vrr_enable;
			__entry->output_mode = mode->output_mode;
			__entry->h_active = mode->h_active;
			__entry->v_active = mode->v_active;
			__entry->fps = mode->fps;
	),
	TP_printk("[Out_ctrl%d] SET_MODE: en:%d bus_format:%d vrr_enable:%d output_mode:%d timings:%dx%d@%d",
		  __entry->hw_id, __entry->en, __entry->bus_format, __entry->vrr_enable,
		  __entry->output_mode, __entry->h_active, __entry->v_active, __entry->fps)
);

/* Framebuffer HW Programming Traces */

TRACE_EVENT(disp_config_hw_fb,
	TP_PROTO(enum hw_trace_component_9x00 component, const char *prop, u32 hw_id,
		 struct dc_hw_fb *fb),
	TP_ARGS(component, prop, hw_id, fb),
	TP_STRUCT__entry(
			__field(u32, component)
			__field(u32, hw_id)
			__field(u64, address)
			__field(u64, u_address)
			__field(u64, v_address)
			__field(u32, stride)
			__field(u32, u_stride)
			__field(u32, v_stride)
			__field(u16, width)
			__field(u16, height)
			__field(u8, format)
			__field(u8, tile_mode)
			__field(u8, rotation)
			__field(u8, swizzle)
			__field(u8, uv_swizzle)
			__field(u8, zpos)
			__field(bool, secure)
			__field(bool, en)
			__string(prop, prop)
	),
	TP_fast_assign(
			__entry->component = component;
			__entry->hw_id = hw_id;
			__entry->address = fb->address;
			__entry->u_address = fb->u_address;
			__entry->v_address = fb->v_address;
			__entry->stride = fb->stride;
			__entry->u_stride = fb->u_stride;
			__entry->v_stride = fb->v_stride;
			__entry->width = fb->width;
			__entry->height = fb->height;
			__entry->format = fb->format;
			__entry->rotation = fb->rotation;
			__entry->tile_mode = fb->tile_mode;
			__entry->swizzle = fb->swizzle;
			__entry->uv_swizzle = fb->uv_swizzle;
			__entry->zpos = fb->zpos;
			__entry->secure = fb->secure;
			__entry->en = fb->enable;
			__assign_str(prop, prop);
	),

	TP_printk(
		"[%s%d] %s: en:%d address_[x u v]:%#llx %#llx %#llx width:%d height:%d format:%d stride_[x u v]:%d %d %d tile_mode:%d rotation:%d swizzle_[x uv]:%d %d zpos:%d secure_layer:%d",
		show_trace_component_9x00(__entry->component), __entry->hw_id, __get_str(prop),
		__entry->en, __entry->address, __entry->u_address, __entry->v_address,
		__entry->width, __entry->height, __entry->format, __entry->stride,
		__entry->u_stride, __entry->v_stride, __entry->tile_mode, __entry->rotation,
		__entry->swizzle, __entry->uv_swizzle, __entry->zpos, __entry->secure));

TRACE_EVENT(disp_underrun_config,
	TP_PROTO(u32 hw_id, u32 output_id, struct dc_hw_display_mode *mode, u32 te_width_us,
		 u32 config),
	TP_ARGS(hw_id, output_id, mode, te_width_us, config),
	TP_STRUCT__entry(
			__field(u32, hw_id)
			__field(u32, output_id)
			__field(u32, h_active)
			__field(u32, v_active)
			__field(u32, te_width_us)
			__field(int, fps)
			__field(u32, config)
	),
	TP_fast_assign(
			__entry->hw_id = hw_id;
			__entry->output_id = output_id;
			__entry->h_active = mode->h_active;
			__entry->v_active = mode->v_active;
			__entry->te_width_us = te_width_us;
			__entry->fps = mode->fps;
			__entry->config = config;
	),
	TP_printk("[Out_ctrl%d] output_id:%u timings:%dx%d@%d te_width_us:%u config:%#x",
		  __entry->hw_id, __entry->output_id,  __entry->h_active, __entry->v_active,
		  __entry->fps, __entry->te_width_us, __entry->config)
);

TRACE_EVENT(disp_urgent_cmd_config,
	TP_PROTO(u32 hw_id, u32 output_id, struct dc_hw_display_mode *mode, u32 sys_counter,
		 u32 sys_delay_counter, u32 urgent_value),
	TP_ARGS(hw_id, output_id, mode, sys_counter, sys_delay_counter, urgent_value),
	TP_STRUCT__entry(
			__field(u32, hw_id)
			__field(u32, output_id)
			__field(u32, h_active)
			__field(u32, v_active)
			__field(int, fps)
			__field(u32, sys_counter)
			__field(u32, sys_delay_counter)
			__field(u32, urgent_value)
	),
	TP_fast_assign(
			__entry->hw_id = hw_id;
			__entry->output_id = output_id;
			__entry->h_active = mode->h_active;
			__entry->v_active = mode->v_active;
			__entry->fps = mode->fps;
			__entry->sys_counter = sys_counter;
			__entry->sys_delay_counter = sys_delay_counter;
			__entry->urgent_value = urgent_value;
	),
	TP_printk("[Out_ctrl%d] output_id:%u timings:%dx%d@%d sys_counter:%#x sys_delay_counter:%#x urgent_value:%#x",
		  __entry->hw_id, __entry->output_id, __entry->h_active, __entry->v_active,
		  __entry->fps, __entry->sys_counter, __entry->sys_delay_counter,
		  __entry->urgent_value)
);

TRACE_EVENT(disp_urgent_vid_config,
	TP_PROTO(u32 hw_id, u32 output_id, u32 qos_thresh_0, u32 qos_thresh_1, u32 qos_thresh_2,
		 u32 urgent_thresh_0, u32 urgent_thresh_1, u32 urgent_thresh_2,
		 u32 urgent_low_thresh, u32 urgent_high_thresh,
		 u32 healthy_thresh),
	TP_ARGS(hw_id, output_id, qos_thresh_0, qos_thresh_1, qos_thresh_2,
		urgent_thresh_0, urgent_thresh_1, urgent_thresh_2,
		urgent_low_thresh, urgent_high_thresh,
		healthy_thresh),
	TP_STRUCT__entry(
			__field(u32, hw_id)
			__field(u32, output_id)
			__field(u32, qos_thresh_0)
			__field(u32, qos_thresh_1)
			__field(u32, qos_thresh_2)
			__field(u32, urgent_thresh_0)
			__field(u32, urgent_thresh_1)
			__field(u32, urgent_thresh_2)
			__field(u32, urgent_low_thresh)
			__field(u32, urgent_high_thresh)
			__field(u32, healthy_thresh)
	),
	TP_fast_assign(
			__entry->hw_id = hw_id;
			__entry->output_id = output_id;
			__entry->qos_thresh_0 = qos_thresh_0;
			__entry->qos_thresh_1 = qos_thresh_1;
			__entry->qos_thresh_2 = qos_thresh_2;
			__entry->urgent_thresh_0 = urgent_thresh_0;
			__entry->urgent_thresh_1 = urgent_thresh_1;
			__entry->urgent_thresh_2 = urgent_thresh_2;
			__entry->urgent_low_thresh = urgent_low_thresh;
			__entry->urgent_high_thresh = urgent_high_thresh;
			__entry->healthy_thresh = healthy_thresh;
	),
	TP_printk(
		"[Out_ctrl%d] output_id:%u qos [%#x %#x %#x] urgent [%#x %#x %#x] lo:%#x hi:%#x health:%#x",
		__entry->hw_id, __entry->output_id,
		__entry->qos_thresh_0, __entry->qos_thresh_1, __entry->qos_thresh_2,
		__entry->urgent_thresh_0, __entry->urgent_thresh_1, __entry->urgent_thresh_2,
		__entry->urgent_low_thresh, __entry->urgent_high_thresh,
		__entry->healthy_thresh)
);

#define trace_config_hw_layer_fb(name, hw_id, fb) \
	trace_disp_config_hw_fb(HW_TRACE_LAYER_9x00, (name), (hw_id), (fb))
#define trace_config_hw_display_fb(name, hw_id, fb) \
	trace_disp_config_hw_fb(HW_TRACE_OUT_CTRL_9x00, (name), (hw_id), (fb))
#define trace_config_hw_wb_fb(name, hw_id, fb) \
	trace_disp_config_hw_fb(HW_TRACE_WRITEBACK_9x00, (name), (hw_id), (fb))

DECLARE_EVENT_CLASS(disp_dc,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc),
	TP_STRUCT__entry(
		__field(bool, enabled)
	),
	TP_fast_assign(
		__entry->enabled = dc->enabled;
	),
	TP_printk("DC enabled:%d", __entry->enabled)
);
DEFINE_EVENT(disp_dc, disp_dc_enable,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc)
);
DEFINE_EVENT(disp_dc, disp_dc_disable,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc)
);
DECLARE_EVENT_CLASS(disp_dc_irq,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc),
	TP_STRUCT__entry(
		__field(bool, suspended)
		__field(bool, active)
		__field(int, usage_count)
		__field(int, enable_count)
	),
	TP_fast_assign(
		__entry->suspended = pm_runtime_suspended(dc->hw.dev);
		__entry->active = pm_runtime_active(dc->hw.dev);
		__entry->usage_count = atomic_read(&dc->hw.dev->power.usage_count);
		__entry->enable_count = dc->irq_enable_count;
	),
	TP_printk("DC IRQ suspended:%d active:%d usage_count:%d irq_enable_count:%d",
		  __entry->suspended, __entry->active, __entry->usage_count, __entry->enable_count)
);
DEFINE_EVENT(disp_dc_irq, disp_dc_enable_irqs,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc)
);
DEFINE_EVENT(disp_dc_irq, disp_dc_disable_irqs,
	TP_PROTO(struct vs_dc *dc),
	TP_ARGS(dc)
);

DECLARE_EVENT_CLASS(disp_dc_power,
	TP_PROTO(struct device *dev, bool sync, int ret),
	TP_ARGS(dev, sync, ret),
	TP_STRUCT__entry(
		__field(bool, enabled)
		__field(bool, suspended)
		__field(bool, active)
		__field(int, usage_count)
		__field(bool, sync)
		__field(ktime_t, timestamp)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->enabled = pm_runtime_enabled(dev);
		__entry->suspended = pm_runtime_suspended(dev);
		__entry->active = pm_runtime_active(dev);
		__entry->usage_count = atomic_read(&dev->power.usage_count);
		__entry->sync = sync;
		__entry->timestamp = ktime_get_real();
		__entry->ret = ret;
	),
	TP_printk("DC PD device enabled:%d suspended:%d active:%d usage_count:%d sync:%d ts_utc:%lld ret:%d",
		  __entry->enabled, __entry->suspended, __entry->active, __entry->usage_count,
		  __entry->sync, __entry->timestamp, __entry->ret)
);
DEFINE_EVENT(disp_dc_power, disp_dc_power_get,
	TP_PROTO(struct device *dev, bool sync, int ret),
	TP_ARGS(dev, sync, ret)
);
DEFINE_EVENT(disp_dc_power, disp_dc_power_put,
	TP_PROTO(struct device *dev, bool sync, int ret),
	TP_ARGS(dev, sync, ret)
);
#endif /* _VS_TRACE_9x00_H_ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE vs_trace_9x00

/* This part must be outside protection */
#include <trace/define_trace.h>
