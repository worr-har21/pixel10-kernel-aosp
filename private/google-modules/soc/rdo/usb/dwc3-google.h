/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Google LLC
 * Google's SoC-specific Glue Driver for the dwc3 USB controller
 */
#ifndef DWC3_GOOGLE_H_
#define DWC3_GOOGLE_H_

#if IS_ENABLED(CONFIG_USB_XHCI_GOOG_DMA)
extern struct xhci_goog_dma_coherent_mem **dwc3_google_get_dma_coherent_mem(struct device *dev);
extern void dwc3_google_put_dma_coherent_mem(struct device *dev);
#else
static inline struct xhci_goog_dma_coherent_mem **dwc3_google_get_dma_coherent_mem(
	struct device *dev) { return NULL; }
static inline void dwc3_google_put_dma_coherent_mem(struct device *dev) { }
#endif

#endif  // DWC3_GOOGLE_H_
