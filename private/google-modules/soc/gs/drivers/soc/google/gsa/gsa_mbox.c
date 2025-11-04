// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Google LLC.
 */
#include <asm-generic/errno.h>
#include <kunit/visibility.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include "gsa_mbox.h"

/* Number of shared registers  */
#define MBOX_SR_NUM 16

/* Shared register semantics for outgoing messages */
#define MBOX_SR_SEND_CMD_IDX 0
#define MBOX_SR_SEND_ARGC_IDX 1
#define MBOX_SR_SEND_ARGV_IDX 2

/* Shared register semantics for incoming messages */
#define MBOX_SR_RECV_CMD_IDX 0
#define MBOX_SR_RECV_ERR_IDX 1
#define MBOX_SR_RECV_ARGC_IDX 2
#define MBOX_SR_RECV_ARGV_IDX 3

struct gsa_mbox_req {
	u32 cmd;
	u32 argc;
	u32 *args;
};

struct gsa_mbox_rsp {
	u32 cmd;
	u32 err;
	u32 argc;
	u32 *args;
};

static bool bug_on_mbox_error;
module_param(bug_on_mbox_error, bool, 0644);
MODULE_PARM_DESC(bug_on_mbox_error, "Trigger a crash when the GSA mailbox receives an error.");

#if IS_ENABLED(CONFIG_GSA_PKVM)

static void gsa_unlink_s2mpu(void *ctx)
{
	struct gsa_mbox *mb = ctx;

	put_device(mb->s2mpu);
	mb->s2mpu = NULL;
}

static int gsa_link_s2mpu(struct device *dev, struct gsa_mbox *mb)
{
	struct device_node *np;
	struct platform_device *pdev;

	/* We expect "s2mpu" entry in device node to point to gsa s2mpu driver
	 * This entry is absolutely required for normal operation on most
	 * devices.
	 */
	np = of_parse_phandle(dev->of_node, "s2mpu", 0);
	if (!np) {
		dev_err(dev, "no 's2mpu' entry found\n");
		return -ENODEV;
	}

	/* Note: next call obtains additional reference on returned device */
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		dev_err(dev, "no device in 's2mpus' device_node\n");
		return -ENODEV;
	}

	mb->s2mpu = &pdev->dev;
	dev_info(dev, "linked to %s\n", dev_name(mb->s2mpu));

	/* register unlink hook for s2mpu device  */
	return devm_add_action_or_reset(dev, gsa_unlink_s2mpu, mb);
}
#else /* CONFIG_GSA_PKVM */
static int gsa_link_s2mpu(struct device *dev, struct gsa_mbox *mb)
{
	return 0;
}
#endif /* CONFIG_GSA_PKVM */

struct gsa_mbox *gsa_mbox_init(struct platform_device *pdev)
{
	int err = 0;
	size_t i = 0;
	struct gsa_mbox *mb = NULL;
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "Initializing GSA mailbox with Linux framework.\n");

	mb = devm_kzalloc(dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return ERR_PTR(-ENOMEM);

	mb->send_mbox_msg = mbox_send_message;

	err = gsa_link_s2mpu(dev, mb);
	if (err)
		return ERR_PTR(err);

	mb->dev = dev;
	mutex_init(&mb->share_reg_lock);

	/* Initialize the linux mailbox client */
	for (i = 0; i < GSA_MBOX_COUNT; i++) {
		mb->slots[i].client.dev = dev;
		mb->slots[i].client.tx_block = true;
		mb->slots[i].client.tx_tout = 100 /* ms */;
		mb->slots[i].client.knows_txdone = false;
		mb->slots[i].client.rx_callback = mbox_rx_callback;
		mb->slots[i].client.tx_done = mbox_tx_done;
		mb->slots[i].client.tx_prepare = mbox_tx_prepare;

		mb->slots[i].rsp_buffer = NULL;
		init_completion(&mb->slots[i].mbox_cmd_completion);

		mb->slots[i].channel = mbox_request_channel(&mb->slots[i].client, i);
		if (IS_ERR(mb->slots[i].channel)) {
			dev_err(dev, "failed to find mailbox interface %zu : %ld\n", i,
				PTR_ERR(mb->slots[i].channel));
			mb->slots[i].channel = NULL;
			goto mbox_request_failed;
		}
	}

	return mb;
mbox_request_failed:
	/* Deallocate any channels successfully allocated.
	 * Since i failed, that's [0, i-1].
	 */
	for (size_t j = 0; j < i; j++)
		mbox_free_channel(mb->slots[j].channel);
	return ERR_PTR(-EIO);
}

void gsa_mbox_destroy(struct gsa_mbox *mb)
{
	for (size_t i = 0; i < GSA_MBOX_COUNT; i++) {
		mbox_free_channel(mb->slots[i].channel);
	}
}

VISIBLE_IF_KUNIT void mbox_tx_prepare(struct mbox_client *cl, void *mssg)
{
}
EXPORT_SYMBOL_IF_KUNIT(mbox_tx_prepare);

VISIBLE_IF_KUNIT void mbox_tx_done(struct mbox_client *cl, void *mssg, int rc)
{
}
EXPORT_SYMBOL_IF_KUNIT(mbox_tx_done);

/* This functions gets called whenever there is an incoming message from the
 * GSA.
 * Since the GSA only sends a message as a response to a message, this
 * function gets called whenever an outgoing request has finished.
 */
VISIBLE_IF_KUNIT void mbox_rx_callback(struct mbox_client *cl, void *mssg)
{
	/* Get the slot that cl points into. */
	struct mbox_slot *slot = container_of(cl, struct mbox_slot, client);
	/* Store the message for exec_mbox_cmd_sync_locked to pick up. */
	slot->rsp_buffer = (u32 *)mssg;
	/* Signal to exwec_mbox_sync_locked that the message has been
	 * received. */
	complete(&slot->mbox_cmd_completion);
}
EXPORT_SYMBOL_IF_KUNIT(mbox_rx_callback);

static int exec_mbox_cmd_sync_locked(struct gsa_mbox *mb,
				     struct gsa_mbox_req *req,
				     struct gsa_mbox_rsp *rsp)
{
	struct mbox_slot *slot = &mb->slots[0];
	struct mbox_chan *channel = slot->channel;
	size_t i = 0;
	int ret = 0;
	u32 message[MBOX_SR_NUM] = { 0 };
	u32 max_rsp_argc = rsp->argc;

	if (!mb)
		return -ENODEV;

	if (!req || !rsp)
		return -EINVAL;

	if (req->argc && !req->args) {
		/* request args required */
		return -EINVAL;
	}

	if (rsp->argc && !rsp->args) {
		/* response args required */
		return -EINVAL;
	}

	if (req->argc + 2 > MBOX_SR_NUM) {
		/* too many request arguments */
		return -EINVAL;
	}

	/* Build the message */
	message[MBOX_SR_SEND_CMD_IDX] = req->cmd;
	message[MBOX_SR_SEND_ARGC_IDX] = req->argc;
	for (i = 0; i < req->argc; i++)
		message[MBOX_SR_SEND_ARGV_IDX + i] = req->args[i];

	/* Prep the return buffer */
	reinit_completion(&slot->mbox_cmd_completion);

	/* Send the message */
	ret = mb->send_mbox_msg(channel, (void *)&message);

	/* wait for response */
	/* Maybe this should use a timeout? */
	wait_for_completion(&slot->mbox_cmd_completion);

	/* Data received via mbox_rx_callback */

	rsp->cmd = slot->rsp_buffer[MBOX_SR_RECV_CMD_IDX];
	rsp->err = slot->rsp_buffer[MBOX_SR_RECV_ERR_IDX];
	rsp->argc = slot->rsp_buffer[MBOX_SR_RECV_ARGC_IDX];

	/* Validity check argc */
	/* check argc: first 3 registers are for cmd, err and argc */
	if (WARN_ON(rsp->argc + 3 > MBOX_SR_NUM)) {
		/* malformed response */
		ret = -EIO;
		goto err_bad_rsp;
	}

	if (WARN_ON(rsp->argc > max_rsp_argc)) {
		/* not enough space to save all returned arguments */
		rsp->argc = max_rsp_argc;
	}

	/* Read the rest of the data */
	for (i = 0; i < rsp->argc; i++)
		rsp->args[i] = slot->rsp_buffer[MBOX_SR_RECV_ARGV_IDX + i];

	ret = 0;

err_bad_rsp:
	return ret;
}

static int check_mbox_cmd_rsp(struct device *dev,
			      struct gsa_mbox_rsp *rsp, u32 cmd)
{
	if (rsp->cmd != (cmd | GSA_MB_CMD_RSP)) {
		/* bad response */
		dev_err(dev, "cmd=0x%x\n", rsp->cmd);
		return -EIO;
	}

	if (rsp->err == GSA_MB_OK)
		return 0;

	dev_err(dev, "mbox cmd=0x%x returned err=%u\n", cmd, rsp->err);

	BUG_ON(bug_on_mbox_error);

	switch (rsp->err) {
	case GSA_MB_ERR_BAD_HANDLE:
	case GSA_MB_ERR_INVALID_ARGS:
		return -EINVAL;

	case GSA_MB_ERR_BUSY:
		return -EBUSY;

	case GSA_MB_ERR_AUTH_FAILED:
		return -EACCES;

	case GSA_MB_ERR_OUT_OF_RESOURCES:
		return -ENOMEM;

	case GSA_MB_ERR_ALREADY_RUNNING:
		return -EEXIST;

	case GSA_MB_ERR_TIMED_OUT:
		return -ETIMEDOUT;

	default:
		return -EIO;
	}

	return 0;
}

/* Builds up the request into a standard struct, sends the command, and
 * translates any resulting error codes. */
static int gsa_send_mbox_cmd_locked(struct gsa_mbox *mb, u32 cmd,
				    u32 *req_args, u32 req_argc,
				    u32 *rsp_args, u32 rsp_max_argc)
{
	int ret;
	struct gsa_mbox_req mb_req;
	struct gsa_mbox_rsp mb_rsp;

	/* prepare request */
	mb_req.cmd = cmd;
	mb_req.argc = req_argc;
	mb_req.args = req_args;

	/* prepare response */
	mb_rsp.cmd = 0;
	mb_rsp.err = 0;
	mb_rsp.argc = rsp_max_argc;
	mb_rsp.args = rsp_args;

	/* execute command */
	ret = exec_mbox_cmd_sync_locked(mb, &mb_req, &mb_rsp);
	if (ret < 0)
		return ret;

	/* check result */
	ret = check_mbox_cmd_rsp(mb->dev, &mb_rsp, cmd);
	if (ret < 0)
		return ret;

	return mb_rsp.argc;
}

static bool is_data_xfer(u32 cmd)
{
	switch (cmd) {
	case GSA_MB_CMD_AUTH_IMG:
	case GSA_MB_CMD_LOAD_FW_IMG:
	case GSA_MB_TEST_CMD_START_UNITTEST:
	case GSA_MB_TEST_CMD_RUN_UNITTEST:
	case GSA_MB_CMD_LOAD_TPU_FW_IMG:
	case GSA_MB_CMD_UNLOAD_TPU_FW_IMG:
	case GSA_MB_CMD_GSC_TPM_DATAGRAM:
	case GSA_MB_CMD_GSC_NOS_CALL:
	case GSA_MB_CMD_KDN_GENERATE_KEY:
	case GSA_MB_CMD_KDN_EPHEMERAL_WRAP_KEY:
	case GSA_MB_CMD_KDN_DERIVE_RAW_SECRET:
	case GSA_MB_CMD_KDN_PROGRAM_KEY:
	case GSA_MB_CMD_LOAD_AOC_FW_IMG:
	case GSA_MB_CMD_LOAD_DSP_FW_IMG:
	case GSA_MB_CMD_SJTAG_GET_PUB_KEY_HASH:
	case GSA_MB_CMD_SJTAG_SET_PUB_KEY:
	case GSA_MB_CMD_SJTAG_GET_CHALLENGE:
	case GSA_MB_CMD_SJTAG_ENABLE:
	case GSA_MB_CMD_LOAD_APP_PKG:
	case GSA_MB_CMD_GET_GSA_VERSION:
	case GSA_MB_CMD_RUN_TRACE_DUMP:
	case GSA_MB_CMD_GET_PM_STATS:
		return true;

	default:
		return false;
	}
}

#if IS_ENABLED(CONFIG_GSA_PKVM)

#define MAX_GSA_WAKELOCK_CNT 100

static int gsa_data_xfer_prepare_locked(struct gsa_mbox *mb)
{
	int rc;
	int ret;

	if (WARN_ON(mb->wake_ref_cnt >= MAX_GSA_WAKELOCK_CNT))
		return -EINVAL;

	if (mb->wake_ref_cnt) {
		/* just bump wake ref count */
		++mb->wake_ref_cnt;
		return 0;
	}

	/*
	 * If we are running under Hypervisor DATA XFER command requires
	 * proper SYSMMU_S2 configuration. Since GSA SYSMMU_S2 is in GSACORE
	 * power domain, we need grab wakelock in order to make sure it is
	 * powered
	 */
	ret = gsa_send_mbox_cmd_locked(mb, GSA_MB_CMD_WAKELOCK_ACQUIRE,
				       NULL, 0, NULL, 0);
	if (ret < 0) {
		dev_err(mb->dev, "gsa wakelock acquire failed (%d)\n",
			ret);
		return ret;
	}
	++mb->wake_ref_cnt;

	/* resume gsa s2mpu */
	ret = pm_runtime_get_sync(mb->s2mpu);
	if (ret < 0) {
		dev_err(mb->s2mpu, "failed to resume s2mpu (%d)\n", ret);
		goto err_s2mpu_resume;
	}

	return 0;

err_s2mpu_resume:
	/* release wakelock */
	rc = gsa_send_mbox_cmd_locked(mb, GSA_MB_CMD_WAKELOCK_RELEASE,
				      NULL, 0, NULL, 0);
	if (WARN_ON(rc < 0)) {
		dev_err(mb->dev, "gsa wakelock release failed (%d): leaking wakelock\n", rc);
	} else {
		/* undo ref count obtained after acquiring lock */
		--mb->wake_ref_cnt;
	}
	return ret;
}

static void gsa_data_xfer_finish_locked(struct gsa_mbox *mb)
{
	int rc;

	if (WARN_ON(!mb->wake_ref_cnt))
		return;

	if (mb->wake_ref_cnt > 1) {
		/* Just decrement wake ref count */
		--mb->wake_ref_cnt;
		return;
	}

	/* suspend gsa s2mpu */
	rc = pm_runtime_put_sync_suspend(mb->s2mpu);
	if (rc < 0) {
		dev_err(mb->s2mpu, "failed to suspend s2mpu (%d), leaking wakelock\n", rc);
		return;
	}

	/* the following call can only fail if acquire/release
	 * are imbalanced, in this case we cannot really continue.
	 */
	rc = gsa_send_mbox_cmd_locked(mb, GSA_MB_CMD_WAKELOCK_RELEASE,
				      NULL, 0, NULL, 0);
	if (WARN_ON(rc < 0)) {
		dev_err(mb->dev, "gsa wakelock release failed (%d)\n", rc);
		goto err_wakelock_release;
	}

	/* decrement wake ref */
	--mb->wake_ref_cnt;
	return;

err_wakelock_release:
	/* at least try to get into consistent state */
	rc = pm_runtime_get_sync(mb->s2mpu);
	if (WARN_ON(rc < 0))
		dev_err(mb->s2mpu, "failed to resume s2mpu (%d), leaking wakelock\n", rc);
	return;
}

#else /* CONFIG_GSA_PKVM */

static int gsa_data_xfer_prepare_locked(struct gsa_mbox *mb)
{
	int rc;

	rc = pm_runtime_resume_and_get(mb->dev);
	if (rc < 0)
		dev_err(mb->dev, "failed to resume pm_runtime (%d)\n", rc);

	return rc;
}

static void gsa_data_xfer_finish_locked(struct gsa_mbox *mb)
{
	int rc;

	rc = pm_runtime_put(mb->dev);
	if (rc < 0)
		dev_err(mb->dev, "failed to suspend pm_runtime device (%d)\n", rc);
}

#endif /* CONFIG_GSA_PKVM */

/* Lock the shared memory, prepare the DMA, and then send the command. */
int gsa_send_mbox_cmd(struct gsa_mbox *mb, u32 cmd,
		      u32 *req_args, u32 req_argc,
		      u32 *rsp_args, u32 rsp_max_argc)
{
	int ret;
	bool data_xfer = is_data_xfer(cmd);

	mutex_lock(&mb->share_reg_lock);

	if (data_xfer) {
		ret = gsa_data_xfer_prepare_locked(mb);
		if (ret < 0)
			goto err_data_prepare;
	}

	/* send command */
	ret = gsa_send_mbox_cmd_locked(mb, cmd, req_args, req_argc,
				       rsp_args, rsp_max_argc);

	if (data_xfer)
		gsa_data_xfer_finish_locked(mb);

err_data_prepare:
	mutex_unlock(&mb->share_reg_lock);
	return ret;
}

MODULE_LICENSE("GPL v2");
