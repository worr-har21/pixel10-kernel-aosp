// SPDX-License-Identifier: GPL-2.0
/**
 * @copyright Copyright (c) 2023 Samsung Electronics Co., Ltd
 *
 */

#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include "include/uwb_gpio.h"
#include "include/uwb_fw_common.h"

static unsigned long gpio_delay_ms = GPIO_DELAY_MS;
module_param(gpio_delay_ms, ulong, 0664);
MODULE_PARM_DESC(gpio_delay_ms, "The delay time between each GPIO operation");

static unsigned long pow_swt_gpio_delay_ms = 50;
module_param(pow_swt_gpio_delay_ms, ulong, 0664);
MODULE_PARM_DESC(pow_swt_gpio_delay_ms, "The delay time between VBAT switch GPIO operations");

void uwb_init_irq(struct uwb_irq *irq, unsigned int num, const char *name,
				unsigned long flags)
{
	spin_lock_init(&irq->lock);
	irq->num = num;
	strscpy(irq->name, name, sizeof(irq->name) - 1);
	irq->flags = flags;
	UWB_DEBUG("name:%s num:%d flags:%#08lX\n", name, num, flags);
}

int uwb_request_irq(struct uwb_irq *irq, irq_handler_t isr, struct u100_ctx *u100_ctx)
{
	int ret;
	struct miscdevice *uci_misc = &u100_ctx->uci_dev;

	ret = devm_request_threaded_irq(uci_misc->parent,
			irq->num,
			isr,
			rx_tsk_work,
			irq->flags | IRQF_ONESHOT,
			"u100",
			u100_ctx);

	if (ret)
		return ret;

	irq->active = true;
	irq->registered = true;
	UWB_DEBUG("%s(#%d) handler registered (flags:%#08lX)\n",
			 irq->name, irq->num, irq->flags);

	return ret;
}

void set_gpio_value(void *desc, int value)
{
	gpiod_set_value_cansleep(*(struct gpio_desc **)desc, value);
}

int get_gpio_value(void *desc)
{
	return gpiod_get_value_cansleep(*(struct gpio_desc **)desc);
}

unsigned long get_power_switch_delay(void)
{
	return pow_swt_gpio_delay_ms;
}

static void pin_rst_high(struct u100_ctx *u100_ctx)
{
	gpiod_set_value_cansleep(u100_ctx->gpio_u100_reset, 0);
}

static void pin_rst_low(struct u100_ctx *u100_ctx)
{
	gpiod_set_value_cansleep(u100_ctx->gpio_u100_reset, 1);
}

static void pin_en_high(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_en, 1);
}

static void pin_en_low(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_en, 0);
}

static void pin_power_high(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_power, 1);
}

static void pin_power_low(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_power, 0);
}

static void pin_sync_high(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_sync, 1);
}

static void pin_sync_low(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_sync, 0);
}

void uwbs_init(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power on begin");
	pin_rst_low(u100_ctx);
	pin_en_low(u100_ctx);
	pin_sync_low(u100_ctx);
	UWB_DEBUG("U100 power on end");
}

void uwbs_power_on(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power on begin");
	pin_sync_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	skb_queue_purge(&u100_ctx->sk_rx_q);
	skb_queue_purge(&u100_ctx->sk_tx_q);
	pin_en_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	atomic_set(&u100_ctx->u100_powered_on, 1);
	UWB_DEBUG("U100 power on end");
}

void uwbs_power_off(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power off begin");
	pin_sync_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_en_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	atomic_set(&u100_ctx->u100_powered_on, 0);
	UWB_DEBUG("U100 power off end");
}

void uwbs_reset(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 reset begin");
	uwbs_power_off(u100_ctx);
	mdelay(10);
	uwbs_power_on(u100_ctx);
	UWB_DEBUG("U100 reset end");
}

void uwbs_reset_vbat(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 reset vbat begin");
	pin_power_low(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	pin_power_high(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	UWB_DEBUG("U100 reset vbat end");
}

int uwbs_sync_reset(struct u100_ctx *u100_ctx)
{
	int ret = 0;

	UWB_DEBUG("U100 sync reset begin");
	reinit_completion(&u100_ctx->atr_done_cmpl);
	u100_ctx->u100_state = U100_UNKNOWN_STATE;
	u100_ctx->wait_atr_err = FW_ERROR_TIME;
	atomic_set(&u100_ctx->waiting_atr, 1);
	uwbs_power_off(u100_ctx);
	mdelay(10);
	uwbs_power_on(u100_ctx);
	ret = wait_for_completion_timeout(&u100_ctx->atr_done_cmpl, PROBE_ATTR_TIMEOUT);
	if (!ret) {
		atomic_set(&u100_ctx->waiting_atr, 0);
		ret = u100_ctx->wait_atr_err;
		UWB_ERR("U100 sync reset timeout.");
	} else {
		UWB_DEBUG("U100 sync reset end");
		ret = FW_OK;
	}
	return ret;
}

void uwbs_start_download(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 enter download mode begin");
	atomic_set(&u100_ctx->u100_enter_download, 1);
	mdelay(gpio_delay_ms);
	uwbs_power_off(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_sync_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_en_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	mdelay(gpio_delay_ms);
	atomic_set(&u100_ctx->u100_enter_download, 0);
	UWB_DEBUG("U100 enter download mode end");
}

/*Add synchronized enter BL0 download mode */
bool uwbs_sync_start_download(struct u100_ctx *u100_ctx)
{
	int ret;

	UWB_INFO("U100 sync enter download begin");
	u100_ctx->uwb_fw_ctx.action = ACTION_IDLE;
	complete_all(&u100_ctx->process_done_cmpl);
	reinit_completion(&u100_ctx->atr_done_cmpl);
	u100_ctx->u100_state = U100_UNKNOWN_STATE;
	u100_ctx->wait_atr_err = FW_ERROR_TIME;
	atomic_set(&u100_ctx->waiting_atr, 1);
	uwbs_start_download(u100_ctx);
	ret = wait_for_completion_timeout(&u100_ctx->atr_done_cmpl, PROBE_ATTR_TIMEOUT);
	if (!ret) {
		atomic_set(&u100_ctx->waiting_atr, 0);
		UWB_ERR("U100 sync enter download timeout.");
		return false;
	} else if (u100_ctx->u100_state != U100_BL0_STATE) {
		UWB_ERR("U100 sync enter download state abnormal, Expect [%#x], but actual [%#x]",
				U100_BL0_STATE, u100_ctx->u100_state);
		return false;
	}
	UWB_INFO("U100 sync enter download end");
	return true;
}

