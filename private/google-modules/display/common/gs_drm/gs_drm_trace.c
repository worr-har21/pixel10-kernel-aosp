/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#define CREATE_TRACE_POINTS
#include <trace/dpu_trace.h>
EXPORT_TRACEPOINT_SYMBOL(tracing_mark_write);
EXPORT_TRACEPOINT_SYMBOL(reg_dump_header);
EXPORT_TRACEPOINT_SYMBOL(reg_dump_line);
EXPORT_TRACEPOINT_SYMBOL(disp_dpu_underrun);
EXPORT_TRACEPOINT_SYMBOL(disp_vblank_irq_enable);
