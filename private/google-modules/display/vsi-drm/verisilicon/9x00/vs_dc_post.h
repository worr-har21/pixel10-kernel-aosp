/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_DC_POST_H__
#define __VS_DC_POST_H__

#include <linux/mm_types.h>
#include <linux/version.h>
#include <drm/drm_modes.h>

#include "vs_crtc.h"
#include "vs_dc_hw.h"
#include "vs_writeback.h"
#include "vs_dc.h"

extern struct platform_driver dc_be_platform_driver;
extern struct platform_driver dc_wb_platform_driver;

inline u8 to_vs_display_id(struct vs_dc *dc, struct drm_crtc *crtc);

#endif /* __VS_SUB_DC_BE_H__ */
