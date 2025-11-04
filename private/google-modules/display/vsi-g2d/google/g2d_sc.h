/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */

#ifndef G2D_SC_H_
#define G2D_SC_H_

#include "g2d_sc_hw.h"
#include "g2d_writeback.h"

struct g2d_sc {
	struct g2d_crtc *crtc[NUM_PIPELINES];
	struct g2d_writeback_connector *writeback[NUM_PIPELINES];
	struct sc_hw hw;

	unsigned int irq_num;
	int *irqs;

	struct dentry *debugfs;
};

struct platform_device;
struct g2d_plane;
void g2d_wb_hw_configure(struct g2d_writeback_connector *g2d_wb_connector,
			 struct drm_framebuffer *fb);
void g2d_plane_hw_commit(struct sc_hw *hw, u8 display_id);
void sc_crtc_init(struct g2d_crtc *g2d_crtc);
void sc_plane_init(struct g2d_plane *g2d_plane);
void sc_wb_init(struct g2d_writeback_connector *g2d_wb_connector);
int sc_ioremap_memory(struct platform_device *pdev, struct g2d_sc *sc);
void sc_init(struct g2d_sc *sc, struct device *dev);
int sc_irq_init(struct platform_device *pdev);
int g2d_pm_runtime_suspend(struct device *dev);
int g2d_pm_runtime_resume(struct device *dev);
void g2d_sc_print_id_regs(struct device *dev);
#if IS_ENABLED(CONFIG_DEBUG_FS)
int sc_debugfs_init(struct device *dev);
void sc_debugfs_deinit(struct device *dev);
#else /* CONFIG_DEBUG_FS */
static inline int sc_debugfs_init(struct device *dev) { return 0; }
static inline void sc_debugfs_deinit(struct device *dev) {}
#endif /* CONFIG_DEBUG_FS */

#endif //G2D_SC_H_
