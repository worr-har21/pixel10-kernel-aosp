/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Google Corp.
 *
 * Author:
 *  Howard.Yen <howardyen@google.com>
 *  Puma.Hsu   <pumahsu@google.com>
 */

#ifndef __USB_OFFLOAD_H
#define __USB_OFFLOAD_H

struct usb_offload_data {
	struct device *dev;

	struct device *aoc_core_pd;
	struct notifier_block aoc_core_nb;
	struct completion aoc_core_pd_power_on;
	struct completion aoc_core_pd_power_off;

	bool offload_inited;
	bool offload_state;
	bool usb_audio_offload;

	/* count how many usb audio devices are connected */
	int usb_audio_count;

	struct work_struct offload_connect_ws;
	struct wakeup_source *wakelock;
};

#if IS_ENABLED(CONFIG_USB_XHCI_GOOG_DMA)
extern int usb_offload_rmem_init(struct device *dev);
extern void usb_offload_rmem_unmap(struct device *dev);
#else
static inline int usb_offload_rmem_init(struct device *dev) { return 0; }
static inline void usb_offload_rmem_unmap(struct device *dev) { }
#endif

int usb_offload_helper_init(void);
void usb_offload_helper_exit(void);

#endif /* __USB_OFFLOAD_H */
