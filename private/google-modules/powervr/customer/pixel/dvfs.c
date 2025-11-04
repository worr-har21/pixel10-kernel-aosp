/* SPDX-License-Identifier: GPL-2.0 */

#include <customer/volcanic/customer_dvfs.h>

#include <pvrsrvkm/rgxfwutils.h>
#include <pvrsrvkm/volcanic/rgx_options.h>


PVRSRV_ERROR pixel_fw_dvfs_set_rate(PVRSRV_RGXDEV_INFO *info, uint32_t rate)
{
	PCPVRSRV_DEVICE_NODE dev_node = info->psDeviceNode;
	/* Send opp instruction via generic platform cmd */
	RGXFWIF_KCCB_CMD cmd = {
		.eCmdType = RGXFWIF_KCCB_CMD_PLATFORM_CMD,
		.uCmdData.sPlatformData = {
			.ui32PlatformCmd = PIXEL_RGXFWIF_PLATFORM_CMD_DVFS_SET_RATE,
			.cmd_data.set_rate.opp = rate,
		},
	};
	PVRSRV_ERROR error = PVRSRV_OK;

	PVR_ASSERT(PVRSRVPwrLockIsLockedByMe(dev_node));

	PVRSRV_VZ_RET_IF_MODE(GUEST, DEVNODE, dev_node, PVRSRV_ERROR_NOT_SUPPORTED);

	/* Submit command to the firmware */
	LOOP_UNTIL_TIMEOUT_US(MAX_HW_TIME_US)
	{
		error = RGXSendCommand(info, &cmd, PDUMP_FLAGS_NONE);

		if (!PVRSRVIsRetryError(error))
			break;

		OSWaitus(MAX_HW_TIME_US / WAIT_TRY_COUNT);
	}
	END_LOOP_UNTIL_TIMEOUT_US();

	PVR_LOG_IF_ERROR_VA(PVR_DBG_ERROR, error, "%s failed", __func__);

	return error;
}
