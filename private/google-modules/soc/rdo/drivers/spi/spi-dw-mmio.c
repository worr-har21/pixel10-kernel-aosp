// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory-mapped interface driver for DW SPI Core
 *
 * Copyright (c) 2010, Octasic semiconductor.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/scatterlist.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/acpi.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "spi-dw.h"

#define DRIVER_NAME "dw_spi_mmio"

/*
 * TODO(b/281621411): Start with conservative value
 * and update for observability.
 */
#define RPM_AUTOSUSPEND_DELAY_MS 200
#define SPI_DMA_16B_ALIGN_ONLY  BIT(0)
#define GOOGLE_SPI_GPIO_CS_WITH_CPOL_HIGH  BIT(1)
#define GOOGLE_SPI_OVERWRITE_DMA_TX_BURST  BIT(2)

struct dw_spi_mmio {
	struct dw_spi  dws;
	struct clk     *clk;
	struct clk     *pclk;
	void           *priv;
	struct reset_control *rstc;
	struct dw_spi_dma_ops dma_ops_modified;
	int quirks;
	struct pinctrl *pinctrl;
	struct pinctrl_state *cli_state;
	u32 cs_delay;
};

#define MSCC_CPU_SYSTEM_CTRL_GENERAL_CTRL	0x24
#define OCELOT_IF_SI_OWNER_OFFSET		4
#define JAGUAR2_IF_SI_OWNER_OFFSET		6
#define MSCC_IF_SI_OWNER_MASK			GENMASK(1, 0)
#define MSCC_IF_SI_OWNER_SISL			0
#define MSCC_IF_SI_OWNER_SIBM			1
#define MSCC_IF_SI_OWNER_SIMC			2

#define MSCC_SPI_MST_SW_MODE			0x14
#define MSCC_SPI_MST_SW_MODE_SW_PIN_CTRL_MODE	BIT(13)
#define MSCC_SPI_MST_SW_MODE_SW_SPI_CS(x)	(x << 5)

#define SPARX5_FORCE_ENA			0xa4
#define SPARX5_FORCE_VAL			0xa8

struct dw_spi_mscc {
	struct regmap       *syscon;
	void __iomem        *spi_mst; /* Not sparx5 */
};

/*
 * Elba SoC does not use ssi, pin override is used for cs 0,1 and
 * gpios for cs 2,3 as defined in the device tree.
 *
 * cs:  |       1               0
 * bit: |---3-------2-------1-------0
 *      |  cs1   cs1_ovr   cs0   cs0_ovr
 */
#define ELBA_SPICS_REG			0x2468
#define ELBA_SPICS_OFFSET(cs)		((cs) << 1)
#define ELBA_SPICS_MASK(cs)		(GENMASK(1, 0) << ELBA_SPICS_OFFSET(cs))
#define ELBA_SPICS_SET(cs, val)		\
		((((val) << 1) | BIT(0)) << ELBA_SPICS_OFFSET(cs))

/*
 * The Designware SPI controller (referred to as master in the documentation)
 * automatically deasserts chip select when the tx fifo is empty. The chip
 * selects then needs to be either driven as GPIOs or, for the first 4 using
 * the SPI boot controller registers. the final chip select is an OR gate
 * between the Designware SPI controller and the SPI boot controller.
 */
static void dw_spi_mscc_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct dw_spi_mmio *dwsmmio = container_of(dws, struct dw_spi_mmio, dws);
	struct dw_spi_mscc *dwsmscc = dwsmmio->priv;
	u32 cs = spi_get_chipselect(spi, 0);

	if (cs < 4) {
		u32 sw_mode = MSCC_SPI_MST_SW_MODE_SW_PIN_CTRL_MODE;

		if (!enable)
			sw_mode |= MSCC_SPI_MST_SW_MODE_SW_SPI_CS(BIT(cs));

		writel(sw_mode, dwsmscc->spi_mst + MSCC_SPI_MST_SW_MODE);
	}

	dw_spi_set_cs(spi, enable);
}

static int dw_spi_mscc_init(struct platform_device *pdev,
			    struct dw_spi_mmio *dwsmmio,
			    const char *cpu_syscon, u32 if_si_owner_offset)
{
	struct dw_spi_mscc *dwsmscc;

	dwsmscc = devm_kzalloc(&pdev->dev, sizeof(*dwsmscc), GFP_KERNEL);
	if (!dwsmscc)
		return -ENOMEM;

	dwsmscc->spi_mst = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(dwsmscc->spi_mst)) {
		dev_err(&pdev->dev, "SPI_MST region map failed\n");
		return PTR_ERR(dwsmscc->spi_mst);
	}

	dwsmscc->syscon = syscon_regmap_lookup_by_compatible(cpu_syscon);
	if (IS_ERR(dwsmscc->syscon))
		return PTR_ERR(dwsmscc->syscon);

	/* Deassert all CS */
	writel(0, dwsmscc->spi_mst + MSCC_SPI_MST_SW_MODE);

	/* Select the owner of the SI interface */
	regmap_update_bits(dwsmscc->syscon, MSCC_CPU_SYSTEM_CTRL_GENERAL_CTRL,
			   MSCC_IF_SI_OWNER_MASK << if_si_owner_offset,
			   MSCC_IF_SI_OWNER_SIMC << if_si_owner_offset);

	dwsmmio->dws.set_cs = dw_spi_mscc_set_cs;
	dwsmmio->priv = dwsmscc;

	return 0;
}

static int dw_spi_mscc_ocelot_init(struct platform_device *pdev,
				   struct dw_spi_mmio *dwsmmio)
{
	return dw_spi_mscc_init(pdev, dwsmmio, "mscc,ocelot-cpu-syscon",
				OCELOT_IF_SI_OWNER_OFFSET);
}

static int dw_spi_mscc_jaguar2_init(struct platform_device *pdev,
				    struct dw_spi_mmio *dwsmmio)
{
	return dw_spi_mscc_init(pdev, dwsmmio, "mscc,jaguar2-cpu-syscon",
				JAGUAR2_IF_SI_OWNER_OFFSET);
}

/*
 * The Designware SPI controller (referred to as master in the
 * documentation) automatically deasserts chip select when the tx fifo
 * is empty. The chip selects then needs to be driven by a CS override
 * register. enable is an active low signal.
 */
static void dw_spi_sparx5_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct dw_spi_mmio *dwsmmio = container_of(dws, struct dw_spi_mmio, dws);
	struct dw_spi_mscc *dwsmscc = dwsmmio->priv;
	u8 cs = spi_get_chipselect(spi, 0);

	if (!enable) {
		/* CS override drive enable */
		regmap_write(dwsmscc->syscon, SPARX5_FORCE_ENA, 1);
		/* Now set CSx enabled */
		regmap_write(dwsmscc->syscon, SPARX5_FORCE_VAL, ~BIT(cs));
		/* Allow settle */
		usleep_range(1, 5);
	} else {
		/* CS value */
		regmap_write(dwsmscc->syscon, SPARX5_FORCE_VAL, ~0);
		/* Allow settle */
		usleep_range(1, 5);
		/* CS override drive disable */
		regmap_write(dwsmscc->syscon, SPARX5_FORCE_ENA, 0);
	}

	dw_spi_set_cs(spi, enable);
}

static int dw_spi_mscc_sparx5_init(struct platform_device *pdev,
				   struct dw_spi_mmio *dwsmmio)
{
	const char *syscon_name = "microchip,sparx5-cpu-syscon";
	struct device *dev = &pdev->dev;
	struct dw_spi_mscc *dwsmscc;

	if (!IS_ENABLED(CONFIG_SPI_MUX)) {
		dev_err(dev, "This driver needs CONFIG_SPI_MUX\n");
		return -EOPNOTSUPP;
	}

	dwsmscc = devm_kzalloc(dev, sizeof(*dwsmscc), GFP_KERNEL);
	if (!dwsmscc)
		return -ENOMEM;

	dwsmscc->syscon =
		syscon_regmap_lookup_by_compatible(syscon_name);
	if (IS_ERR(dwsmscc->syscon)) {
		dev_err(dev, "No syscon map %s\n", syscon_name);
		return PTR_ERR(dwsmscc->syscon);
	}

	dwsmmio->dws.set_cs = dw_spi_sparx5_set_cs;
	dwsmmio->priv = dwsmscc;

	return 0;
}

static int dw_spi_alpine_init(struct platform_device *pdev,
			      struct dw_spi_mmio *dwsmmio)
{
	dwsmmio->dws.caps = DW_SPI_CAP_CS_OVERRIDE;

	return 0;
}

static int dw_spi_pssi_init(struct platform_device *pdev,
			    struct dw_spi_mmio *dwsmmio)
{
	dw_spi_dma_setup_generic(&dwsmmio->dws);

	return 0;
}

static int dw_spi_hssi_init(struct platform_device *pdev,
			    struct dw_spi_mmio *dwsmmio)
{
	dwsmmio->dws.ip = DW_HSSI_ID;

	dw_spi_dma_setup_generic(&dwsmmio->dws);

	return 0;
}

static int dw_spi_intel_init(struct platform_device *pdev,
			     struct dw_spi_mmio *dwsmmio)
{
	dwsmmio->dws.ip = DW_HSSI_ID;

	return 0;
}

/*
 * DMA-based mem ops are not configured for this device and are not tested.
 */
static int dw_spi_mountevans_imc_init(struct platform_device *pdev,
				      struct dw_spi_mmio *dwsmmio)
{
	/*
	 * The Intel Mount Evans SoC's Integrated Management Complex DW
	 * apb_ssi_v4.02a controller has an errata where a full TX FIFO can
	 * result in data corruption. The suggested workaround is to never
	 * completely fill the FIFO. The TX FIFO has a size of 32 so the
	 * fifo_len is set to 31.
	 */
	dwsmmio->dws.fifo_len = 31;

	return 0;
}

static int dw_spi_canaan_k210_init(struct platform_device *pdev,
				   struct dw_spi_mmio *dwsmmio)
{
	/*
	 * The Canaan Kendryte K210 SoC DW apb_ssi v4 spi controller is
	 * documented to have a 32 word deep TX and RX FIFO, which
	 * spi_hw_init() detects. However, when the RX FIFO is filled up to
	 * 32 entries (RXFLR = 32), an RX FIFO overrun error occurs. Avoid this
	 * problem by force setting fifo_len to 31.
	 */
	dwsmmio->dws.fifo_len = 31;

	return 0;
}

static void dw_spi_elba_override_cs(struct regmap *syscon, int cs, int enable)
{
	regmap_update_bits(syscon, ELBA_SPICS_REG, ELBA_SPICS_MASK(cs),
			   ELBA_SPICS_SET(cs, enable));
}

static void dw_spi_elba_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct dw_spi_mmio *dwsmmio = container_of(dws, struct dw_spi_mmio, dws);
	struct regmap *syscon = dwsmmio->priv;
	u8 cs;

	cs = spi_get_chipselect(spi, 0);
	if (cs < 2)
		dw_spi_elba_override_cs(syscon, spi_get_chipselect(spi, 0), enable);

	/*
	 * The DW SPI controller needs a native CS bit selected to start
	 * the serial engine.
	 */
	spi_set_chipselect(spi, 0, 0);
	dw_spi_set_cs(spi, enable);
	spi_set_chipselect(spi, 0, cs);
}

static int dw_spi_elba_init(struct platform_device *pdev,
			    struct dw_spi_mmio *dwsmmio)
{
	struct regmap *syscon;

	syscon = syscon_regmap_lookup_by_phandle(dev_of_node(&pdev->dev),
						 "amd,pensando-elba-syscon");
	if (IS_ERR(syscon))
		return dev_err_probe(&pdev->dev, PTR_ERR(syscon),
				     "syscon regmap lookup failed\n");

	dwsmmio->priv = syscon;
	dwsmmio->dws.set_cs = dw_spi_elba_set_cs;

	return 0;
}

static bool spi_google_can_dma(struct spi_controller *master,
			       struct spi_device *spi,
			       struct spi_transfer *xfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);
	dma_addr_t addr_align = (dma_addr_t)xfer->tx_buf |
				(dma_addr_t)xfer->rx_buf;

	if (xfer->len > dws->fifo_len)
		if (IS_ALIGNED(xfer->len, 16) && IS_ALIGNED(addr_align, 16))
			return true;
	return false;
}

static void spi_google_set_idle_cpol_high(struct device *dev, struct dw_spi_mmio *dwsmmio)
{
	struct dw_spi *dws = &dwsmmio->dws;
	int retry = DW_SPI_WAIT_RETRIES;
	u32 cr0 = 0;

	/*
	 * The DW SSI IP drives the clock polarity as low to the pads on reset, when
	 * there is a switch in clock polarity the controller corrects the polarity right
	 * before asserting the native CS.
	 * The same does not happen when a GPIO Chip Select is used since the SPI core
	 * asserts the CS before the controller starts the transfer leading to the clock
	 * polarity changing after CS assertion which is seen as an edge to the device.
	 *
	 * Doing a 1 byte transfer with CPHA high when the pads are not driven by the
	 * controller sets the steady state polarity as high.
	 *
	 * This helps workaround this issue but only if all devices connected to the host
	 * operate with the same clock polarity setting.
	 */

	dw_spi_reset_chip(dws);
	dw_spi_enable_chip(dws, 0);
	/* Configure divider for 10Mhz*/
	dw_spi_set_clk(dws, (dws->max_freq / 10000000) & 0xffe);
	cr0 |= FIELD_PREP(DW_PSSI_CTRLR0_FRF_MASK, DW_SPI_CTRLR0_FRF_MOTO_SPI);
	cr0 |= FIELD_PREP(DW_PSSI_CTRLR0_TMOD_MASK, DW_SPI_CTRLR0_TMOD_TO);
	cr0 |= FIELD_PREP(DW_PSSI_CTRLR0_DFS32_MASK, 0x7);
	cr0 |= DW_PSSI_CTRLR0_SCPOL;
	dw_writel(dws, DW_SPI_CTRLR0, cr0);
	dw_spi_enable_chip(dws, 1);
	dw_writel(dws, DW_SPI_DR, 0xAA);
	dw_writel(dws, DW_SPI_SER, BIT(0));

	/* Poll for completion which should take less than 1us at 10Mhz*/
	while (dw_readl(dws, DW_SPI_SR) & DW_SPI_SR_BUSY && retry--)
		udelay(1);

	if (retry < 0)
		dev_err(dev, "Transfer timed out\n");
}

static void spi_google_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct dw_spi_mmio *dwsmmio = container_of(dws, struct dw_spi_mmio, dws);
	bool cs_high = !!(spi->mode & SPI_CS_HIGH);

	/*
	 * Google DW SSI IP is synthesized with SSI_NUM_SLAVES = 1 hence always
	 * select chip select index 0 to initiate SPI transactions.
	 * Transfers to multiple slaves are carried out with GPIO Chip Select
	 * control.
	 */

	if (cs_high == enable)
		dw_writel(dws, DW_SPI_SER, BIT(0));
	else
		dw_writel(dws, DW_SPI_SER, 0);

	/* Add extra delay between CS and clock if requested */
	if (enable && dwsmmio->cs_delay)
		usleep_range(dwsmmio->cs_delay, dwsmmio->cs_delay + 10);
}

static int spi_google_init(struct platform_device *pdev,
			   struct dw_spi_mmio *dwsmmio)
{
	int num_cs_gpios;

	if (device_property_present(&pdev->dev, "spi-dma-16B-align-only"))
		dwsmmio->quirks |= SPI_DMA_16B_ALIGN_ONLY;

	if (device_property_present(&pdev->dev, "google,spi-gpio-cs-with-cpol-high")) {
		num_cs_gpios = gpiod_count(&pdev->dev, "cs");
		if (num_cs_gpios > 0)
			dwsmmio->quirks |= GOOGLE_SPI_GPIO_CS_WITH_CPOL_HIGH;
		else
			dev_warn(&pdev->dev,
				 "google,spi-gpio-cs-with-cpol-high set without GPIO based CS\n");
	}

	dw_spi_dma_setup_generic(&dwsmmio->dws);
	dwsmmio->quirks |= GOOGLE_SPI_OVERWRITE_DMA_TX_BURST;

	if (dwsmmio->quirks & SPI_DMA_16B_ALIGN_ONLY && dwsmmio->dws.dma_ops) {
		memcpy(&dwsmmio->dma_ops_modified, dwsmmio->dws.dma_ops,
		       sizeof(struct dw_spi_dma_ops));
		dwsmmio->dma_ops_modified.can_dma = spi_google_can_dma;
		dwsmmio->dws.dma_ops = &dwsmmio->dma_ops_modified;
	}

	if (dwsmmio->quirks & GOOGLE_SPI_GPIO_CS_WITH_CPOL_HIGH) {
		pinctrl_pm_select_sleep_state(&pdev->dev);
		spi_google_set_idle_cpol_high(&pdev->dev, dwsmmio);
		pinctrl_pm_select_default_state(&pdev->dev);
	}

	dwsmmio->dws.set_cs = spi_google_set_cs;

	return 0;
}

static int dw_spi_mmio_setup(struct device *dev,
			     struct dw_spi_mmio *dwsmmio)
{
	int ret;

	ret = clk_prepare_enable(dwsmmio->clk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dwsmmio->pclk);
	if (ret)
		return ret;
	ret = reset_control_assert(dwsmmio->rstc);
	if (ret)
		return ret;

	return reset_control_deassert(dwsmmio->rstc);
}

static int dw_spi_mmio_probe(struct platform_device *pdev)
{
	int (*init_func)(struct platform_device *pdev,
			 struct dw_spi_mmio *dwsmmio);
	struct dw_spi_mmio *dwsmmio;
	struct resource *mem;
	struct dw_spi *dws;
	bool ext_power_control;
	int ret;
	int num_cs;

	dwsmmio = devm_kzalloc(&pdev->dev, sizeof(struct dw_spi_mmio),
			       GFP_KERNEL);
	if (!dwsmmio)
		return -ENOMEM;

	dws = &dwsmmio->dws;

	/* Get basic io resource and map it */
	dws->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (IS_ERR(dws->regs))
		return PTR_ERR(dws->regs);

	dws->paddr = mem->start;

	dws->irq = platform_get_irq(pdev, 0);
	if (dws->irq < 0)
		return dws->irq; /* -ENXIO */

	dwsmmio->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dwsmmio->clk))
		return PTR_ERR(dwsmmio->clk);

	/* Optional clock needed to access the registers */
	dwsmmio->pclk = devm_clk_get_optional(&pdev->dev, "pclk");
	if (IS_ERR(dwsmmio->pclk)) {
		ret = PTR_ERR(dwsmmio->pclk);
		goto out_clk;
	}

	/* find an optional reset controller */
	dwsmmio->rstc = devm_reset_control_get_optional_exclusive(&pdev->dev, "spi");
	if (IS_ERR(dwsmmio->rstc)) {
		ret = PTR_ERR(dwsmmio->rstc);
		goto out_clk;
	}

	ret = dw_spi_mmio_setup(&pdev->dev, dwsmmio);
	if (ret)
		goto out;

	dws->bus_num = pdev->id;

	dws->max_freq = clk_get_rate(dwsmmio->clk);

	if (device_property_read_u32(&pdev->dev, "reg-io-width",
				     &dws->reg_io_width))
		dws->reg_io_width = 4;

	num_cs = 4;

	device_property_read_u32(&pdev->dev, "num-cs", &num_cs);

	dws->num_cs = num_cs;

	device_property_read_u32(&pdev->dev, "google,cs-clock-delay-us", &dwsmmio->cs_delay);

	init_func = device_get_match_data(&pdev->dev);
	if (init_func) {
		ret = init_func(pdev, dwsmmio);
		if (ret)
			goto out;
	}

	dwsmmio->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (!IS_ERR(dwsmmio->pinctrl)) {
		dwsmmio->cli_state = pinctrl_lookup_state(dwsmmio->pinctrl, "cli");
		if (IS_ERR(dwsmmio->cli_state))
			dwsmmio->cli_state = NULL;
	} else {
		dwsmmio->pinctrl = NULL;
	}

	/* We must set drvdata before enabling pm_runtime. We have to expect
	 * for our suspend and resume callbacks to be called as soon as
	 * pm_runtime is enabled, and those functions reference drvdata.
	 */
	platform_set_drvdata(pdev, dwsmmio);

	/* Increment PM usage to avoid possible spurious runtime suspend */
	pm_runtime_get_noresume(&pdev->dev);

	WARN_ON(pm_runtime_enabled(&pdev->dev));
	ext_power_control = of_property_read_bool(pdev->dev.of_node,
						  "external-power-control");
	if (!ext_power_control) {
		pm_runtime_set_autosuspend_delay(&pdev->dev,
						 RPM_AUTOSUSPEND_DELAY_MS);
		pm_runtime_use_autosuspend(&pdev->dev);
	}
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = dw_spi_add_host(&pdev->dev, dws);
	if (ret)
		goto out;

	if (dws->host->can_dma && dwsmmio->quirks & GOOGLE_SPI_OVERWRITE_DMA_TX_BURST) {
		dws->txburst = dws->fifo_len / 2;
		dw_writel(dws, DW_SPI_DMATDLR, dws->txburst);
	}

	platform_set_drvdata(pdev, dwsmmio);

	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	if (ext_power_control)
		pm_runtime_suspend(&pdev->dev);

	return 0;

out:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	clk_disable_unprepare(dwsmmio->pclk);
out_clk:
	clk_disable_unprepare(dwsmmio->clk);
	reset_control_assert(dwsmmio->rstc);

	return ret;
}

static void dw_spi_mmio_remove(struct platform_device *pdev)
{
	struct dw_spi_mmio *dwsmmio = platform_get_drvdata(pdev);

	dw_spi_remove_host(&dwsmmio->dws);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	clk_disable_unprepare(dwsmmio->pclk);
	clk_disable_unprepare(dwsmmio->clk);
	reset_control_assert(dwsmmio->rstc);
}

#if IS_ENABLED(CONFIG_PM)
/**
 * dw_spi_mmio_runtime_suspend - disable controller,
 * gate ip clock and apb clocks
 * and reconfigure pins to GPIO open drain
 * @dev: pointer to spi device
 */
static int dw_spi_mmio_runtime_suspend(struct device *dev)
{
	struct dw_spi_mmio *dwsmmio = dev_get_drvdata(dev);
	struct dw_spi *dws = &dwsmmio->dws;
	int ret;

	ret = pinctrl_pm_select_sleep_state(dev);
	if (ret)
		return ret;

	disable_irq(dws->irq);
	dw_spi_shutdown_chip(dws);
	clk_disable_unprepare(dwsmmio->pclk);
	clk_disable_unprepare(dwsmmio->clk);

	return 0;
}

static int dw_spi_mmio_runtime_resume(struct device *dev)
{
	struct dw_spi_mmio *dwsmmio = dev_get_drvdata(dev);
	struct dw_spi *dws = &dwsmmio->dws;
	int ret;

	ret = dw_spi_mmio_setup(dev, dwsmmio);
	if (ret)
		return ret;

	if (dwsmmio->cli_state) {
		ret = pinctrl_select_state(dwsmmio->pinctrl, dwsmmio->cli_state);
		if (ret)
			return ret;
	}

	dws->current_freq = 0;
	dws->cur_rx_sample_dly = 0;

	if (dwsmmio->quirks & GOOGLE_SPI_GPIO_CS_WITH_CPOL_HIGH)
		spi_google_set_idle_cpol_high(dev, dwsmmio);

	dw_spi_reset_chip(dws);
	enable_irq(dws->irq);

	if (dws->host->can_dma) {
		dw_writel(dws, DW_SPI_DMATDLR, dws->txburst);
		dw_writel(dws, DW_SPI_DMARDLR, dws->rxburst - 1);
	}

	return pinctrl_pm_select_default_state(dev);
}
#endif /* CONFIG_PM */

#if IS_ENABLED(CONFIG_PM_SLEEP)
static int dw_spi_mmio_suspend(struct device *dev)
{
	struct dw_spi_mmio *dwsmmio = dev_get_drvdata(dev);
	struct dw_spi *dws = &dwsmmio->dws;
	int ret;

	ret = spi_controller_suspend(dws->host);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int dw_spi_mmio_resume(struct device *dev)
{
	struct dw_spi_mmio *dwsmmio = dev_get_drvdata(dev);
	struct dw_spi *dws = &dwsmmio->dws;
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	return spi_controller_resume(dws->host);
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops dw_spi_mmio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw_spi_mmio_suspend, dw_spi_mmio_resume)
	SET_RUNTIME_PM_OPS(dw_spi_mmio_runtime_suspend, dw_spi_mmio_runtime_resume, NULL)
};

static const struct of_device_id dw_spi_mmio_of_match[] = {
	{ .compatible = "snps,dw-apb-ssi", .data = dw_spi_pssi_init},
	{ .compatible = "mscc,ocelot-spi", .data = dw_spi_mscc_ocelot_init},
	{ .compatible = "mscc,jaguar2-spi", .data = dw_spi_mscc_jaguar2_init},
	{ .compatible = "amazon,alpine-dw-apb-ssi", .data = dw_spi_alpine_init},
	{ .compatible = "renesas,rzn1-spi", .data = dw_spi_pssi_init},
	{ .compatible = "snps,dwc-ssi-1.01a", .data = dw_spi_hssi_init},
	{ .compatible = "intel,keembay-ssi", .data = dw_spi_intel_init},
	{ .compatible = "intel,thunderbay-ssi", .data = dw_spi_intel_init},
		{
		.compatible = "intel,mountevans-imc-ssi",
		.data = dw_spi_mountevans_imc_init,
	},
	{ .compatible = "microchip,sparx5-spi", dw_spi_mscc_sparx5_init},
	{ .compatible = "canaan,k210-spi", dw_spi_canaan_k210_init},
	{ .compatible = "amd,pensando-elba-spi", .data = dw_spi_elba_init},
	{ .compatible = "google,spi", .data = spi_google_init },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, dw_spi_mmio_of_match);

#ifdef CONFIG_ACPI
static const struct acpi_device_id dw_spi_mmio_acpi_match[] = {
	{"HISI0173", (kernel_ulong_t)dw_spi_pssi_init},
	{},
};
MODULE_DEVICE_TABLE(acpi, dw_spi_mmio_acpi_match);
#endif

static struct platform_driver dw_spi_mmio_driver = {
	.probe		= dw_spi_mmio_probe,
	.remove_new	= dw_spi_mmio_remove,
	.driver		= {
		.name	= DRIVER_NAME,
		.of_match_table = dw_spi_mmio_of_match,
		.acpi_match_table = ACPI_PTR(dw_spi_mmio_acpi_match),
		.pm = &dw_spi_mmio_pm_ops,
	},
};
module_platform_driver(dw_spi_mmio_driver);

MODULE_AUTHOR("Jean-Hugues Deschenes <jean-hugues.deschenes@octasic.com>");
MODULE_DESCRIPTION("Memory-mapped I/O interface driver for DW SPI Core");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SPI_DW_CORE);
