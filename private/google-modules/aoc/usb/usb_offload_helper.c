// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Google Corp.
 *
 * Author:
 *  Howard.Yen <howardyen@google.com>
 *  Puma.Hsu   <pumahsu@google.com>
 */

#include <core/hub.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_domain.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>

#include "aoc_usb.h"
#include "xhci-exynos.h"
#include "xhci-plat.h"
#include "usb_offload.h"
#include "xhci-goog-dma.h"

#define AOC_CORE_POWER_CTRL_TIMEOUT 1000

static struct usb_offload_data *offload_data;

static void xhci_set_early_stop(struct usb_device *hdev)
{
	struct usb_hub *hub;
	struct usb_port *port_dev;

	if (!hdev->actconfig || !hdev->maxchild) {
		dev_err(&hdev->dev, "hdev is not active\n");
		return;
	}

	hub = usb_get_intfdata(hdev->actconfig->interface[0]);

	if (!hub) {
		dev_err(&hdev->dev, "can't get usb_hub\n");
		return;
	}

	port_dev = hub->ports[0];
	port_dev->early_stop = true;

	return;
}

/*
 * If the Host connected to a hub, user may connect more than two USB audio
 * headsets or DACs. A caller can call this function to know how many USB
 * audio devices are connected now.
 */
int xhci_get_usb_audio_count(void)
{
	if (offload_data)
		return offload_data->usb_audio_count;
	else
		return 0;
}

/*
 * Determine if an USB device is a compatible devices:
 *     True: Devices are audio class and they contain ISOC endpoint
 *    False: Devices are not audio class or they're audio class but no ISOC endpoint
 */
static bool is_compatible_with_usb_audio_offload(struct usb_device *udev)
{
	struct usb_endpoint_descriptor *epd;
	struct usb_host_config *config;
	struct usb_host_interface *alt;
	struct usb_interface_cache *intfc;
	int i, j, k;
	bool is_audio = false;

	config = udev->config;
	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		intfc = config->intf_cache[i];
		for (j = 0; j < intfc->num_altsetting; j++) {
			alt = &intfc->altsetting[j];

			if (alt->desc.bInterfaceClass == USB_CLASS_AUDIO) {
				for (k = 0; k < alt->desc.bNumEndpoints; k++) {
					epd = &alt->endpoint[k].desc;
					if (usb_endpoint_xfer_isoc(epd)) {
						is_audio = true;
						break;
					}
				}
			}
		}
	}

	return is_audio;
}

static void adjust_remote_wakeup(struct usb_device *udev, bool device_add)
{
	struct usb_interface_descriptor *desc;
	struct usb_host_config *config;
	struct usb_device *rhdev = udev->bus->root_hub;
	struct device *xhci_dev = rhdev->dev.parent;
	bool enable_remote_wakeup = false;
	int i;

	if (!udev || is_root_hub(udev))
		return;

	if (device_add) {
		if (is_root_hub(udev->parent) && device_can_wakeup(&udev->dev)) {
			config = udev->config;
			for (i = 0; i < config->desc.bNumInterfaces; i++) {
				desc = &config->intf_cache[i]->altsetting->desc;
				if (desc->bInterfaceClass == USB_CLASS_AUDIO) {
					enable_remote_wakeup = true;
					break;
				}
			}
		}
	} else {
		enable_remote_wakeup = false;
	}

	if (enable_remote_wakeup) {
		device_set_wakeup_enable(&udev->dev, 1);
		usb_enable_autosuspend(udev);
		__pm_relax(offload_data->wakelock);
		pm_runtime_allow(xhci_dev);
	} else {
		__pm_stay_awake(offload_data->wakelock);
		pm_runtime_forbid(xhci_dev);
	}
	dev_info(&udev->dev, "device %s, %s wakelock\n",
		 device_add ? "add" : "remove",
		 enable_remote_wakeup ? "release" : "acquire");
}

/* This function is copied from drivers/usb/core/buffer.c */
static void hcd_buffer_destroy_cp(struct usb_hcd *hcd)
{
	int i;

	if (!IS_ENABLED(CONFIG_HAS_DMA))
		return;

	for (i = 0; i < HCD_BUFFER_POOLS; i++) {
		dma_pool_destroy(hcd->pool[i]);
		hcd->pool[i] = NULL;
	}
}


static int aoc_core_pd_notifier(struct notifier_block *nb,
	unsigned long action, void *data)
{
	switch (action) {
	case GENPD_NOTIFY_ON:
		complete(&offload_data->aoc_core_pd_power_on);
		break;
	case GENPD_NOTIFY_OFF:
		complete(&offload_data->aoc_core_pd_power_off);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

void usb_offload_rmem_unmap(struct device *sysdev)
{
	struct iommu_domain	*domain;
	struct reserved_mem	*rmem;
	struct device_node	*np;
	int i, count, ret;

	xhci_goog_restore_dma_ops(sysdev);

	domain = iommu_get_domain_for_dev(sysdev);
	count = of_property_count_elems_of_size(sysdev->of_node, "memory-region",
						sizeof(u32));

	for (i = 0; i < count; i++) {
		if (!domain)
			break;

		np = of_parse_phandle(sysdev->of_node, "memory-region", i);
		if (!np) {
			dev_err(sysdev, "memory-region not found.\n");
			continue;
		}

		rmem = of_reserved_mem_lookup(np);
		if (!rmem) {
			dev_err(sysdev, "rmem lookup failed.\n");
			continue;
		}

		ret = iommu_unmap(domain, rmem->base, rmem->size);
		if (ret < 0)
			dev_err(sysdev, "iommu_numap error: %d\n", ret);
	}
}

static void usb_audio_offload_cleanup(struct usb_hcd *hcd)
{
	struct device		*sysdev = offload_data->dev;
	int timeout = 0;

	if (!offload_data->offload_inited)
		return;

	offload_data->usb_audio_offload = false;
	offload_data->offload_state = false;
	offload_data->offload_inited = false;

	/* We need to clean the pools before releasing the reserved memory */
	hcd_buffer_destroy_cp(hcd);

	if (IS_ENABLED(CONFIG_USB_XHCI_GOOG_DMA))
		usb_offload_rmem_unmap(sysdev);

	of_reserved_mem_device_release(sysdev);

	/* Notification for xhci driver removing */
	usb_host_mode_state_notify(USB_DISCONNECTED);

	pm_runtime_put_sync(offload_data->aoc_core_pd);

	timeout = wait_for_completion_timeout(&offload_data->aoc_core_pd_power_off,
		msecs_to_jiffies(AOC_CORE_POWER_CTRL_TIMEOUT));
	if (timeout == 0)
		dev_err(sysdev->parent, "timed out waiting for aoc_core_pd to power off\n");

	dev_pm_genpd_remove_notifier(offload_data->aoc_core_pd);

	dev_pm_domain_detach(offload_data->aoc_core_pd, false);

	__pm_relax(offload_data->wakelock);
}

int usb_offload_rmem_init(struct device *sysdev)
{
	struct iommu_domain	*domain;
	struct reserved_mem	*rmem;
	struct device_node	*np;
	int i, count, ret;

	domain = iommu_get_domain_for_dev(sysdev);
	if (!domain) {
		dev_err(sysdev, "iommu domain not found.\n");
		ret = -ENOMEM;
		goto release_rmem;
	}

	xhci_goog_rmem_setup_latecall(sysdev);

	for (i = 0; i < XHCI_GOOG_DMA_RMEM_MAX; i++) {
		ret = of_reserved_mem_device_init_by_idx(sysdev, sysdev->of_node, i);
		if (ret) {
			dev_err(sysdev, "Could not get reserved memory index %d\n", i);
			goto release_rmem;
		}
	}

	count = of_property_count_elems_of_size(sysdev->of_node, "memory-region",
						sizeof(u32));

	for (i = 0; i < count; i++) {
		np = of_parse_phandle(sysdev->of_node, "memory-region", i);
		if (!np) {
			dev_err(sysdev, "memory-region not found\n");
			ret = -ENOMEM;
			goto unmap_iommu;
		}

		rmem = of_reserved_mem_lookup(np);
		if (!rmem) {
			dev_err(sysdev, "rmem lookup failed.\n");
			ret = -ENOMEM;
			goto unmap_iommu;
		}

		ret = iommu_map(domain, rmem->base, rmem->base, rmem->size,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret < 0) {
			dev_err(sysdev, "iommu_map error: %d\n", ret);
			goto unmap_iommu;
		}
	}

	xhci_goog_setup_dma_ops(sysdev);

	return 0;

unmap_iommu:
	if (i > 0) {
		for (i = 0; i < count - 1; i++) {
			np = of_parse_phandle(sysdev->of_node, "memory-region", i);
			if (!np)
				continue;

			rmem = of_reserved_mem_lookup(np);
			if (!rmem)
				continue;

			iommu_unmap(domain, rmem->base, rmem->size);
		}
	}

release_rmem:
	of_reserved_mem_device_release(sysdev);

	return ret;
}

static int usb_audio_offload_init(struct device *dev)
{
	int ret;
	u32 out_val;

	if (!of_property_read_u32(dev->of_node, "offload", &out_val))
		offload_data->usb_audio_offload = (out_val == 1) ? true : false;

	offload_data->aoc_core_pd = dev_pm_domain_attach_by_name(dev->parent, "aoc_core_pd");
	if (!offload_data->aoc_core_pd) {
		dev_err(dev->parent, "Couldn't attach power domain aoc_core_pd\n");
		return -EINVAL;
	}

	offload_data->aoc_core_nb.notifier_call = aoc_core_pd_notifier;

	init_completion(&offload_data->aoc_core_pd_power_on);
	init_completion(&offload_data->aoc_core_pd_power_off);

	ret = dev_pm_genpd_add_notifier(offload_data->aoc_core_pd, &offload_data->aoc_core_nb);
	if (ret) {
		dev_err(dev->parent, "failed to add genpd notifier on aoc_core_pd, ret = %d\n",
			ret);
		goto err_genpd_add_notifier;
	}

	ret = pm_runtime_get_sync(offload_data->aoc_core_pd);
	if (ret < 0) {
		dev_err(dev->parent, "failed to call pm_runtime_get_sync on %s, ret = %d\n",
			dev_name(offload_data->aoc_core_pd), ret);
		goto err_pm_get;
	}

	if (!pm_runtime_active(offload_data->aoc_core_pd)) {
		ret = wait_for_completion_timeout(&offload_data->aoc_core_pd_power_on,
						msecs_to_jiffies(AOC_CORE_POWER_CTRL_TIMEOUT));
		if (ret == 0) {
			dev_err(dev->parent, "timed out waiting for aoc_core_pd to power on\n");
			ret = -ETIMEDOUT;
			goto err_aoc_core_pd_power_on;
		}
	}

	if (IS_ENABLED(CONFIG_USB_XHCI_GOOG_DMA))
		ret = usb_offload_rmem_init(dev);
	else
		ret = of_reserved_mem_device_init(dev);

	if (ret) {
		dev_err(dev, "Could not get reserved memory\n");
		goto err_rmem_init;
	}

	/* Notification for xhci driver probing */
	usb_host_mode_state_notify(USB_CONNECTED);

	offload_data->offload_state = false;
	offload_data->usb_audio_count = 0;
	offload_data->dev = dev;

	xhci_setup_done();

	__pm_stay_awake(offload_data->wakelock);

	offload_data->offload_inited = true;

	return 0;

err_rmem_init:
err_aoc_core_pd_power_on:
	pm_runtime_put_sync(offload_data->aoc_core_pd);

err_pm_get:
	dev_pm_genpd_remove_notifier(offload_data->aoc_core_pd);
err_genpd_add_notifier:
	dev_pm_domain_detach(offload_data->aoc_core_pd, false);

	return ret;
}


static int xhci_udev_notify(struct notifier_block *self, unsigned long action,
			    void *data)
{
	struct usb_device	*udev;
	struct usb_bus		*ubus;
	struct usb_hcd		*hcd;
	int ret;

	switch (action) {
	case USB_DEVICE_ADD:
		udev = data;
		if (is_root_hub(udev)) {
			udev->quirks = udev->quirks | USB_QUIRK_SHORT_SET_ADDRESS_REQ_TIMEOUT;
			xhci_set_early_stop(udev);
		} else if (is_compatible_with_usb_audio_offload(udev)) {
			dev_dbg(&udev->dev, "Compatible with usb audio offload\n");
			offload_data->usb_audio_count++;
			xhci_sync_conn_stat(udev->bus->busnum, udev->devnum, udev->slot_id,
					    USB_CONNECTED);

		}
		adjust_remote_wakeup(udev, true);
		break;
	case USB_DEVICE_REMOVE:
		udev = data;
		if (is_compatible_with_usb_audio_offload(udev)) {
			offload_data->usb_audio_count--;
			xhci_sync_conn_stat(udev->bus->busnum, udev->devnum, udev->slot_id,
					    USB_DISCONNECTED);
		}
		adjust_remote_wakeup(udev, false);
		break;
	case USB_BUS_ADD:
		ubus = data;
		if (ubus->busnum == 1) {
			ret = usb_audio_offload_init(ubus->sysdev);
			if (ret) {
				dev_err(ubus->sysdev, "offload init failed, ret = %d\n", ret);
				return ret;
			}
		}
		break;
	case USB_BUS_REMOVE:
		ubus = data;
		if (ubus->busnum == 1) {
			hcd = bus_to_hcd(ubus);
			usb_audio_offload_cleanup(hcd);
		}
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block xhci_udev_nb = {
	.notifier_call = xhci_udev_notify,
};

int usb_offload_helper_init(void)
{
	offload_data = kzalloc(sizeof(struct usb_offload_data), GFP_KERNEL);
	if (!offload_data)
		return -ENOMEM;

	offload_data->wakelock = wakeup_source_register(NULL, "usb_offload");
	usb_register_notify(&xhci_udev_nb);

	return 0;
}

void usb_offload_helper_exit(void)
{
	usb_unregister_notify(&xhci_udev_nb);
	wakeup_source_unregister(offload_data->wakelock);
	kfree(offload_data);
	offload_data = NULL;
}
