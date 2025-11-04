// SPDX-License-Identifier: GPL-2.0-only
/*
 * Platform device driver to power on/off google soc power domains
 * Copyright (C) 2023-2025 Google LLC.
 */

#include <linux/arm-smccc.h>
#include <linux/container_of.h>
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/pm_domain.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/iopoll.h>
#include <linux/workqueue.h>

#include <dt-bindings/power/genpd_lga.h>
#include <mailbox/protocols/mba/cpm/common/lpb/lpb_service.h>

#include "power_controller.h"
#include "cpm_mappings.h"
#include "gpu_core_logic.h"

#define CREATE_TRACE_POINTS
#include "power_controller_trace.h"

#include <soc/google/goog_cpm_service_ids.h>

#define MAILBOX_TIMEOUT_EMULATION_MULTIPLIER 30

#define LPB_REMOTE_CHANNEL 0x5
#define LPCM_CMD_SET_GEN_PD 1
#define LPCM_REMOTE_CHANNEL 0x8
#define POWER_ON_BITFIELD GENMASK(31, 24)
#define PD_ID_BITFIELD GENMASK(23, 16)
#define SSWRP_ID_BITFIELD GENMASK(15, 8)
#define REQ_ID_BITFIELD GENMASK(7, 0)

#define NO_ERROR 0
#define LPB_STATUS_ON 3
#define LPB_STATUS_OFF 0
#define LPB_EVENT_ON 0
#define LPB_EVENT_OFF 1
#define CPM_COMMON_PROT_NO_ERR 15

#define SIP_SVC_PD_CONTROL 0x82000701

#define ON_OFF_STR(x) ((x) ? "power_on" : "power_off")

static u32 mbx_send_timeout_ms = 3000;
static u32 mbx_receive_timeout_ms = 3000;

static inline struct power_domain *to_power_domain(struct generic_pm_domain *d)
{
	return container_of(d, struct power_domain, genpd);
}

/*
 * This callback is triggered when Kernel receives the second message
 * (power on/off completion) from CPM's LPB service.
 */
static void google_power_controller_rx_callback(u32 context, void *msg,
						void *priv_data)
{
	struct cpm_iface_payload *cpm_msg = msg;
	struct power_controller *power_controller = priv_data;
	struct device *dev = power_controller->dev;
	struct power_domain *pd = NULL;
	u32 sswrp_id = cpm_msg->payload[0];
	bool event_on = cpm_msg->payload[1] == LPB_EVENT_ON;
	int i;

	/* FIXME: client->dev ?? */
	dev_dbg(dev, "rx callback msg %d %d %d\n", cpm_msg->payload[0],
		cpm_msg->payload[1], cpm_msg->payload[2]);

	for (i = 0; i < power_controller->pd_count; i++) {
		if (power_controller->pds[i].is_top_pd &&
			power_controller->pds[i].cpm_lpb_sswrp_id == sswrp_id) {
			pd = &power_controller->pds[i];
			break;
		}
	}
	if (!pd) {
		dev_err(dev,
			"cannot find corresponding top power domain for cpm_lpb_sswrp_id %d",
			sswrp_id);
		return;
	}

	/*
	 * Power controller should never receive a completion of a transition to the current state.
	 */
	if (pd->state == event_on) {
		dev_err(&pd->genpd.dev,
			"Unexpected CPM %s completion received\n",
			ON_OFF_STR(event_on));
		return;
	}

	if (power_controller->pds) {
		pd_latency_profile_store(&pd->genpd,
					 (uint64_t)cpm_msg->payload[2],
					 (event_on ? FW_ON : FW_OFF));
	}

	/* todo(b/247494764): add error handling once CPM LPB completion message is ready */
	pd->cpm_err = NO_ERROR;

	dev_dbg(dev, "%s: %s completed\n", pd->name, ON_OFF_STR(event_on));

	pd_latency_profile_stop(&pd->genpd,
		event_on ? LATENCY_TYPE_BITMASK(EXEC_ON) : LATENCY_TYPE_BITMASK(EXEC_OFF),
		false);

	complete(&pd->cpm_resp_done);
}

static int initialize_mailbox_client(struct power_controller *power_controller)
{
	struct device *dev = power_controller->dev;
	int ret = 0;
	struct device_node *np = dev->of_node;

	if (of_property_present(np, "in_emulation")) {
		mbx_send_timeout_ms *= MAILBOX_TIMEOUT_EMULATION_MULTIPLIER;
		mbx_receive_timeout_ms *= MAILBOX_TIMEOUT_EMULATION_MULTIPLIER;
	}

	power_controller->client = cpm_iface_request_client(dev,
			APC_COMMON_SERVICE_ID_POWER_CONTROLLER,
			google_power_controller_rx_callback, power_controller);

	if (IS_ERR(power_controller->client))
		ret = PTR_ERR(power_controller->client);

	return ret;
}

static int get_lpb_state(struct generic_pm_domain *genpd)
{
	struct power_domain *pd = to_power_domain(genpd);
	struct device *dev = pd->power_controller->dev;
	struct cpm_iface_req cpm_req;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	int ret;

	if (!pd->is_top_pd) {
		dev_dbg(dev, "%s: Can't use LPB state get for subdomain", pd->name);
		return -EINVAL;
	}

	cpm_req.msg_type = REQUEST_MSG;
	cpm_req.req_msg = &req_msg;
	cpm_req.resp_msg = &resp_msg;
	cpm_req.tout_ms = mbx_send_timeout_ms;
	cpm_req.dst_id = LPB_REMOTE_CHANNEL;

	req_msg.payload[0] = LPB_CMD_SSWRP_STATE_GET;
	req_msg.payload[1] = pd->cpm_lpb_sswrp_id;

	ret = cpm_send_message(pd->power_controller->client, &cpm_req);
	if (ret < 0) {
		dev_err(dev, "%s: failed to get SSWRP state, MBA err: %d", pd->name, ret);
		return ret;
	}

	return resp_msg.payload[1] == LPB_STATUS_ON ? PD_STATE_ON : PD_STATE_OFF;
}

static int send_lpb_mail(struct power_domain *pd, bool power_on)
{
	struct device *dev = pd->power_controller->dev;
	struct cpm_iface_req cpm_req;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	int ret;

	dev_dbg(dev, "%s: %s: sswrp_id = %d\n", pd->name,
		ON_OFF_STR(power_on), pd->cpm_lpb_sswrp_id);

	cpm_req.msg_type = REQUEST_MSG;
	cpm_req.req_msg = &req_msg;
	cpm_req.resp_msg = &resp_msg;
	cpm_req.tout_ms = mbx_send_timeout_ms;
	cpm_req.dst_id = LPB_REMOTE_CHANNEL;

	req_msg.payload[0] = LPB_CMD_SSWRP_STATE_SET;
	req_msg.payload[1] = pd->cpm_lpb_sswrp_id;
	req_msg.payload[2] = power_on;

	reinit_completion(&pd->cpm_resp_done);

	trace_send_mail_lpb(pd, power_on);
	ret = cpm_send_message(pd->power_controller->client, &cpm_req);
	pd_latency_profile_stop(&pd->genpd,
		power_on ? LATENCY_TYPE_BITMASK(ACK_ON) : LATENCY_TYPE_BITMASK(ACK_OFF),
		false);

	if (ret < 0) {
		dev_err(dev, "%s: %s: send lpb message failed ret (%d)\n",
			pd->name, ON_OFF_STR(power_on), ret);

		pd->cpm_err = ret;
		goto finish_lpb;
	}
	dev_dbg(dev, "%s: %s: lpb response msg %d %d %d\n", pd->name, ON_OFF_STR(power_on),
		resp_msg.payload[0], resp_msg.payload[1], resp_msg.payload[2]);

	if (resp_msg.payload[0] != LPB_SERVICE_CMD_SUCCESS) {
		/* We failed completely no additional message is waiting */
		dev_err(dev, "%s: %s: CPM LPB service failed with err=%d\n",
			pd->name, ON_OFF_STR(power_on), resp_msg.payload[0]);
		pd->cpm_err = resp_msg.payload[0];
		goto finish_lpb;
	}

	switch (resp_msg.payload[1]) {
	case LPB_SERVICE_CMD_SUCCESS:
		/* Command requires no asynchronous processing -- we are done */
		pd->cpm_err = NO_ERROR;
		break;
	case LPB_SERVICE_PWRUP_STARTED:
	case LPB_SERVICE_PWRDN_STARTED:
		/*
		 * We have received the first response for power on/off.  The final
		 * result will be received asynchronously.
		 *
		 * pd->cpm_err will have been set in the interrupt handler.
		 */

		ret = wait_for_completion_timeout(&pd->cpm_resp_done,
						  msecs_to_jiffies(mbx_receive_timeout_ms));
		if (ret == 0) {
			panic("%s: %s: wait for CPM response timeout\n",
				pd->name, ON_OFF_STR(power_on));
			pd->cpm_err = -ETIMEDOUT;
			goto finish_lpb;
		}
		break;
	case LPB_SERVICE_CMD_FAIL:
	default:
		/* Command got through but the result was failure */
		pd->cpm_err = resp_msg.payload[1];
		break;
	}

finish_lpb:
	trace_recv_result_lpb(pd, power_on, pd->cpm_err);
	if (pd->cpm_err)
		pd_latency_profile_stop(&pd->genpd,
			power_on ? LATENCY_TYPE_BITMASK(ACK_ON) | LATENCY_TYPE_BITMASK(EXEC_ON) :
				LATENCY_TYPE_BITMASK(ACK_OFF) | LATENCY_TYPE_BITMASK(EXEC_OFF),
			true);
	return pd->cpm_err;
}

static int send_lpcm_mail(struct power_domain *pd, bool power_on)
{
	struct device *dev = pd->power_controller->dev;
	struct cpm_iface_req cpm_req;
	struct cpm_iface_payload req_msg;
	struct cpm_iface_payload resp_msg;
	int ret;

	if (pd->cpm_lpcm_sswrp_id == NOT_SUPPORTED) {
		dev_err(dev,
			"%s: %s: genpd_sswrp_id %d is not supported by CPM GENPD LPCM service\n",
			pd->name, ON_OFF_STR(power_on), pd->genpd_sswrp_id);
		pd->cpm_err = -EPROTONOSUPPORT;
		goto finish_lpcm;
	}

	dev_dbg(dev, "%s: %s: lpcm_id = %d\n", pd->name,
		ON_OFF_STR(power_on), pd->cpm_lpcm_sswrp_id);

	cpm_req.msg_type = REQUEST_MSG;
	cpm_req.req_msg = &req_msg;
	cpm_req.resp_msg = &resp_msg;
	cpm_req.tout_ms = mbx_send_timeout_ms;
	cpm_req.dst_id = LPCM_REMOTE_CHANNEL;
	req_msg.payload[0] = FIELD_PREP(REQ_ID_BITFIELD, LPCM_CMD_SET_GEN_PD) |
			     FIELD_PREP(SSWRP_ID_BITFIELD, pd->cpm_lpcm_sswrp_id) |
			     FIELD_PREP(PD_ID_BITFIELD, pd->cpm_lpcm_subdomain_id) |
			     FIELD_PREP(POWER_ON_BITFIELD, power_on);

	trace_send_mail_lpcm(pd, power_on);
	ret = cpm_send_message(pd->power_controller->client, &cpm_req);
	pd_latency_profile_stop(&pd->genpd,
		power_on ? LATENCY_TYPE_BITMASK(ACK_ON) : LATENCY_TYPE_BITMASK(ACK_OFF),
		false);
	trace_recv_result_lpcm(pd, power_on, resp_msg.payload[0]);

	if (ret < 0) {
		dev_err(dev, "%s: %s: send lpcm message failed ret (%d)\n",
			pd->name, ON_OFF_STR(power_on), ret);

		pd->cpm_err = ret;
		goto finish_lpcm;
	}

	dev_dbg(dev, "%s: %s: got resp from %u, data %u %u %u.\n", pd->name, ON_OFF_STR(power_on),
		cpm_req.dst_id, resp_msg.payload[0], resp_msg.payload[1], resp_msg.payload[2]);

	// todo(b/247494764): add more error handling logic once CPM LPCM interface is ready
	switch (resp_msg.payload[0]) {
	case NO_ERROR:
		dev_dbg(dev, "%s: %s: lpcm service success.\n", pd->name, ON_OFF_STR(power_on));
		pd->cpm_err = 0;
		break;
	default:
		// TODO(b/293359099): Make this message dev_err after the model is fixed.
		dev_dbg(dev, "%s: %s: unknown error code %d.\n", pd->name, ON_OFF_STR(power_on),
			resp_msg.payload[0]);
		pd->cpm_err = -EPROTO;
		break;
	}

finish_lpcm:
	pd_latency_profile_stop(&pd->genpd,
		power_on ? LATENCY_TYPE_BITMASK(ACK_ON) | LATENCY_TYPE_BITMASK(EXEC_ON) :
		LATENCY_TYPE_BITMASK(ACK_OFF) | LATENCY_TYPE_BITMASK(EXEC_OFF),
		pd->cpm_err != 0);
	return pd->cpm_err;
}

static int send_mba_mail(struct power_domain *pd, bool power_on)
{
	if (pd->is_top_pd)
		return send_lpb_mail(pd, power_on);

	return send_lpcm_mail(pd, power_on);
}

static bool is_gpu_logic_core_pd(struct power_domain *pd)
{
	return pd->genpd_sswrp_id == GENPD_LGA_SSWRP_GPU_ID &&
	       !pd->is_top_pd &&
	       pd->cpm_lpcm_subdomain_id == GENPD_LGA_GPU_CORE_LOGIC_ID;
}

static bool is_aoc_sswrp_pd(struct power_domain *pd)
{
	return pd->genpd_sswrp_id == GENPD_LGA_SSWRP_AOC_ID && pd->is_top_pd &&
	       pd->cpm_lpcm_sswrp_id == GENPD_LGA_SSWRP_AOC_ID;
}

static inline unsigned long lpcm_smc(struct power_domain *pd, bool power_on)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SIP_SVC_PD_CONTROL, pd->genpd_sswrp_id,
		      pd->cpm_lpcm_subdomain_id, power_on, 0, 0, 0, 0, &res);

	WARN_ON(res.a0);
	if ((unsigned long)res.a1) {
		dev_err(pd->power_controller->dev, "Failed LPCM SMC request sswrp:%d "
			"sub-domain:%d, res.a1:%lu\n", pd->genpd_sswrp_id,
			pd->cpm_lpcm_subdomain_id, res.a1);
		return -EINVAL;
	}

	return 0;
}

static int power_on(struct generic_pm_domain *domain)
{
	int ret;
	struct power_domain *pd = to_power_domain(domain);

	pd_latency_profile_start(domain,
		LATENCY_TYPE_BITMASK(E2E_ON) |
		LATENCY_TYPE_BITMASK(EXEC_ON) |
		LATENCY_TYPE_BITMASK(ACK_ON));

	if (is_gpu_logic_core_pd(pd))
		ret = gpu_core_logic_on(pd->power_controller->dev, &pd->gpu_core_logic);
	else if (pd->use_smc)
		ret = lpcm_smc(pd, true);
	else
		ret = send_mba_mail(pd, true);

	if (!ret)
		pd->state = PD_STATE_ON;

	pd_latency_profile_stop(domain,
		LATENCY_TYPE_BITMASK(E2E_ON) |
		LATENCY_TYPE_BITMASK(EXEC_ON) |
		LATENCY_TYPE_BITMASK(ACK_ON), false);

	return ret;
}

static int power_off(struct generic_pm_domain *domain)
{
	int ret;
	struct power_domain *pd = to_power_domain(domain);

	pd_latency_profile_start(domain,
		LATENCY_TYPE_BITMASK(E2E_OFF) |
		LATENCY_TYPE_BITMASK(EXEC_OFF) |
		LATENCY_TYPE_BITMASK(ACK_OFF));

	if (is_gpu_logic_core_pd(pd))
		ret = gpu_core_logic_off(pd->power_controller->dev, &pd->gpu_core_logic);
	else if (pd->use_smc)
		ret = lpcm_smc(pd, false);
	else
		ret = send_mba_mail(pd, false);

	if (!ret)
		pd->state = PD_STATE_OFF;

	pd_latency_profile_stop(domain,
		LATENCY_TYPE_BITMASK(E2E_OFF) |
		LATENCY_TYPE_BITMASK(EXEC_OFF) |
		LATENCY_TYPE_BITMASK(ACK_OFF), false);

	return ret;
}

static int noop_resume_noirq_overwrite(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	dev_dbg(dev, "Skipping power_domain resume_noirq call for the device.");

	return pm && pm->resume_noirq ? pm->resume_noirq(dev) : 0;
}

static void unregister_power_domains(struct platform_device *pdev, int count)
{
	/* TODO: b/316073743 - Remove the bad domains only */
	struct power_controller *power_controller = platform_get_drvdata(pdev);
	struct device_node *pwr_ctrl_np = pdev->dev.of_node;
	struct device_node *pwr_domain_np;
	struct power_domain *pd;
	int i = 0;

	dev_dbg(&pdev->dev, "Unregistering all power domains\n");
	for_each_available_child_of_node(pwr_ctrl_np, pwr_domain_np) {
		if (i >= count)
			break;
		pd = &power_controller->pds[i];
		of_genpd_del_provider(pwr_domain_np);
		pm_genpd_remove(&pd->genpd);
		++i;
	}
}

#ifdef CONFIG_DEBUG_FS
/*
 * This flag can be dangerous because it allows power on/off a power domain
 * without changing reference count in genpd core. This can lead to an
 * inconsistent state where genpd core thinks the domain is ON while it is
 * actually OFF and vice versa. Only define this flag for testing purposes.
 */
static int debugfs_power_domain_ctrl_set(void *domain, u64 val)
{
	struct generic_pm_domain *genpd = domain;
	int ret = 0;

	dev_dbg(&genpd->dev, "%s\n", ON_OFF_STR(val));

	mutex_lock(&genpd->mlock);
	if (val)
		ret = power_on(genpd);
	else
		ret = power_off(genpd);
	mutex_unlock(&genpd->mlock);

	return ret;
}

static int debugfs_power_domain_ctrl_get(void *domain, u64 *val)
{
	struct generic_pm_domain *genpd = domain;
	int ret;

	ret = get_lpb_state(genpd);
	if (ret < 0)
		return -EINVAL;

	*val = ret;

	return NO_ERROR;
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_power_domain_ctrl_fops, debugfs_power_domain_ctrl_get,
			 debugfs_power_domain_ctrl_set, "%llu\n");

static void genpd_debugfs_add(struct generic_pm_domain *domain)
{
	struct power_domain *pd = to_power_domain(domain);
	struct dentry *d =
		debugfs_create_dir(domain->name,
				   pd->power_controller->debugfs_root);

	debugfs_create_file("state", 0220, d, domain,
			    &debugfs_power_domain_ctrl_fops);
}

static void genpd_debugfs_init(struct power_controller *power_controller)
{
	power_controller->debugfs_root =
		debugfs_create_dir(dev_name(power_controller->dev), NULL);

	for (int i = 0; i < power_controller->pd_count; ++i)
		genpd_debugfs_add(&power_controller->pds[i].genpd);
}

static void genpd_debugfs_remove(struct power_controller *power_controller)
{
	debugfs_remove_recursive(power_controller->debugfs_root);
}
#else
static inline void genpd_debugfs_init(struct power_controller *power_controller) {}
static inline void genpd_debugfs_remove(struct power_controller *power_controller) {}
#endif

static inline void power_controller_mbox_free(struct power_controller *pc)
{
	cpm_iface_free_client(pc->client);
}

struct power_controller_desc {
	enum gpu_dts_version version;
	const struct cpm_mappings *cpm_map;
};

struct power_controller_desc lga_power_controller_desc = {
	.version = GPU_DTS_VERSION_2,
	.cpm_map = &lga_cpm_mappings,
};

static int power_controller_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_controller *power_controller;
	struct device_node *np = dev->of_node;
	struct device_node *child_np;
	struct of_phandle_args child_args, parent_args;
	const struct power_controller_desc *desc;
	const struct cpm_mappings *cpm_map;
	int ret, i, pd_count;

	power_controller =
		devm_kzalloc(dev, sizeof(*power_controller), GFP_KERNEL);

	if (!power_controller)
		return -ENOMEM;

	power_controller->dev = dev;

	platform_set_drvdata(pdev, power_controller);

	ret = initialize_mailbox_client(power_controller);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			dev_dbg(dev, "mailbox is not ready. Probe later. (ret: %d)", ret);
		else
			dev_err(dev, "failed to request mailbox channel err %d\n", ret);
		return ret;
	}

	pd_count = of_get_child_count(np);
	power_controller->pds = devm_kcalloc
		(dev, pd_count, sizeof(*power_controller->pds), GFP_KERNEL);
	power_controller->pd_count = pd_count;

	i = 0;

	desc = of_device_get_match_data(dev);
	if (!desc) {
		ret = -EINVAL;
		goto cleanup_pds;
	}
	cpm_map = desc->cpm_map;

	for_each_available_child_of_node(np, child_np) {
		struct power_domain *pd = &power_controller->pds[i];

		pd->is_top_pd = false;
		pd->name = child_np->name;
		ret = of_property_read_u32(child_np, "subdomain-id",
					    &pd->cpm_lpcm_subdomain_id);
		if (ret == -EINVAL) {
			/* Top power domains do not have subdomain-id property. */
			pd->is_top_pd = true;
		} else if (ret < 0) {
			dev_err(dev, "failed to read subdomain-id\n");
			of_node_put(child_np);
			goto cleanup_pds;
		}
		pd->use_smc = of_property_read_bool(child_np, "use-smc");

		ret = of_property_read_u32(child_np, "sswrp-id",
					    &pd->genpd_sswrp_id);
		if (ret < 0) {
			dev_err(dev, "failed to read sswrp-id of node %s\n",
				child_np->name);
			of_node_put(child_np);
			goto cleanup_pds;
		}

		if (pd->is_top_pd) {
			pd->cpm_lpb_sswrp_id =
				cpm_map->lpb_sswrp_ids[pd->genpd_sswrp_id];
			init_completion(&pd->cpm_resp_done);
		} else if (is_gpu_logic_core_pd(&power_controller->pds[i])) {
			ret = gpu_core_logic_init(dev, child_np,
						   &pd->gpu_core_logic);
			if (ret) {
				of_node_put(child_np);
				goto cleanup_pds;
			}

			pd->gpu_core_logic.gpu_dts_version = desc->version;
		} else {
			pd->cpm_lpcm_sswrp_id =
				cpm_map->lpcm_sswrp_ids[pd->genpd_sswrp_id];
			init_completion(&pd->cpm_resp_done);
		}

		pd->power_controller = power_controller;
		pd->genpd.name = child_np->name;
		pd->genpd.power_on = power_on;
		pd->genpd.power_off = power_off;
		pd->state = PD_STATE_OFF;

		/*
		 * The 'on-at-init' flag signifies that the power domain is already ON.
		 * TODO(b/294182577): remove on-at-init flag after a proper solution is implemented.
		 */
		if (of_property_read_bool(child_np, "on-at-init"))
			pd->state = PD_STATE_ON;
		/*
		 * The 'force-on' flag signifies that the power domain has to be turned ON.
		 */
		else if (of_property_read_bool(child_np, "force-on")) {
			ret = power_on(&pd->genpd);
			if (ret) {
				dev_err(dev, "failed to turn ON the domain err %d\n", ret);
				of_node_put(child_np);
				goto cleanup_pds;
			}
			pd->state = PD_STATE_ON;
		}

		/*
		 * The 'always-on' flag signifies that the power domain needs to stay ON always.
		 * The core GenPD driver expects such domains to be ON already.
		 */
		if (of_property_read_bool(child_np, "always-on")) {
			pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
			if (pd->state == PD_STATE_OFF) {
				ret = -EINVAL;
				dev_err(dev, "always-on PM domain is not on\n");
				of_node_put(child_np);
				goto cleanup_pds;
			}
		}

		/*
		 * The 'rpm-always-on' flag signifies that the power domain needs to stay ON during
		 * runtime suspend.
		 * The core GenPD driver expects such domains to be ON already.
		 */
		if (of_property_read_bool(child_np, "rpm-always-on")) {
			pd->genpd.flags |= GENPD_FLAG_RPM_ALWAYS_ON;
			if (pd->state == PD_STATE_OFF) {
				ret = -EINVAL;
				dev_err(dev, "rpm-always-on PM domain is not on\n");
				of_node_put(child_np);
				goto cleanup_pds;
			}
		}

		if (of_property_read_bool(child_np, "irq-safe")) {
			pd->genpd.flags |= GENPD_FLAG_IRQ_SAFE;

			if (pd->genpd.flags & GENPD_FLAG_RPM_ALWAYS_ON) {
				dev_err(dev, "rpm-always-on and irq-safe both set\n");
				of_node_put(child_np);
				goto cleanup_pds;
			}
		} else {
			WARN_ON(pd->use_smc && pd->is_top_pd);
		}

		/*
		 * The `active-wakeup` signifies powerdomain holds a wakeup source hence should
		 * be kept active in case any device attached is wakeup capable during suspend.
		 */
		if (of_property_read_bool(child_np, "active-wakeup"))
			power_controller->pds[i].genpd.flags |= GENPD_FLAG_ACTIVE_WAKEUP;

		ret = pm_genpd_init(&pd->genpd, NULL, pd->state == PD_STATE_OFF);
		if (ret) {
			dev_err(dev, "failed to init power domain\n");
			of_node_put(child_np);
			goto cleanup_pds;
		}

		if (of_property_read_bool(child_np, "no-auto-resume")) {
			dev_dbg(dev, "%s: set noop for resume_noirq callback", pd->name);
			pd->genpd.domain.ops.resume_noirq = noop_resume_noirq_overwrite;
		}

		ret = of_genpd_add_provider_simple(child_np, &pd->genpd);
		if (ret) {
			dev_err(dev,
				"failed to register power domain provider with genpd\n");
			pm_genpd_remove(&pd->genpd);
			of_node_put(child_np);
			goto cleanup_pds;
		}

		i++;
	}
	for_each_available_child_of_node(np, child_np) {
		if (of_parse_phandle_with_args(child_np, "power-domains",
					       "#power-domain-cells", 0,
					       &parent_args))
			continue;

		child_args.np = child_np;
		child_args.args_count = 0;
		ret = of_genpd_add_subdomain(&parent_args, &child_args);
		of_node_put(parent_args.np);
		if (ret) {
			dev_err(dev, "failed to add subdomain\n");
			of_node_put(child_np);
			goto cleanup_pds;
		}
	}

	genpd_debugfs_init(power_controller);
	ret = pd_latency_profile_init(pdev);
	if (ret)
		dev_err(dev, "Error while initiating latency profiler: %d", ret);

	return 0;

cleanup_pds:
	power_controller_mbox_free(power_controller);
	unregister_power_domains(pdev, i);
	return ret;
};

static int power_controller_remove(struct platform_device *pdev)
{
	struct power_controller *power_controller = platform_get_drvdata(pdev);
	struct device_node *child_np, *np = pdev->dev.of_node;
	int i;

	pd_latency_profile_remove(pdev);
	genpd_debugfs_remove(power_controller);

	power_controller_mbox_free(power_controller);

	for_each_available_child_of_node(np, child_np) {
		of_genpd_del_provider(child_np);
	}
	for (i = power_controller->pd_count - 1; i >= 0; i--)
		pm_genpd_remove(&power_controller->pds[i].genpd);

	return 0;
}

static const struct of_device_id power_controller_of_match_table[] = {
	[0] = { .compatible = "google,lga-power-controller",
			.data = &lga_power_controller_desc },
	[1] = {},
};
MODULE_DEVICE_TABLE(of, power_controller_of_match_table);
static int power_controller_suspend_prepare(struct device *dev)
{
	struct power_controller *controller = dev_get_drvdata(dev);

	for (int i = 0; i < controller->pd_count; i++) {
		struct power_domain *pd = &controller->pds[i];

		if (is_aoc_sswrp_pd(pd)) {
			pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
			break;
		}
	}
	return 0;
}

static void power_controller_suspend_complete(struct device *dev)
{
	struct power_controller *controller = dev_get_drvdata(dev);

	for (int i = 0; i < controller->pd_count; i++) {
		struct power_domain *pd = &controller->pds[i];

		if (is_aoc_sswrp_pd(pd)) {
			pd->genpd.flags &= ~GENPD_FLAG_ALWAYS_ON;
			break;
		}
	}
}

static const struct dev_pm_ops simple_pm_ops = {
	.prepare = power_controller_suspend_prepare,
	.complete = power_controller_suspend_complete,
};

static struct platform_driver power_controller_driver = {
	.probe = power_controller_probe,
	.remove = power_controller_remove,
	.driver = {
		.name = "power-controller",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(power_controller_of_match_table),
		.pm = &simple_pm_ops,
	},
};

module_platform_driver(power_controller_driver);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("power controller driver");
MODULE_LICENSE("GPL");
