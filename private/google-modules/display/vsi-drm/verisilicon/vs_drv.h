/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_DRV_H__
#define __VS_DRV_H__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>

#include "vs_plane.h"

/*
 * @dma_dev: device for DMA API.
 *  - use the first attached device if support iommu
 *    else use drm device (only contiguous buffer support)
 * @domain: iommu domain for DRM.
 *  - all DC IOMMU share same domain to reduce mapping
 * @pitch_alignment: buffer pitch alignment required by sub-devices.
 */
struct vs_drm_private {
	struct device *dma_dev;
	/* when we have more than one display core, this need to be an array */
	struct device *dc_dev;

	struct iommu_domain *domain;

	unsigned int pitch_alignment;
	unsigned int addr_alignment;
};

int vs_drm_iommu_attach_device(struct drm_device *drm_dev, struct device *dev);

void vs_drm_iommu_detach_device(struct drm_device *drm_dev, struct device *dev);

void vs_drm_update_alignment(struct drm_device *drm_dev, unsigned int pitch_align,
			     unsigned int addr_align);

static inline struct device *to_dma_dev(struct drm_device *dev)
{
	struct vs_drm_private *priv = dev->dev_private;

	return priv->dma_dev;
}

static inline bool is_iommu_enabled(struct drm_device *dev)
{
	struct vs_drm_private *priv = dev->dev_private;

	return priv->domain != NULL ? true : false;
}
#endif /* __VS_DRV_H__ */
