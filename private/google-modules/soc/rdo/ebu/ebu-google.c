// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Google LLC
 * Platform driver for Google's EBU IP
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/usb.h>
#include <ebu/ebu.h>

#define EBU_CFG 0x0
#define EBU_TID_TRACE 0x4
#define EBU_TRACE_TIMEOUT 0x8
#define EBU_IN_EP_MAP_LOW 0x1c
#define EBU_IN_EP_MAP_HIGH 0x20

#define EBU_SUPER_SPEED		0
#define EBU_HIGH_SPEED		1

/** enum ebu_state
 * @EBU_STOPPED: Inactive
 * @EBU_RUNNING: Clocks running, reset deasserted
 */
enum ebu_state {
	EBU_STOPPED = 0,
	EBU_RUNNING
};

/**
 * union ebu_cfg
 * @speed: Connected USB controller is running in HS mode. EBU only works
 *	with HS and SS
 * @trace_timeout_en: Timeout behaviour. When enabled, the trace_timeout value
 *	is meaningful
 * @lossy_mode: Whether EBU will drop packets that when the FIFO is full
 * @internal_retry: Whether EBU attempts to retransmit or relies on the USB
 *	controller's retransmission logic
 */
union ebu_cfg {
	uint32_t value;
	struct {
		uint32_t speed : 1;
		uint32_t trace_timeout_en : 1;
		uint32_t lossy_mode : 1;
		uint32_t internal_retry : 1;
	} __packed;
};

/**
 * struct google_ebu - Representation of a google EBU
 * @ebu_state: Whether any channel is being routed to an active EP via the EBU
 * @ebu: Generic EBU object exposed to consumers (callbacks and context)
 * @dev: Device
 * @csr_base: Base address for configuration registers
 * @ebu_clk: Must be enabled before config changes and data transmission
 * @ebu_rst: Must be released before config changes and data transmission
 * @fifo_base: Physical address where USB controller can access the head of the
 *	fifo. NB: Currently there is only one channel and hence only one FIFO
 * @fifo_size: Valid address range for the FIFO
 * @trace_tid: Source from which this EBU accepts data
 * @ebu_cfg: Global configuration for this EBU. Cache settings here when the
 *	EBU isn't enabled
 * @trace_timeout: Max timeout before which EBU raises the buffer_avail line
 *	Usually buffer_avail goes high when EBU has >= 1 MPS bytes available. When
 *	trace timeout is enabled, EBU will pad out a packet and raise the line when
 *	this timeout expires
 * @ch_use: Bitmap indicating for each channel whether it has been assigned
 * @ch_map: Map of channels to endpoints
 * @ep_map: Map of endpoints to channels. ch_map and ep_map are complementary
 * @lock: Serialize access
 */
struct google_ebu {
	enum ebu_state state;
	struct ebu_controller ebu;
	struct device *dev;
	void __iomem *csr_base;
	struct clk *ebu_clk;
	struct reset_control *ebu_rst;
	dma_addr_t fifo_base;
	size_t fifo_size;
	u32 trace_tid;
	union ebu_cfg ebu_cfg;
	uint32_t trace_timeout;
	u16 ch_use;
	u64 ch_map;
	u64 ep_map;
	struct mutex lock;
};

uint32_t ebu_readl(struct google_ebu *gebu, uint32_t offset)
{
	return readl(gebu->csr_base + offset);
}

void ebu_writel(struct google_ebu *gebu, uint32_t offset, uint32_t value)
{
	writel(value, gebu->csr_base + offset);
}

#define NIBBLE_POS(idx) (4 * (idx))
#define NIBBLE_MASK(idx) ((uint64_t)0xf << NIBBLE_POS(idx))
#define GET_NIBBLE(dword, idx) (((dword) >> NIBBLE_POS(idx)) & 0xf)

int reinit_ebu(struct google_ebu *gebu)
{
	ebu_writel(gebu, EBU_TID_TRACE, gebu->trace_tid);
	ebu_writel(gebu, EBU_TRACE_TIMEOUT, gebu->trace_timeout);
	ebu_writel(gebu, EBU_CFG, gebu->ebu_cfg.value);
	ebu_writel(gebu, EBU_IN_EP_MAP_LOW, (uint32_t)gebu->ep_map);
	ebu_writel(gebu, EBU_IN_EP_MAP_HIGH, gebu->ep_map >> 32);
	return 0;
}

void stop_ebu(struct google_ebu *gebu)
{
	reset_control_assert(gebu->ebu_rst);
	clk_disable_unprepare(gebu->ebu_clk);
	gebu->state = EBU_STOPPED;
}

int google_ebu_add_mapping(struct ebu_controller *ebu, u8 channel, u8 epaddr)
{
	int ret = 0;
	u8 prev_channel, prev_epnum;
	u8 epnum = epaddr & USB_ENDPOINT_NUMBER_MASK;
	struct google_ebu *gebu = container_of(ebu, struct google_ebu, ebu);

	if (!(epaddr & USB_DIR_IN)) {
		/* Limitation of RDO EBU */
		dev_err(gebu->dev, "Buffer only supports IN transfers\n");
		return -EINVAL;
	}

	if (channel > 0xf) {
		dev_err(gebu->dev, "Channel out of range\n");
		return -EINVAL;
	}

	mutex_lock(&gebu->lock);
	if (gebu->ch_use & (0x1 << channel)) {
		dev_err(gebu->dev, "Channel already assigned\n");
		ret = -EBUSY;
		goto unlock;
	}

	prev_epnum = GET_NIBBLE(gebu->ch_map, channel);
	prev_channel = GET_NIBBLE(gebu->ep_map, epnum);

	/* All the LOW/HIGH BIT_<index> should be programmed to unique value
	 * To maintain this, we need to swap the nibble corresponding to epnum
	 * with the nibble corresponding to the ep where channel was prev mapped
	 */
	gebu->ep_map &= ~(NIBBLE_MASK(epnum) | NIBBLE_MASK(prev_epnum));
	gebu->ep_map |= ((uint64_t)channel << NIBBLE_POS(epnum)) |
			((uint64_t)prev_channel << NIBBLE_POS(prev_epnum));

	/* Maintain channel-ep (reverse) map to avoid a linear search */
	gebu->ch_map &= ~(NIBBLE_MASK(channel) | NIBBLE_MASK(prev_channel));
	gebu->ch_map |= ((uint64_t)epnum << NIBBLE_POS(channel)) |
			((uint64_t)prev_epnum << NIBBLE_POS(prev_channel));

	gebu->ch_use |= (0x1 << channel);
unlock:
	mutex_unlock(&gebu->lock);
	return ret;
}

int google_ebu_release_mapping(struct ebu_controller *ebu, u8 channel)
{
	int ret = 0;
	struct google_ebu *gebu = container_of(ebu, struct google_ebu, ebu);

	if (channel > 0xf) {
		dev_err(gebu->dev, "Channel out of range\n");
		return -EINVAL;
	}

	mutex_lock(&gebu->lock);
	if ((gebu->ch_use & (0x1 << channel)) == 0) {
		ret = -EINVAL;
		goto unlock;
	}

	gebu->ch_use &= ~(0x1 << channel);
unlock:
	mutex_unlock(&gebu->lock);
	return ret;
}

int google_ebu_enable_data(struct ebu_controller *ebu, enum usb_device_speed speed)
{
	int ret;
	struct google_ebu *gebu = container_of(ebu, struct google_ebu, ebu);

	mutex_lock(&gebu->lock);
	switch (speed) {
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		gebu->ebu_cfg.speed = EBU_SUPER_SPEED;
		break;
	case USB_SPEED_HIGH:
		gebu->ebu_cfg.speed = EBU_HIGH_SPEED;
		break;
	default:
		/* EBU Only supports HS, SS and SSP */
		ret = -EINVAL;
		goto unlock;
	}
	ret = reinit_ebu(gebu);
unlock:
	mutex_unlock(&gebu->lock);
	return ret;
}

dma_addr_t google_ebu_get_fifo(struct ebu_controller *ebu, u8 channel)
{
	struct google_ebu *gebu = container_of(ebu, struct google_ebu, ebu);

	return gebu->fifo_base;
}

static int google_ebu_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct google_ebu *gebu;
	struct resource *fifo_res;

	dev_dbg(dev, "google_ebu probe\n");
	gebu = devm_kzalloc(dev, sizeof(*gebu), GFP_KERNEL);
	if (!gebu)
		return -ENOMEM;

	gebu->dev = dev;

	gebu->ebu_clk = devm_clk_get(gebu->dev, "ebu_clk");
	if (IS_ERR(gebu->ebu_clk))
		return PTR_ERR(gebu->ebu_clk);

	gebu->ebu_rst = devm_reset_control_get_exclusive(gebu->dev, "ebu_rst");
	if (IS_ERR(gebu->ebu_rst))
		return PTR_ERR(gebu->ebu_rst);

	gebu->csr_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gebu->csr_base)) {
		dev_err(gebu->dev, "Couldn't Remap EBU CSRs\n");
		return PTR_ERR(gebu->csr_base);
	}

	fifo_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ebu_fifo");
	if (!fifo_res) {
		dev_err(gebu->dev, "No FIFO specified for EBU\n");
		return -ENODEV;
	}

	gebu->fifo_base = (dma_addr_t)fifo_res->start;
	gebu->fifo_size = 1 + fifo_res->end - fifo_res->start;

	ret = of_property_read_u32(gebu->dev->of_node, "tid", &gebu->trace_tid);
	if (ret) {
		dev_err(gebu->dev, "No TID specified for EBU\n");
		return ret;
	}

	gebu->ebu_cfg.speed = EBU_SUPER_SPEED;
	gebu->ebu_cfg.trace_timeout_en = false;
	gebu->ebu_cfg.lossy_mode = true;
	gebu->ebu_cfg.internal_retry = true;
	gebu->trace_timeout = 1000;

	gebu->ep_map = 0x0123456789abcdef;
	gebu->ch_map = 0x0123456789abcdef;
	gebu->ch_use = 0x0;

	gebu->ebu.add_mapping = google_ebu_add_mapping;
	gebu->ebu.release_mapping = google_ebu_release_mapping;
	gebu->ebu.get_fifo = google_ebu_get_fifo;
	gebu->ebu.enable_data = google_ebu_enable_data;
	mutex_init(&gebu->lock);
	platform_set_drvdata(pdev, &gebu->ebu);

	ret = clk_prepare_enable(gebu->ebu_clk);
	if (ret) {
		dev_err(gebu->dev, "ebu clock on failed:%d\n", ret);
		return ret;
	}

	ret = reset_control_deassert(gebu->ebu_rst);
	if (ret) {
		dev_err(gebu->dev, "ebu reset deassert failed:%d\n", ret);
		clk_disable_unprepare(gebu->ebu_clk);
		return ret;
	}
	gebu->state = EBU_RUNNING;

	return 0;
}

static int google_ebu_remove(struct platform_device *pdev)
{
	struct ebu_controller *ebu = platform_get_drvdata(pdev);
	struct google_ebu *gebu = container_of(ebu, struct google_ebu, ebu);

	stop_ebu(gebu);
	mutex_destroy(&gebu->lock);
	return 0;
}

static const struct of_device_id google_ebu_match[] = {
	{ .compatible = "google,ebu" },
	{},
};

static struct platform_driver google_ebu_driver = {
	.probe = google_ebu_probe,
	.remove = google_ebu_remove,
	.driver = {
		.name = "google-ebu",
		.owner = THIS_MODULE,
		.of_match_table = google_ebu_match,
	}
};

module_platform_driver(google_ebu_driver);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Driver for Google's EBU IP.");
MODULE_LICENSE("GPL");
