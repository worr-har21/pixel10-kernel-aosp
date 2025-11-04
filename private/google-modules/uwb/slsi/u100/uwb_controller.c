// SPDX-License-Identifier: GPL-2.0
/**
 * @copyright Copyright (c) 2023 Samsung Electronics Co., Ltd
 *
 */

#include "include/uwb.h"
#include "include/uwb_gpio.h"

static irqreturn_t uwb2ap_irq_handler(int irq, void *data)
{
	struct u100_ctx *u100_ctx = data;

	complete_all(&u100_ctx->irq_done_cmpl);
	pm_wakeup_dev_event(&u100_ctx->spi->dev, 0, true);
	return IRQ_WAKE_THREAD;
}

int init_controller_layer(struct u100_ctx *u100_ctx)
{
	int ret = 0;

	u100_ctx->gpio_u100_power = devm_gpiod_get_optional(&u100_ctx->spi->dev,
			"u100-power-switch", GPIOD_IN);
	if (IS_ERR_OR_NULL(u100_ctx->gpio_u100_power)) {
		/* Compatible with older versions that haven't power-switch in device tree. */
		UWB_WARN("Init gpio_u100_power failed %d",
				PTR_ERR_OR_ZERO(u100_ctx->gpio_u100_power));
	}

	/* init ap2uwbs sync gpio */
	u100_ctx->gpio_u100_sync = devm_gpiod_get_optional(&u100_ctx->spi->dev, "u100-sync",
								GPIOD_OUT_LOW);
	if (IS_ERR_OR_NULL(u100_ctx->gpio_u100_sync)) {
		UWB_ERR("Init gpio_u100_sync get failed, ret = %d",
				PTR_ERR_OR_ZERO(u100_ctx->gpio_u100_sync));
		return -ENODEV;
	}

	/* init ap2uwbs reset gpio */
	u100_ctx->gpio_u100_reset = devm_gpiod_get_optional(&u100_ctx->spi->dev, "u100-rst",
								GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(u100_ctx->gpio_u100_reset)) {
		UWB_ERR("Init gpio_u100_reset get failed, ret = %d",
			PTR_ERR_OR_ZERO(u100_ctx->gpio_u100_reset));
		return -ENODEV;
	}
	gpiod_direction_output(u100_ctx->gpio_u100_reset, 1);
	UWB_DEBUG("Init gpio_u100_reset num %d\n", desc_to_gpio(u100_ctx->gpio_u100_reset));

	/* init ap2uwbs uwben gpio */
	u100_ctx->gpio_u100_en = devm_gpiod_get_optional(&u100_ctx->spi->dev, "u100-en",
								GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(u100_ctx->gpio_u100_en)) {
		UWB_ERR("Init gpio_u100_en get failed, ret = %d",
			PTR_ERR_OR_ZERO(u100_ctx->gpio_u100_en));
		return -ENODEV;
	}
	gpiod_direction_output(u100_ctx->gpio_u100_en, 1);
	UWB_DEBUG("Init gpio_u100_en num %d\n", desc_to_gpio(u100_ctx->gpio_u100_en));

	/* init uwbs2ap interrupt gpio */
	UWB_DEBUG("Init uwbs2ap interrupt gpiod %d", u100_ctx->spi->irq);

	u100_ctx->gpio_u100_irq.u100_irq = irq_to_desc(u100_ctx->spi->irq);
	if (IS_ERR_OR_NULL(u100_ctx->gpio_u100_irq.u100_irq))
		UWB_WARN("Init irq(%d) desc failed, ret %d", u100_ctx->spi->irq,
			PTR_ERR_OR_ZERO(u100_ctx->gpio_u100_irq.u100_irq));
	uwb_init_irq(&u100_ctx->gpio_u100_irq, u100_ctx->spi->irq,
		"uwb2ap_irq_handler", IRQF_TRIGGER_RISING);
	ret = uwb_request_irq(&u100_ctx->gpio_u100_irq, uwb2ap_irq_handler, u100_ctx);
	if (ret) {
		UWB_ERR("Init uwb_request_irq failed, ret = %d", ret);
		return ret;
	}
	return ret;
}
