// SPDX-License-Identifier: GPL-2.0

#include "of.h"
#include "sysconfig.h"

#include <pvrsrvkm/volcanic/rgxdevice.h>

#include <linux/of_address.h>
#include <linux/of_platform.h>

#if defined(CONFIG_POWERVR_DISABLE_APM)
#define disable_apm (true)
#else
#define disable_apm (false)
#endif

static struct _of_phandles {
	struct device_node *gpu_core_logic_pd_node;
#if defined(SUPPORT_TRUSTED_DEVICE)
	struct device_node *gpu_memory_region_node;
#endif
} of_phandles;

static uint32_t read_u32_property_or_default(struct device *dev,
					     uint32_t default_value,
					     const char *path[],
					     size_t path_elements)
{
	uint32_t value = default_value;
	int last_element = path_elements - 1;
	struct device_node *node = dev->of_node;
	const char *property_name = path[last_element];
	int return_code;
	struct device_node *final_node;

	for (int index = 0; index < last_element; ++index) {
		struct device_node *last = node;
		node = of_get_child_by_name(last, path[index]);

		if (node == NULL) {
			dev_dbg(dev,
				"%s: unable to follow path to DT property %s "
				"(failed to find child '%s' on node '%s')",
				__func__, property_name, path[index],
				last->name);
			final_node = last;
			goto end;
		}

		if (last != dev->of_node) {
			of_node_put(last);
		}

	}

	return_code =
		of_property_read_u32_index(node, property_name, 0, &value);
	final_node = node;

	switch (return_code) {
	case 0:
		break;
	case -EINVAL:
		dev_dbg(dev, "%s: DT property %s does not exist", __func__,
			property_name);
		break;
	case -ENODATA:
		dev_dbg(dev, "%s: DT property %s does not have a value",
			__func__, property_name);
		break;
	case -EOVERFLOW:
		dev_dbg(dev,
			"%s: DT property %s has data that isn't large enough",
			__func__, property_name);
		break;
	default:
		dev_dbg(dev, "%s: DT property %s has unknown return code %d",
			__func__, property_name, return_code);
		break;
	}

end:
	if (final_node != dev->of_node) {
		of_node_put(final_node);
	}

	return value;
}

#define READ_U32_PROPERTY_OR_DEFAULT(DEV, DEFAULT_VALUE, ...) \
	(read_u32_property_or_default(DEV, DEFAULT_VALUE, (const char *[]){ __VA_ARGS__ }, \
				      sizeof((const char *[]){ __VA_ARGS__ }) / \
				      sizeof(const char *)))

#if defined(SUPPORT_TRUSTED_DEVICE)
int fill_carveout_resource(struct resource *carveout)
{
	/* Get the resource associated with the OF node */
	int rc = of_address_to_resource(of_phandles.gpu_memory_region_node, 0, carveout);

	of_node_put(of_phandles.gpu_memory_region_node);
	of_phandles.gpu_memory_region_node = NULL;

	return rc;
}
#endif

int init_pixel_of(struct pixel_gpu_device *pixel_dev)
{
	struct device *dev = pixel_dev->dev;
	struct pixel_of_properties *of_properties = &pixel_dev->of_properties;
	struct pixel_of_pdevs *of_pdevs = &pixel_dev->of_pdevs;
	RGX_TIMING_INFORMATION *timing_info =
		((RGX_DATA *)pixel_dev->dev_config->hDevData)->psRGXTimingInfo;
	struct device_node *gpu_pf_state_node;

	// Setup prerequisite data
	gpu_pf_state_node = of_parse_phandle(dev->of_node, "clocks", 0);
	if (!gpu_pf_state_node) {
		dev_err(dev, "failed to find clocks node");
		goto exit_error;
	}

	of_phandles.gpu_core_logic_pd_node =
		of_parse_phandle(dev->of_node, "power-domains",
				 PIXEL_GPU_PM_DOMAIN_GPU_CORE_LOGIC_PD);
	if (!of_phandles.gpu_core_logic_pd_node) {
		dev_err(dev, "failed to find core logic node");
		goto exit_error;
	}

#if defined(SUPPORT_TRUSTED_DEVICE)
	of_phandles.gpu_memory_region_node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!of_phandles.gpu_memory_region_node) {
		dev_err(dev, "failed to find memory-region node");
		goto exit_error;
	}
#endif

	of_pdevs->gpu_pf_state_pdev = of_find_device_by_node(gpu_pf_state_node);
	if (!of_pdevs->gpu_pf_state_pdev) {
		dev_err(dev, "failed to find gpu_pf_state device");
		goto exit_error;
	}
	of_node_put(gpu_pf_state_node);
	gpu_pf_state_node = NULL;

	// Obtain OF config
	of_properties->spu_gating =
		READ_U32_PROPERTY_OR_DEFAULT(dev, true, "config", "power",
					     "enable-firmware-managed-spu-power");

	if (disable_apm) {
		dev_dbg(dev, "APM disabled, overriding device tree");
		of_properties->apm = false;
	} else {
		of_properties->apm =
			READ_U32_PROPERTY_OR_DEFAULT(dev, false, "config", "power",
						     "enable-active-power-management");
	}

	of_properties->pre_silicon = of_property_read_bool(dev->of_node, "pre-silicon");

	of_properties->emulator = of_property_read_bool(dev->of_node, "emulator");

	of_properties->virtual_platform = of_property_read_bool(dev->of_node, "virtual-platform");

	of_properties->jones_force_on = of_property_read_bool(of_phandles.gpu_core_logic_pd_node,
							      "force-on");

	of_properties->apm_latency =
		READ_U32_PROPERTY_OR_DEFAULT(dev, 33, "config", "power",
					     "active-power-management-latency-ms");

	of_properties->autosuspend_latency =
		READ_U32_PROPERTY_OR_DEFAULT(dev, 18, "config", "power",
					     "sswrp-autosuspend-latency-ms");

	// Set any global state based on OF
	if (of_properties->pre_silicon)
		set_time_multiplier(2500);

	timing_info->bEnableRDPowIsland    = of_properties->spu_gating;
	timing_info->bEnableActivePM       = of_properties->apm;
	timing_info->ui32ActivePMLatencyms = of_properties->apm_latency;

#if defined(EMULATOR_OF)
	pixel_dev->dev_config->emulator = of_properties->emulator;
#endif
#if defined(VIRTUAL_PLATFORM_OF)
	pixel_dev->dev_config->virtual_platform = of_properties->virtual_platform;
#endif

	// Perform checks based on OF
	/*
		Jones should not be forced on in silicon under normal circumstances.
		Jones should not be forced on when APM is enabled - there will be no power saving.
	*/
	WARN_ON(of_properties->jones_force_on &&
		(!of_properties->pre_silicon || of_properties->apm));

	// Warn when SPU gating is disabled - this should never be turned off
	WARN_ON(!of_properties->spu_gating);

	// emulator or virtual_platform should only be set for pre_silicon
	WARN_ON(!of_properties->pre_silicon &&
		(of_properties->emulator || of_properties->virtual_platform));

	// emulator and virtual_platform should not both be set
	WARN_ON(of_properties->emulator && of_properties->virtual_platform);

	// Print OF config
	dev_dbg(dev, "DT configuration");
	dev_dbg(dev, "Pre-Silicon Configuration: %d", of_properties->pre_silicon);
	dev_dbg(dev, "Emulator Configuration: %d", of_properties->emulator);
	dev_dbg(dev, "Virtual Platform Configuration: %d", of_properties->virtual_platform);
	dev_dbg(dev, "GPU Power Forced On: %d", of_properties->jones_force_on);
	dev_dbg(dev, "SPU Power Gating: %d", of_properties->spu_gating);
	dev_dbg(dev, "Active Power Management (APM): %d", of_properties->apm);
	dev_dbg(dev, "APM Latency: %u", of_properties->apm_latency);
	dev_dbg(dev, "Autosuspend Latency: %u", of_properties->autosuspend_latency);

	return 0;
exit_error:
	if (gpu_pf_state_node)
		of_node_put(gpu_pf_state_node);
	deinit_pixel_of(pixel_dev);
	return -1;
}

void deinit_pixel_of(struct pixel_gpu_device *pixel_dev)
{
	if (pixel_dev->of_pdevs.gpu_pf_state_pdev)
		put_device(&pixel_dev->of_pdevs.gpu_pf_state_pdev->dev);
#if defined(SUPPORT_TRUSTED_DEVICE)
	if (of_phandles.gpu_memory_region_node)
		of_node_put(of_phandles.gpu_memory_region_node);
#endif
	if (of_phandles.gpu_core_logic_pd_node)
		of_node_put(of_phandles.gpu_core_logic_pd_node);
}
