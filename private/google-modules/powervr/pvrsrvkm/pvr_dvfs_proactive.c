/*************************************************************************/ /*!
@File           pvr_dvfs_proactive.c
@Title          PowerVR devfreq for proactive DVFS
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    PDVFS-specific devfreq code
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/


#if !defined(NO_HARDWARE)

#include <linux/version.h>
#include <linux/device.h>
#include <drm/drm.h>
#if defined(CONFIG_PM_OPP)
#include <linux/pm_opp.h>
#endif

#include "pvrsrv.h"

/*
 * Common DVFS support code shared between SUPPORT_LINUX_DVFS and
 * SUPPORT_PDVFS, primarily for OPP table support.
 *
 * Note that PDVFS implements the Linux/OS devfreq module in
 * the firmware, so no devfreq API calls should be used here.
 */
#include "pvr_dvfs.h"
#include "pvr_dvfs_common.h"
#include "pvr_dvfs_proactive.h"
#include "pvrsrv_device.h"
#include "rgxpdvfs.h"
#include "sofunc_rgx.h"

#include "kernel_compatibility.h"

/*************************************************************************/ /*!
@Function       InitPDVFS

@Description    Initialise the device for Proactive DVFS support.
                Prepares the OPP table from the devicetree, if enabled.

@Input          psDeviceNode       Device node
@Return			PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR InitPDVFS(PPVRSRV_DEVICE_NODE psDeviceNode)
{
#if !(defined(CONFIG_PM_OPP) && defined(CONFIG_OF))
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);

	return PVRSRV_OK;
#else
	IMG_DVFS_DEVICE_CFG *psDVFSDeviceCfg = NULL;
	IMG_PDVFS_DEVICE *psPDVFSDevice;
	struct device *psDev;
	PVRSRV_ERROR eError;
	int err;

	if (!psDeviceNode)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	PVR_ASSERT(psDeviceNode->psDevConfig);

	psDev = psDeviceNode->psDevConfig->pvOSDevice;
	psDVFSDeviceCfg = &psDeviceNode->psDevConfig->sDVFS.sDVFSDeviceCfg;
	psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	/* Setup the OPP table from the device tree for Proactive DVFS. */
	if (dev_pm_opp_get_opp_count(psDev) <= 0)
	{
		err = dev_pm_opp_of_add_table(psDev);
	}
	else
	{
		err = 0;
	}

	if (err == 0)
	{
		psDVFSDeviceCfg->bDTConfig = IMG_TRUE;
	}
	else
	{
		/*
		 * If there are no device tree or system layer provided operating points
		 * then return an error
		 */
		if (psDVFSDeviceCfg->pasOPPTable)
		{
			psDVFSDeviceCfg->bDTConfig = IMG_FALSE;
		}
		else
		{
			PVR_DPF((PVR_DBG_ERROR, "No system or device tree opp points found, %d", err));
			return PVRSRV_ERROR_RESOURCE_UNAVAILABLE;
		}
	}

	psPDVFSDevice->sParams.i64HeadroomInHz = 0;
	psPDVFSDevice->sParams.ui32PollingIntervalInus = 20 * 1000;
	psPDVFSDevice->sParams.ui32UpThresholdInPct = 90;
	psPDVFSDevice->sParams.ui32DownDifferentialInPct = 5;

	eError = SORgxGpuUtilStatsRegister(&psPDVFSDevice->hGpuUtilUserDVFS);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	return PVRSRV_OK;
#endif
}

/*************************************************************************/ /*!
@Function       DeinitPDVFS

@Description    De-Initialise the device for Proactive DVFS support.

@Input          psDeviceNode       Device node
@Return			PVRSRV_ERROR
*/ /**************************************************************************/
void DeinitPDVFS(PPVRSRV_DEVICE_NODE psDeviceNode)
{
#if !(defined(CONFIG_PM_OPP) && defined(CONFIG_OF))
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
#else
	IMG_DVFS_DEVICE_CFG *psDVFSDeviceCfg = NULL;
	IMG_PDVFS_DEVICE *psPDVFSDevice;
	struct device *psDev = NULL;

	/* Check the device exists */
	if (!psDeviceNode)
	{
		return;
	}

	psDev = psDeviceNode->psDevConfig->pvOSDevice;
	psDVFSDeviceCfg = &psDeviceNode->psDevConfig->sDVFS.sDVFSDeviceCfg;
	psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	SORgxGpuUtilStatsUnregister(psPDVFSDevice->hGpuUtilUserDVFS);

	if (psDVFSDeviceCfg->bDTConfig)
	{
		/*
		 * Remove OPP entries for this device; only static entries from
		 * the device tree are present.
		 */
		dev_pm_opp_of_remove_table(psDev);
	}

#endif
}

/*************************************************************************/ /*!
@Function       pdvfs_target

@Description    Ignore all requests to change the GPU frequency.

@Return			0 (no error)
*/ /**************************************************************************/
static int pdvfs_target(struct device *dev, unsigned long *requested_freq, u32 flags)
{
	PVRSRV_DEVICE_NODE *psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev));
	PVRSRV_RGXDEV_INFO *psDevInfo;

	if (!psDeviceNode)
	{
		return -ENODEV;
	}

	psDevInfo = psDeviceNode->pvDevice;

#if defined(RGXFW_META_SUPPORT_2ND_THREAD)
	RGXPDVFSCheckCoreClkRateChange(psDevInfo);
#endif
	*requested_freq = psDevInfo->ui32CoreClkRateSnapshot;
	return 0;
}

/*************************************************************************/ /*!
@Function       pdvfs_get_dev_status

@Description    Get frequency and utilization data from the firmware.

@Input          dev               OS device node
@Output         stat              Utilization and current frequency

@Return			0 if the data could be fetched, or an error otherwise.
*/ /**************************************************************************/
static int pdvfs_get_dev_status(struct device *dev, struct devfreq_dev_status *stat)
{
	PVRSRV_DEVICE_NODE *psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev));
	PVRSRV_RGXDEV_INFO *psDevInfo;
	IMG_PDVFS_DEVICE *psPDVFSDevice;
	RGXFWIF_GPU_UTIL_STATS sGPUUtilStats;
	PVRSRV_ERROR eError;

	if (!psDeviceNode)
	{
		return -ENODEV;
	}

	psDevInfo = psDeviceNode->pvDevice;
	psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

#if defined(RGXFW_META_SUPPORT_2ND_THREAD)
	RGXPDVFSCheckCoreClkRateChange(psDevInfo);
#endif
	stat->current_frequency = psDevInfo->ui32CoreClkRateSnapshot;

	if (psDevInfo->pfnGetGpuUtilStats == NULL)
	{
		/* Not yet ready. So set times to something sensible. */
		stat->busy_time = 0;
		stat->total_time = 0;
		return 0;
	}

	eError = psDevInfo->pfnGetGpuUtilStats(psDeviceNode, psPDVFSDevice->hGpuUtilUserDVFS, &sGPUUtilStats);

	if (eError != PVRSRV_OK)
	{
		return -EAGAIN;
	}

	stat->busy_time = sGPUUtilStats.ui64GpuStatActive;
	stat->total_time = sGPUUtilStats.ui64GpuStatCumulative;

	return 0;
}

/*************************************************************************/ /*!
@Function       pdvfs_get_dev_status

@Description    Get frequency from the firmware.

@Input          dev               OS device node
@Output         freq              The current frequency

@Return			0 if the data could be fetched, or an error otherwise.
*/ /**************************************************************************/
static int pdvfs_get_cur_freq(struct device *dev, unsigned long *freq)
{
	PVRSRV_DEVICE_NODE *psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev));
	PVRSRV_RGXDEV_INFO *psDevInfo;

	if (!psDeviceNode)
	{
		return -ENODEV;
	}

	psDevInfo = psDeviceNode->pvDevice;

	if (!psDevInfo)
	{
		return -ENOTSUPP;
	}

#if defined(RGXFW_META_SUPPORT_2ND_THREAD)
	RGXPDVFSCheckCoreClkRateChange(psDevInfo);
#endif
	*freq = psDevInfo->ui32CoreClkRateSnapshot;

	return 0;
}

/* This function is available as a symbol but the prototype lives in
 * drivers/devfreq/governor.h which is not visible to modules.
 */
void devfreq_get_freq_range(struct devfreq *devfreq,
			    unsigned long *min_freq,
			    unsigned long *max_freq);

/*************************************************************************/ /*!
@Function       pdvfs_notifier

@Description    Notify PDVFS that parameters have changed.

@Input          nb                psPDVFSDevice->psDevFreq
@Input          action            DEVFREQ_PRECHANGE
@Input          data              a pointer to struct devfreq_freqs

@Return			0 on success, an error otherwise
*/ /**************************************************************************/
static int pdvfs_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	IMG_PDVFS_DEVICE *psPDVFSDevice = container_of(nb, IMG_PDVFS_DEVICE, sNotifierBlock);
	PVRSRV_DVFS *psDVFS = container_of(psPDVFSDevice, PVRSRV_DVFS, sPDVFSDevice);
	PVRSRV_DEVICE_CONFIG *psDeviceConfig = container_of(psDVFS, struct _PVRSRV_DEVICE_CONFIG_, sDVFS);
	PVRSRV_RGXDEV_INFO *psDevInfo = psDeviceConfig->psDevNode->pvDevice;
	PVRSRV_DEVICE_NODE *psDeviceNode = psDeviceConfig->psDevNode;
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_DEV_POWER_STATE ePowerState;

	devfreq_get_freq_range(psPDVFSDevice->psDevFreq, &psPDVFSDevice->ulMinFreq, &psPDVFSDevice->ulMaxFreq);

	eError = PVRSRVPowerLock(psDeviceNode);
	if (eError != PVRSRV_OK)
	{
		goto err_nolock;
	}

	eError = PVRSRVGetDevicePowerState(psDeviceNode, &ePowerState);
	if (eError != PVRSRV_OK)
	{
		goto err_lock;
	}

	if (ePowerState == PVRSRV_DEV_POWER_STATE_ON)
	{
		PDVFSLimitMinFrequency(psDevInfo, psPDVFSDevice->ulMinFreq);
		PDVFSLimitMaxFrequency(psDevInfo, psPDVFSDevice->ulMaxFreq);
	}

	PVRSRVPowerUnlock(psDeviceNode);
	return 0;
err_lock:
	PVRSRVPowerUnlock(psDeviceNode);
err_nolock:
	return -EAGAIN;
}


/*************************************************************************/ /*!
@Function       set_pdvfs_params

@Description    Send PDVFS parameters to the device, if it is on

@Input          psDeviceNode      device node
*/ /**************************************************************************/
static void set_pdvfs_params(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVRSRV_RGXDEV_INFO *psDevInfo = psDeviceNode->pvDevice;
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	PVRSRV_ERROR eLockError, eError;
	PVRSRV_DEV_POWER_STATE ePowerState;

	eLockError = PVRSRVPowerLock(psDeviceNode);
	if (eLockError != PVRSRV_OK)
	{
		return;
	}

	eError = PVRSRVGetDevicePowerState(psDeviceNode, &ePowerState);
	if (eError != PVRSRV_OK)
	{
		struct device *psDev = psDeviceNode->psDevConfig->pvOSDevice;
		dev_warn(psDev, "Device power state unknown in PDVFS startup");
		goto exit_lock;
	}

	if (ePowerState != PVRSRV_DEV_POWER_STATE_ON)
	{
		/* Settings will be applied next DVFS resume. */
		goto exit_lock;
	}

	PDVFSSetParams(psDevInfo, &psPDVFSDevice->sParams);

exit_lock:
	PVRSRVPowerUnlock(psDeviceNode);
}

/*************************************************************************/ /*!
@Function       capacity_headroom_show

@Description    Report the current headroom to userspace

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to write into

@Return			the number of bytes printed into buf, or error
*/ /**************************************************************************/
static ssize_t capacity_headroom_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	PVRSRV_DEVICE_NODE *psDeviceNode =
		PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
		psPDVFSDevice->sParams.i64HeadroomInHz);
}

/*************************************************************************/ /*!
@Function       capacity_headroom_show

@Description    Update capacity headroom and notify the firmware if it's on

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to read from
@Input          count             number of valid bytes in buf

@Return			the number of bytes read from buf, or error
*/ /**************************************************************************/
static ssize_t capacity_headroom_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	PVRSRV_DEVICE_NODE	*psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(
		GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	IMG_INT64		i64HeadroomInHz;

	if (kstrtoll(buf, 0, &i64HeadroomInHz))
	{
		return -EINVAL;
	}

	psPDVFSDevice->sParams.i64HeadroomInHz = i64HeadroomInHz;

	set_pdvfs_params(psDeviceNode);

	return count;
}

static DEVICE_ATTR_RW(capacity_headroom);

/*************************************************************************/ /*!
@Function       polling_interval_show

@Description    Report the current reactive polling interval to userspace

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to write into

@Return			the number of bytes printed into buf, or error
*/ /**************************************************************************/
static ssize_t polling_interval_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	PVRSRV_DEVICE_NODE *psDeviceNode =
		PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
		psPDVFSDevice->sParams.ui32PollingIntervalInus);
}

/*************************************************************************/ /*!
@Function       polling_interval_store

@Description    Update polling interval and notify the firmware if it's on

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to read from
@Input          count             number of valid bytes in buf

@Return			the number of bytes read from buf, or error
*/ /**************************************************************************/
static ssize_t polling_interval_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	PVRSRV_DEVICE_NODE	*psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(
		GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	IMG_UINT32		ui32PollingIntervalInus;

	if (kstrtouint(buf, 0, &ui32PollingIntervalInus))
	{
		return -EINVAL;
	}

	psPDVFSDevice->sParams.ui32PollingIntervalInus = ui32PollingIntervalInus;

	set_pdvfs_params(psDeviceNode);

	return count;
}

static DEVICE_ATTR_RW(polling_interval);

static IMG_BOOL validate_simpleondemand_parameters(
	IMG_UINT32 ui32UpThresholdInPct,
	IMG_UINT32 ui32DownDifferentialInPct)
{
	return (ui32UpThresholdInPct <= 100 &&
		ui32DownDifferentialInPct <= ui32UpThresholdInPct);
}

/*************************************************************************/ /*!
@Function       up_threshold_show

@Description    Report the current reactive up threshold to userspace

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to write into

@Return			the number of bytes printed into buf, or error
*/ /**************************************************************************/
static ssize_t up_threshold_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	PVRSRV_DEVICE_NODE *psDeviceNode =
		PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
		psPDVFSDevice->sParams.ui32UpThresholdInPct);
}

/*************************************************************************/ /*!
@Function       up_threshold_store

@Description    Update up threshold and notify the firmware if it's on

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to read from
@Input          count             number of valid bytes in buf

@Return			the number of bytes read from buf, or error
*/ /**************************************************************************/
static ssize_t up_threshold_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	PVRSRV_DEVICE_NODE	*psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(
		GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	IMG_UINT32		ui32UpThresholdInPct;

	if (kstrtouint(buf, 0, &ui32UpThresholdInPct))
	{
		return -EINVAL;
	}

	if (!validate_simpleondemand_parameters(
		ui32UpThresholdInPct,
		psPDVFSDevice->sParams.ui32DownDifferentialInPct))
	{
		dev_warn(dev, "Invalid up threshold (%u%%) "
			"and down differential (%u%%)",
			ui32UpThresholdInPct,
			psPDVFSDevice->sParams.ui32DownDifferentialInPct);
		return -EINVAL;
	}

	psPDVFSDevice->sParams.ui32UpThresholdInPct = ui32UpThresholdInPct;

	set_pdvfs_params(psDeviceNode);

	return count;
}

static DEVICE_ATTR_RW(up_threshold);

/*************************************************************************/ /*!
@Function       down_differential_show

@Description    Report the current reactive down differential to userspace

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to write into

@Return			the number of bytes printed into buf, or error
*/ /**************************************************************************/
static ssize_t down_differential_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	PVRSRV_DEVICE_NODE *psDeviceNode =
		PVRSRVGetDeviceInstanceByKernelDevID(GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;

	return scnprintf(buf, PAGE_SIZE, "%u\n",
		psPDVFSDevice->sParams.ui32DownDifferentialInPct);
}

/*************************************************************************/ /*!
@Function       down_differential_store

@Description    Update down differential and notify the firmware if it's on

@Input          dev               OS device node (child of devfreq node)
@Input          attr              the device attributes (unused)
@Input          buf               the buffer to read from
@Input          count             number of valid bytes in buf

@Return			the number of bytes read from buf, or error
*/ /**************************************************************************/
static ssize_t down_differential_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	PVRSRV_DEVICE_NODE	*psDeviceNode = PVRSRVGetDeviceInstanceByKernelDevID(
		GetDevID(dev->parent));
	IMG_PDVFS_DEVICE *psPDVFSDevice =
		&psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	IMG_UINT32		ui32DownDifferentialInPct;

	if (kstrtouint(buf, 0, &ui32DownDifferentialInPct))
	{
		return -EINVAL;
	}

	if (!validate_simpleondemand_parameters(
		psPDVFSDevice->sParams.ui32UpThresholdInPct,
		ui32DownDifferentialInPct))
	{
		dev_warn(dev, "Invalid up threshold (%u%%) "
			"and down differential (%u%%)",
			psPDVFSDevice->sParams.ui32UpThresholdInPct,
			ui32DownDifferentialInPct);
		return -EINVAL;
	}

	psPDVFSDevice->sParams.ui32DownDifferentialInPct = ui32DownDifferentialInPct;

	set_pdvfs_params(psDeviceNode);

	return count;
}

static DEVICE_ATTR_RW(down_differential);

/*************************************************************************/ /*!
@Function       RegisterParameterFiles

@Description    Add new devices to devfreq for reading/writing PDVFS parameters

@Input          devfreq            OS devfreq node
*/ /**************************************************************************/
void RegisterParameterFiles(struct devfreq *devfreq)
{
	IMG_BOOL bMadeSysfsNodes =
		!sysfs_create_file(&devfreq->dev.kobj, &dev_attr_capacity_headroom.attr) &&
		!sysfs_create_file(&devfreq->dev.kobj, &dev_attr_polling_interval.attr) &&
		!sysfs_create_file(&devfreq->dev.kobj, &dev_attr_up_threshold.attr) &&
		!sysfs_create_file(&devfreq->dev.kobj, &dev_attr_down_differential.attr);

	/*
	 * If these nodes aren't present, userland services will fail to
	 * initialise, causing erratic behavior that can be hard to diagnose.
	 *
	 * Also, failing to create the file indicates something is fatally
	 * wrong (kernel OOM for example).
	 */
	BUG_ON(!bMadeSysfsNodes);
}

/*************************************************************************/ /*!
@Function       UnregisterParameterFiles

@Description    Remove the devices added by RegisterParameterFiles from devfreq

@Input          devfreq            OS devfreq node
*/ /**************************************************************************/
void UnregisterParameterFiles(struct devfreq *devfreq)
{
	sysfs_remove_file(&devfreq->dev.kobj, &dev_attr_capacity_headroom.attr);
	sysfs_remove_file(&devfreq->dev.kobj, &dev_attr_polling_interval.attr);
	sysfs_remove_file(&devfreq->dev.kobj, &dev_attr_up_threshold.attr);
	sysfs_remove_file(&devfreq->dev.kobj, &dev_attr_down_differential.attr);
}

/*************************************************************************/ /*!
@Function       ResumePDVFS

@Description    Restore firmware state controlled by the DVFS governor after power on.

@Input          psDeviceNode       Device node
*/ /**************************************************************************/
PVRSRV_ERROR ResumePDVFS(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	IMG_PDVFS_DEVICE *psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	PVRSRV_RGXDEV_INFO *psDevInfo = psDeviceNode->pvDevice;
	struct device *psDev = psDeviceNode->psDevConfig->pvOSDevice;
	PVRSRV_ERROR eError;
	PVRSRV_DEV_POWER_STATE ePowerState;

	eError = PVRSRVGetDevicePowerState(psDeviceNode, &ePowerState);
	if (eError != PVRSRV_OK)
	{
		dev_warn(psDev, "Device power state unknown in PDVFS startup, error %d", eError);
		return eError;
	}

	if (ePowerState == PVRSRV_DEV_POWER_STATE_ON)
	{
		PDVFSLimitMinFrequency(psDevInfo, psPDVFSDevice->ulMinFreq);
		PDVFSLimitMaxFrequency(psDevInfo, psPDVFSDevice->ulMaxFreq);
		PDVFSSetParams(psDevInfo, &psPDVFSDevice->sParams);
	}

	return PVRSRV_OK;
}

#if defined(CONFIG_DEVFREQ_THERMAL)
static int RegisterCoolingDevice(struct device *dev,
					IMG_PDVFS_DEVICE *psPDVFSDevice,
					struct devfreq_cooling_power *powerOps)
{
	struct device_node *of_node;
	int err = 0;

	if (!psPDVFSDevice)
	{
		return -EINVAL;
	}

	if (!powerOps)
	{
		dev_info(dev, "Cooling: power ops not registered, not enabling cooling");
		return 0;
	}

	of_node = of_node_get(dev->of_node);

	psPDVFSDevice->psDevfreqCoolingDevice = of_devfreq_cooling_register_power(
		of_node, psPDVFSDevice->psDevFreq, powerOps);

	if (IS_ERR(psPDVFSDevice->psDevfreqCoolingDevice))
	{
		err = PTR_ERR(psPDVFSDevice->psDevfreqCoolingDevice);
		dev_err(dev, "Failed to register as devfreq cooling device %d", err);
	}

	of_node_put(of_node);

	return err;
}
#endif

/*************************************************************************/ /*!
@Function       RegisterPDVFSDevice

@Description    Initialise the devfreq entries for Proactive DVFS.

@Input          psDeviceNode       Device node
@Return			PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR RegisterPDVFSDevice(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	static struct devfreq_dev_profile dev_profile =
	{
		.target = pdvfs_target,
		.get_dev_status = pdvfs_get_dev_status,
		.get_cur_freq = pdvfs_get_cur_freq
	};

	IMG_PDVFS_DEVICE *psPDVFSDevice = NULL;
#if defined(CONFIG_DEVFREQ_THERMAL)
	IMG_DVFS_DEVICE_CFG *psDVFSDeviceCfg = NULL;
#endif
	struct device *psDev;
	struct pvr_opp_freq_table pvr_freq_table = {0};
	unsigned long min_freq = 0, max_freq = 0, min_volt = 0;
	PVRSRV_ERROR eError;
	int err;

	if (!psDeviceNode)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDev = psDeviceNode->psDevConfig->pvOSDevice;
	psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
#if defined(CONFIG_DEVFREQ_THERMAL)
	psDVFSDeviceCfg = &psDeviceNode->psDevConfig->sDVFS.sDVFSDeviceCfg;
#endif

	err = GetOPPValues(psDev, &min_freq, &min_volt, &max_freq, &pvr_freq_table);
	if (err)
	{
		PVR_DPF((PVR_DBG_ERROR, "Failed to read OPP points, %d", err));
		eError = TO_IMG_ERR(err);
		goto err_exit;
	}

	dev_profile.freq_table = pvr_freq_table.freq_table;
	dev_profile.max_state = pvr_freq_table.num_levels;
	dev_profile.initial_freq = min_freq;
	dev_profile.polling_ms = 0;

	psPDVFSDevice->psDevFreq = devm_devfreq_add_device(
		psDev,
		&dev_profile,
		"userspace",
		NULL);

	if (IS_ERR(psPDVFSDevice->psDevFreq))
	{
		PVR_DPF((PVR_DBG_ERROR,
				 "Failed to add as devfreq device %p, %ld",
				 psPDVFSDevice->psDevFreq,
				 PTR_ERR(psPDVFSDevice->psDevFreq)));
		eError = TO_IMG_ERR(PTR_ERR(psPDVFSDevice->psDevFreq));
		goto err_exit;
	}

	psPDVFSDevice->sNotifierBlock.notifier_call = pdvfs_notifier;
	psPDVFSDevice->sNotifierBlock.priority = 0;

	devfreq_get_freq_range(psPDVFSDevice->psDevFreq, &psPDVFSDevice->ulMinFreq, &psPDVFSDevice->ulMaxFreq);

	if (devm_devfreq_register_notifier(psDev, psPDVFSDevice->psDevFreq, &psPDVFSDevice->sNotifierBlock, DEVFREQ_TRANSITION_NOTIFIER)) {
		PVR_DPF((PVR_DBG_ERROR, "Failed to add devfreq noifier block"));
		eError = TO_IMG_ERR(PTR_ERR(psPDVFSDevice->psDevFreq));
		goto err_exit;
	}

#if defined(CONFIG_DEVFREQ_THERMAL)
	if (!PVRSRV_VZ_MODE_IS(GUEST, DEVNODE, psDeviceNode))
	{
		err = RegisterCoolingDevice(psDev, psPDVFSDevice, psDVFSDeviceCfg->psPowerOps);
		if (err)
		{
			eError = TO_IMG_ERR(err);
			goto err_exit;
		}
	}
#endif

	RegisterParameterFiles(psPDVFSDevice->psDevFreq);

	return PVRSRV_OK;

err_exit:
	UnregisterPDVFSDevice(psDeviceNode);
	return eError;
}

/*************************************************************************/ /*!
@Function       UnregisterPDVFSDevice

@Description    Remove the devfreq entries for Proactive DVFS.

@Input          psDeviceNode       Device node
@Return			PVRSRV_ERROR
*/ /**************************************************************************/
PVRSRV_ERROR UnregisterPDVFSDevice(PPVRSRV_DEVICE_NODE psDeviceNode)
{
	IMG_PDVFS_DEVICE *psPDVFSDevice = NULL;
	struct device *psDev;

	if (!psDeviceNode)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psPDVFSDevice = &psDeviceNode->psDevConfig->sDVFS.sPDVFSDevice;
	psDev = psDeviceNode->psDevConfig->pvOSDevice;

	if (!psPDVFSDevice->psDevFreq)
	{
		return PVRSRV_OK;
	}

#if defined(CONFIG_DEVFREQ_THERMAL)
	if (!IS_ERR_OR_NULL(psPDVFSDevice->psDevfreqCoolingDevice))
	{
		devfreq_cooling_unregister(psPDVFSDevice->psDevfreqCoolingDevice);
		psPDVFSDevice->psDevfreqCoolingDevice = NULL;
	}
#endif

	UnregisterParameterFiles(psPDVFSDevice->psDevFreq);

	devm_devfreq_unregister_notifier(psDev, psPDVFSDevice->psDevFreq, &psPDVFSDevice->sNotifierBlock, DEVFREQ_TRANSITION_NOTIFIER);

	devm_devfreq_remove_device(psDev, psPDVFSDevice->psDevFreq);
	psPDVFSDevice->psDevFreq = NULL;

	return PVRSRV_OK;
}

#endif /* !NO_HARDWARE */
