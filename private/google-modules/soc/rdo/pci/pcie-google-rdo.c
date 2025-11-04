// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2021-2024 Google LLC
 */

#include <linux/pcie_google_if.h>
#include "pcie-google.h"

#include <linux/version.h>

#define CREATE_TRACE_POINTS
#include "pcie_trace.h"

#define DEFAULT_PERST_DELAY_US		20000
#define LINK_UP_MAX_RETRIES		4800
#define LINK_UP_WAIT_TIME_US		10
#define MAX_L2_COUNT			2000
#define L2_PME_TO_ACK_WAIT_US		10
#define SRAM_INIT_DELAY_US		100
#define SRAM_INIT_TIMEOUT_US		1000
#define WIDTH_SPEED_CHANGE_WAIT_US	10
#define MAX_TIMEOUT_WIDTH_SPEED_CHANGE	10000 /* Link width/speed change timeout (in 10us units) */

/* SII region offsets */
#define LINK_RST_STATUS			0x1c
#define SMLH_REQ_RST_NOT		BIT(1)

#define PM_UNLOCK_ERR_MSG		0x24
#define APPS_PM_XMT_TURNOFF		BIT(0)
#define RADM_PM_TO_ACK			BIT(7)

#define PM_STATE			0x28
#define CURRENT_STATE(x)		((x) & (GENMASK(2, 0)))
#define LTSSM_STATE(x)			(((x) & (GENMASK(8, 3))) >> 3)
#define S_L0				0x11
#define LINK_IN_L0S			BIT(12)
#define LINK_IN_L1			BIT(13)
#define LINK_IN_L2			BIT(14)
#define MASTER_STATE			(((x) & (GENMASK(20, 16))) >> 16)
#define SLAVE_STATE			(((x) & (GENMASK(25, 21))) >> 21)
#define L1SUB_STATE			(((x) & (GENMASK(28, 26))) >> 26)
#define LINK_IN_L1SUB			BIT(30)

#define PM_CTRL_STATUS			0x2C
#define L1_PWR_OFF_EN			BIT(11)

/* TOP region offsets */
#define PCIE_PHY_CFG1			0x0
#define PG_MODE_EN			BIT(0)
#define SRAM_EXT_LD_DONE		BIT(23)

#define PCIE_PHY_STATUS1		0x10
#define SRAM_INIT_DONE			BIT(2)

#define PCIE_CTL_CFG1			0x14
#define LTSSM_ENABLE			BIT(0)

#define PCIE_CTL_STATUS1		0x18
#define SMLH_LTSSM_STATE		GENMASK(5, 0)
#define PORT_LOGIC_LTSSM_STATE_L1_IDLE	0x14
#define SMLH_LINKUP			BIT(6)
#define RDLH_LINKUP			BIT(7)
#define LINKUP				(SMLH_LINKUP | RDLH_LINKUP)

#define PCIE_AXI_QOS			0x24
#define PCIE_AW_USER_VC			GENMASK(2, 0)
#define PCIE_AR_USER_VC			GENMASK(5, 3)

#define PIPE_COMMON_STATUS		0x70
#define TXCOMMONMODE_DISABLE		BIT(30)

/* DWC Port Logic offsets */
/* bit-fields of PCIE_PORT_MULTI_LANE_CTRL */
#define PORT_TARGET_LINK_WIDTH_MASK		GENMASK(5, 0)
#define PORT_DIRECT_LINK_WIDTH_CHANGE		BIT(6)

#define AMBA_LINK_TIMEOUT_OFF		0x8D4
#define LINK_TIMEOUT_PERIOD		GENMASK(7, 0)

#define S5400_VENDOR_DEVICE_ID		0xA5A5144D
#define S5400_MSI_CAP_OFFSET		0x50

static LIST_HEAD(gpcie_inst_list);

static u32 google_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				 u32 reg, size_t size)
{
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	if (!gpcie->powered_on) {
		spin_unlock_irqrestore(&gpcie->power_on_lock, flags);
		WARN_ONCE(1, "%s%s Invalid attempt to read DBI while powered down",
			  dev_bus_name(gpcie->dev), dev_name(gpcie->dev));
		return U32_MAX;
	}
	dw_pcie_read(base + reg, size, &val);
	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);

	return val;
}

static int google_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = google_pcie_read_dbi(pci, pci->dbi_base, where, size);

	return PCIBIOS_SUCCESSFUL;
}

static void google_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				  u32 reg, size_t size, u32 val)
{
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	unsigned long flags;

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	if (!gpcie->powered_on) {
		spin_unlock_irqrestore(&gpcie->power_on_lock, flags);
		WARN_ONCE(1, "%s%s Invalid attempt to write DBI while powered down",
		     dev_bus_name(gpcie->dev), dev_name(gpcie->dev));
		return;
	}
	dw_pcie_write(base + reg, size, val);
	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);
}

static int google_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	google_pcie_write_dbi(pci, pci->dbi_base, where, size, val);

	return PCIBIOS_SUCCESSFUL;
}

/* Dedicated ops for accessing RC's config space */
static struct pci_ops google_pcie_ops = {
	.read = google_pcie_rd_own_conf,
	.write = google_pcie_wr_own_conf,
};

static void __iomem *google_pcie_other_conf_map_bus(struct pci_bus *bus,
						    unsigned int devfn,
						    int where)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	struct dw_pcie_rp *pp = bus->sysdata;
#else
	struct pcie_port *pp = bus->sysdata;
#endif
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	u64 limit_addr;
	u32 retries;
	u32 busdev;
	u32 offset;
	u32 val;
	int type;

	busdev = PCIE_ATU_BUS(bus->number) | PCIE_ATU_DEV(PCI_SLOT(devfn)) |
		 PCIE_ATU_FUNC(PCI_FUNC(devfn));

	if (gpcie->curr_mapped_busdev == busdev)
		goto atu_enabled;

	if (pci_is_root_bus(bus->parent))
		type = PCIE_ATU_TYPE_CFG0;
	else
		type = PCIE_ATU_TYPE_CFG1;

	/* Setup 0th outbound ATU region for config transactions */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	offset = PCIE_ATU_UNROLL_BASE(PCIE_ATU_REGION_DIR_OB, 0);
#else
	offset = PCIE_GET_ATU_OUTB_UNR_REG_OFFSET(0);
#endif
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_BASE, 4,
		      lower_32_bits(pp->cfg0_base));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_UPPER_BASE, 4,
		      upper_32_bits(pp->cfg0_base));
	limit_addr = pp->cfg0_base + pp->cfg0_size - 1;
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_LIMIT, 4,
		      lower_32_bits(limit_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_UPPER_LIMIT, 4,
		      upper_32_bits(limit_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_TARGET, 4,
		      lower_32_bits(busdev));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_UPPER_TARGET, 4,
		      upper_32_bits(busdev));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL1, 4,
		      type);
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL2, 4,
		      PCIE_ATU_ENABLE);

	/* Make sure ATU enable takes effect before any subsequent config and I/O accesses */
	for (retries = 0; retries < LINK_WAIT_MAX_IATU_RETRIES; retries++) {
		dw_pcie_read(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL2,
			     4, &val);
		if (val & PCIE_ATU_ENABLE) {
			gpcie->curr_mapped_busdev = busdev;
			goto atu_enabled;
		}

		mdelay(LINK_WAIT_IATU);
	}

	dev_err(pci->dev, "Outbound iATU is not being enabled\n");

	return NULL;

atu_enabled:
	return pp->va_cfg0_base + where;
}

static int google_pcie_rd_other_conf(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 *val)
{
	int ret;
	u32 vendor_device;
	struct dw_pcie_rp *pp = bus->sysdata;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	unsigned long flags;

	spin_lock_irqsave(&gpcie->link_up_lock, flags);
	if (!gpcie->is_link_up) {
		spin_unlock_irqrestore(&gpcie->link_up_lock, flags);
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	ret = pci_generic_config_read(bus, devfn, where, size, val);

	if (where == PCI_CLASS_REVISION && (*val >> 8) == 0x0) {

		/* S5400 workaround: Fake class code as network device */
		if ((pci_generic_config_read(bus, devfn, PCI_VENDOR_ID, 4,
					     &vendor_device) == 0) &&
		    (vendor_device == S5400_VENDOR_DEVICE_ID)) {
			*val |= (PCI_CLASS_NETWORK_OTHER << 16);
			dev_info(pci->dev,
				 "Fake PCI class as a Network device (0x%x)\n",
				 *val);
		}
	}

	if ((where == S5400_MSI_CAP_OFFSET + PCI_MSI_FLAGS) &&
	    (*val & PCI_MSI_FLAGS_QMASK) == 0x0) {
		/* S5400 workaround: Fake MSI multiple message as capable
		*                   of 16 vectors (0x4) */
		if ((pci_generic_config_read(bus, devfn, PCI_VENDOR_ID, 4,
					     &vendor_device) == 0) &&
		    (vendor_device == S5400_VENDOR_DEVICE_ID)) {
			*val |= (0x4 << 1);
			dev_info(pci->dev,
				 "Fake MSI capability as: 0x%x\n", *val);
		}
	}
	spin_unlock_irqrestore(&gpcie->link_up_lock, flags);

	return ret;
}

static int google_pcie_wr_other_conf(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 val)
{
	int ret;
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	unsigned long flags;

	spin_lock_irqsave(&gpcie->link_up_lock, flags);
	if (!gpcie->is_link_up) {
		spin_unlock_irqrestore(&gpcie->link_up_lock, flags);
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	ret = pci_generic_config_write(bus, devfn, where, size, val);
	spin_unlock_irqrestore(&gpcie->link_up_lock, flags);

	return ret;
}

/* Dedicated ops for accessing EP's config space */
static struct pci_ops google_child_pcie_ops = {
	.map_bus = google_pcie_other_conf_map_bus,
	.read = google_pcie_rd_other_conf,
	.write = google_pcie_wr_other_conf,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static int google_pcie_host_init(struct dw_pcie_rp *pp)
#else
static int google_pcie_host_init(struct pcie_port *pp)
#endif
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	u32 val;

	pp->bridge->ops = &google_pcie_ops;
	pp->bridge->child_ops = &google_child_pcie_ops;

	if (gpcie->link_timeout_ms) {
		val = readl(pci->dbi_base + AMBA_LINK_TIMEOUT_OFF);
		val &= ~LINK_TIMEOUT_PERIOD;
		val |= gpcie->link_timeout_ms;
		writel(val, pci->dbi_base + AMBA_LINK_TIMEOUT_OFF);
		dev_info(gpcie->dev, "Link request queue flush timeout set to %u ms\n",
			 gpcie->link_timeout_ms);
	}

	return 0;
}

static const struct dw_pcie_host_ops google_pcie_host_ops = {
	.host_init = google_pcie_host_init,
};

static int google_pcie_link_up(struct dw_pcie *pci)
{
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	unsigned long flags;
	u32 ctl_status;
	u32 ltssm_state;
	u32 debug_info;

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	if (!gpcie->powered_on) {
		spin_unlock_irqrestore(&gpcie->power_on_lock, flags);
		return 0;
	}

	ctl_status = readl(gpcie->top_base + PCIE_CTL_STATUS1);
	ltssm_state = ctl_status & SMLH_LTSSM_STATE;
	debug_info = readl(pci->dbi_base + PCIE_PORT_DEBUG1);
	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);

	return (((ctl_status & LINKUP) == LINKUP) &&
		(ltssm_state >= PORT_LOGIC_LTSSM_STATE_L0) &&
		(ltssm_state <= PORT_LOGIC_LTSSM_STATE_L1_IDLE) &&
		(!(debug_info & PCIE_PORT_DEBUG1_LINK_IN_TRAINING)));
}

static void google_pcie_assert_perst_n(struct google_pcie *gpcie, bool val)
{
	dev_dbg(gpcie->dev, "%s PERST reset\n", val ? "asserting" : "deasserting");
	gpiod_set_value(gpcie->perstn_gpio, val);
}

/* Wait for a max of 48ms for link up, then timeout */
static int google_pcie_wait_for_link(struct dw_pcie *pci)
{
	u32 i;

	for (i = 0; i < LINK_UP_MAX_RETRIES; i++) {
		if (dw_pcie_link_up(pci)) {
			dev_info(pci->dev, "Link up\n");
			return 0;
		}
		usleep_range(LINK_UP_WAIT_TIME_US, LINK_UP_WAIT_TIME_US + 2);
	}

	dev_info(pci->dev, "Phy link never came up\n");
	return -ETIMEDOUT;
}

static void google_pcie_set_link_width_speed(struct google_pcie *gpcie)
{
	struct dw_pcie *pci = gpcie->pci;

	pci->link_gen = gpcie->target_link_speed;
	pci->num_lanes = gpcie->target_link_width;

	dev_dbg(gpcie->dev, "Setting the link speed: %d width: %d",
		gpcie->target_link_speed, gpcie->target_link_width);
}

static void google_pcie_disable_equalization(struct google_pcie *gpcie)
{
	u32 val;

	/* Bit field name implies GEN3, but applies to GEN3 & GEN4 */
	val = readl(gpcie->pci->dbi_base + GEN3_RELATED_OFF);
	val |= GEN3_RELATED_OFF_GEN3_EQ_DISABLE;
	writel(val, gpcie->pci->dbi_base + GEN3_RELATED_OFF);
	dev_dbg(gpcie->dev, "Disabled GEN3/GEN4 Link equalization\n");
}

static void google_pcie_stop_link(struct google_pcie *gpcie)
{
	u32 val;

	dev_dbg(gpcie->dev, "disabling LTSSM state\n");
	val = readl(gpcie->top_base + PCIE_CTL_CFG1);
	val &= ~LTSSM_ENABLE;
	writel(val, gpcie->top_base + PCIE_CTL_CFG1);
	google_pcie_assert_perst_n(gpcie, true);
}

static int google_pcie_start_link(struct dw_pcie *pci)
{
	struct google_pcie *gpcie = dev_get_drvdata(pci->dev);
	struct pci_dev *pci_dev = NULL;
	u16 cur_speed, max_speed;
	u8 exp_cap_offset;
	u32 val, ret;
	u16 cur_width;

	/* reset PHY */
	google_pcie_assert_perst_n(gpcie, false);
	usleep_range(gpcie->perst_delay_us, gpcie->perst_delay_us + 2000);

	dev_dbg(pci->dev, "enable app_ltssm_en for link training\n");
	val = readl(gpcie->top_base + PCIE_CTL_CFG1);
	val |= LTSSM_ENABLE;
	writel(val, gpcie->top_base + PCIE_CTL_CFG1);

	ret = google_pcie_wait_for_link(pci);
	if (ret)
		goto link_fail;

	pci_dev = pci_get_slot(gpcie->pci->pp.bridge->bus, 0);
	if (!pci_dev) {
		dev_err(gpcie->dev, "Could not find the pci_dev");
		ret = -ENODEV;
		goto link_fail;
	}

	exp_cap_offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	val = dw_pcie_read_dbi(pci, exp_cap_offset + PCI_EXP_LNKSTA, 2);
	cur_speed = val & PCI_EXP_LNKSTA_CLS;
	cur_width = (val & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;

	val = pcie_get_speed_cap(pci_dev);
	max_speed = (u16) (val - PCIE_SPEED_2_5GT + 1);

	pci_dev_put(pci_dev);

	if (cur_speed < max_speed) {
		dev_info(gpcie->dev,
			 "%s: Link speed (GEN-%d) is less than target speed (GEN-%d)",
			__func__, cur_speed, max_speed);
		ret = -EAGAIN;
	} else {
		dev_dbg(gpcie->dev, "Link started successfully. Link speed = Gen%d, Target = Gen%d\n",
			cur_speed, max_speed);
	}

	gpcie->current_link_speed = cur_speed;
	gpcie->current_link_width = cur_width;

link_fail:
	return ret;
}

static void google_pcie_send_pme_turn_off(struct google_pcie *gpcie)
{
	u32 val, count;

	/* Toggle SII signal to send PME_Turn_Off  */
	val = readl(gpcie->sii_base + PM_UNLOCK_ERR_MSG);
	val |= APPS_PM_XMT_TURNOFF;
	writel(val, gpcie->sii_base + PM_UNLOCK_ERR_MSG);
	val &= ~APPS_PM_XMT_TURNOFF;
	writel(val, gpcie->sii_base + PM_UNLOCK_ERR_MSG);

	for (count = 0; count < MAX_L2_COUNT; count++) {
		usleep_range(L2_PME_TO_ACK_WAIT_US, L2_PME_TO_ACK_WAIT_US + 1);
		val = readl(gpcie->sii_base + PM_UNLOCK_ERR_MSG);

		if (val & RADM_PM_TO_ACK) {
			val = RADM_PM_TO_ACK;
			writel(val, gpcie->sii_base + PM_UNLOCK_ERR_MSG);
			dev_dbg(gpcie->dev, "Received PME_TO_ACK\n");
			return;
		}
	}

	dev_err(gpcie->dev, "Timed out waiting for PME_TO_ACK\n");

	return;
}

static const struct dw_pcie_ops google_dw_pcie_ops = {
	.link_up = google_pcie_link_up,
	.read_dbi = google_pcie_read_dbi,
	.write_dbi = google_pcie_write_dbi,
};

static struct google_pcie *google_pcie_get_handle(int num)
{
	struct google_pcie *gpcie = NULL;

	list_for_each_entry(gpcie, &gpcie_inst_list, node) {
		if (gpcie->domain == num)
			return gpcie;
	}

	pr_err("%s: Cannot find PCIe controller for %d", __func__, num);
	return NULL;
}

static u64 power_stats_get_ts(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static void power_stats_update_up(struct google_pcie *gpcie)
{
	u64 current_ts;
	unsigned long flags;

	spin_lock_irqsave(&gpcie->power_stats_lock, flags);

	current_ts = power_stats_get_ts();

	gpcie->link_up.count++;
	gpcie->link_up.last_entry_ms = current_ts;
	gpcie->link_down.duration += current_ts - gpcie->link_down.last_entry_ms;

	spin_unlock_irqrestore(&gpcie->power_stats_lock, flags);
}

static void power_stats_update_down(struct google_pcie *gpcie)
{
	u64 current_ts;
	unsigned long flags;

	spin_lock_irqsave(&gpcie->power_stats_lock, flags);

	current_ts = power_stats_get_ts();

	gpcie->link_down.count++;
	gpcie->link_down.last_entry_ms = current_ts;
	gpcie->link_up.duration += current_ts - gpcie->link_up.last_entry_ms;

	spin_unlock_irqrestore(&gpcie->power_stats_lock, flags);
}

static int google_pcie_set_regulators(struct google_pcie *gpcie, bool enable)
{
	int ret = 0;

	if (enable) {
		ret = regulator_enable(gpcie->vdd0p75);
		if (ret) {
			dev_err(gpcie->dev, "Failed to enable PHY digital supply, err: %d\n", ret);
			return ret;
		}

		ret = regulator_enable(gpcie->vdd1p2);
		if (ret) {
			dev_err(gpcie->dev, "Failed to enable PHY I/O supply, err: %d\n", ret);
			regulator_disable(gpcie->vdd0p75);
			return ret;
		}
	} else {
		regulator_disable(gpcie->vdd1p2);
		regulator_disable(gpcie->vdd0p75);
	}

	return ret;
}

/**
 * google_pcie_rc_poweron - Power on the RC and bringup the link with EP
 *
 * This will setup the RC, run the link_up sequence with a few retries and
 * wait for link to be established. When the link is established successfully
 * first time, it also triggers the bus scanning for the bus starting with
 * the RC controller.
 *
 * @num: The domain number representing the PCIe controller in the system
 * returns 0 on success or negative error codes
 */
int google_pcie_rc_poweron(int num)
{
	struct google_pcie *gpcie = NULL;
	struct dw_pcie *pci = NULL;
	struct pci_bus *bus = NULL;
	struct pci_dev *pci_dev = NULL;
	int ret, retry = 1;
	unsigned long flags;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	mutex_lock(&gpcie->link_lock);
	if (gpcie->is_link_up) {
		dev_err(gpcie->dev, "Link is up, quit poweron\n");
		mutex_unlock(&gpcie->link_lock);
		return -EINVAL;
	}

	pci = gpcie->pci;
	bus = pci->pp.bridge->bus;
	pci_dev = pci_get_slot(bus, 0);
	if (!pci_dev) {
		dev_err(gpcie->dev, "Could not find the pci_dev");
		mutex_unlock(&gpcie->link_lock);
		return -ENODEV;
	}

	dev_dbg(gpcie->dev, "running poweron sequence...\n");
	pm_runtime_get_sync(gpcie->dev);

	dev_dbg(gpcie->dev, "Restoring PCIe root port state");
	ret = pci_load_saved_state(pci_dev, gpcie->saved_state);
	if (ret) {
		dev_err(gpcie->dev, "Could not restore PCIe state\n");
		goto pm_put;
	}

	pci_restore_state(pci_dev);

	google_pcie_set_link_width_speed(gpcie);
	/* Restore IP's DBI registers - reinitialize instead of restoring */
	dw_pcie_setup_rc(&pci->pp);

	if (gpcie->skip_link_eq)
		google_pcie_disable_equalization(gpcie);

	/*
	 * Program the ATU on poweron - either by map_bus on first poweron(),
	 * or manually at the end on subsequent poweron().
	 */
	gpcie->curr_mapped_busdev = 0;

	while (!gpcie->is_link_up) {
		ret = google_pcie_start_link(pci);
		if (!ret) {
			gpcie->is_link_up = true;
			break;
		}

		if (retry++ < LINK_WAIT_MAX_RETRIES) {
			google_pcie_stop_link(gpcie);
			usleep_range(gpcie->perst_delay_us, gpcie->perst_delay_us + 2000);
			continue;
		}

		if (ret == -EAGAIN) {
			gpcie->is_link_up = true;
			dev_info(gpcie->dev, "Link up at reduced rate\n");
			ret = 0;
		} else {
			dev_err(gpcie->dev, "Failed to start link, giving up retries\n");
			ret = -EPIPE;
			goto link_down;
		}
	}

	/* At this point gpcie->is_link_up flag is true. Enable link down interrupt. */
	enable_irq(gpcie->link_down_irq);

	if (gpcie->pcie_flags & GPCIE_FLAG_PHY_FW_CLK) {
		ret = goog_cpm_clk_toggle_auto_gate(gpcie->phy_fw_clk, true);
		if (ret) {
			dev_err(gpcie->dev,
				"Failed to enable ACG for phy_fw clk\n");
			goto link_down;
		}
	}

	if (gpcie->pcie_flags & GPCIE_FLAG_REFCLK) {
		ret = goog_cpm_clk_toggle_auto_gate(gpcie->ref_clk, true);
		if (ret) {
			dev_err(gpcie->dev, "Failed to enable ACG for ref clk");
			goto disable_phy_fw_acg;
		}
	}

	power_stats_update_up(gpcie);
	if (!gpcie->enumeration_done) {
		dev_dbg(gpcie->dev, "Rescanning bus for enumeration\n");
		pci_rescan_bus(bus);
		gpcie->enumeration_done = true;
	}

	if (!gpcie->curr_mapped_busdev)
		google_pcie_other_conf_map_bus(pci_find_bus(num, 1), PCI_DEVFN(0, 0), 0);

	dev_dbg(gpcie->dev, "poweron success\n");
	pci_dev_put(pci_dev);
	mutex_unlock(&gpcie->link_lock);

	return 0;

disable_phy_fw_acg:
	if (gpcie->pcie_flags & GPCIE_FLAG_PHY_FW_CLK)
		goog_cpm_clk_toggle_auto_gate(gpcie->phy_fw_clk, false);
link_down:
	spin_lock_irqsave(&gpcie->link_up_lock, flags);
	gpcie->is_link_up = false;
	spin_unlock_irqrestore(&gpcie->link_up_lock, flags);
	gpcie->current_link_speed = 0;
	gpcie->current_link_width = 0;
	disable_irq(gpcie->link_down_irq);
	flush_delayed_work(&gpcie->link_down_work);
	google_pcie_send_pme_turn_off(gpcie);
	google_pcie_stop_link(gpcie);
pm_put:
	pm_runtime_put_sync(gpcie->dev);
	pci_dev_put(pci_dev);
	mutex_unlock(&gpcie->link_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(google_pcie_rc_poweron);

/**
 * google_pcie_rc_poweroff - Disable link between EP and power OFF the RC
 *
 * This initiates the PME handshake to transition the PCIe link between EP
 * to power saving state L2 and turns off the PCIe RC controller.
 *
 * @num: The domain number representing the PCIe controller in the system
 * returns 0 on success or negative error codes
 */
int google_pcie_rc_poweroff(int num)
{
	struct google_pcie *gpcie = NULL;
	struct dw_pcie *pci = NULL;
	struct pci_bus *bus = NULL;
	struct pci_dev *pci_dev = NULL;
	unsigned long flags;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	mutex_lock(&gpcie->link_lock);
	if (!gpcie->is_link_up) {
		dev_err(gpcie->dev, "Link is down, quit poweroff\n");
		mutex_unlock(&gpcie->link_lock);
		return -EINVAL;
	}

	pci = gpcie->pci;
	bus = pci->pp.bridge->bus;
	pci_dev = pci_get_slot(bus, 0);
	if (!pci_dev) {
		dev_err(gpcie->dev, "Could not find the pci_dev");
		mutex_unlock(&gpcie->link_lock);
		return -ENODEV;
	}

	dev_dbg(gpcie->dev, "Saving PCI state");
	pci_save_state(pci_dev);
	kfree(gpcie->saved_state);
	gpcie->saved_state = pci_store_saved_state(pci_dev);

	if (gpcie->pcie_flags & GPCIE_FLAG_REFCLK)
		goog_cpm_clk_toggle_auto_gate(gpcie->ref_clk, false);

	if (gpcie->pcie_flags & GPCIE_FLAG_PHY_FW_CLK)
		goog_cpm_clk_toggle_auto_gate(gpcie->phy_fw_clk, false);

	spin_lock_irqsave(&gpcie->link_up_lock, flags);
	gpcie->is_link_up = false;
	spin_unlock_irqrestore(&gpcie->link_up_lock, flags);
	gpcie->current_link_speed = 0;
	gpcie->current_link_width = 0;
	disable_irq(gpcie->link_down_irq);
	flush_delayed_work(&gpcie->link_down_work);
	google_pcie_send_pme_turn_off(gpcie);
	google_pcie_stop_link(gpcie);
	power_stats_update_down(gpcie);
	pm_runtime_put_sync(gpcie->dev);
	dev_dbg(gpcie->dev, "poweroff success\n");

	pci_dev_put(pci_dev);
	mutex_unlock(&gpcie->link_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(google_pcie_rc_poweroff);

static u32 google_pcie_get_link_state(struct google_pcie *gpcie)
{
	u32 pm_state = UNKNOWN;
	u32 pipe_status;
	unsigned long flags;

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	if (!gpcie->powered_on) {
		spin_unlock_irqrestore(&gpcie->power_on_lock, flags);
		pm_state = L2;
		goto exit;
	}

	pm_state = readl(gpcie->sii_base + PM_STATE);
	pipe_status = readl(gpcie->top_base + PIPE_COMMON_STATUS);

	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);

	if (LTSSM_STATE(pm_state) == S_L0) {
		pm_state = L0;
	} else if (pm_state & LINK_IN_L0S) {
		pm_state = L0S;
	} else if (pm_state & LINK_IN_L1SUB) {
		if (pipe_status & TXCOMMONMODE_DISABLE)
			pm_state = L12;
		else
			pm_state = L11;
	} else if (pm_state & LINK_IN_L1) {
		pm_state = L1;
	} else if (pm_state & LINK_IN_L2) {
		pm_state = L2;
	} else {
		pm_state = UNKNOWN;
	}

exit:
	return pm_state;
}

static bool google_pcie_is_link_in_l0(struct google_pcie *gpcie)
{
	u32 link_state = UNKNOWN;

	link_state = google_pcie_get_link_state(gpcie);
	return (link_state == L0);
}

/**
 * google_pcie_get_max_link_speed - Get max link speed
 *
 * This will get the max speed that link is capable of for the specified PCIe
 * controller.  The max speed is the lower of the RC max speed capability and
 * the max-speed configured in the device tree.
 *
 * @num: The domain number representing the PCIe controller in the system.
 * returns maximum link speed on success or negative error codes.
 */
int google_pcie_get_max_link_speed(int num)
{
	struct google_pcie *gpcie = google_pcie_get_handle(num);

	if (!gpcie)
		return -EINVAL;

	return gpcie->max_link_speed;
}
EXPORT_SYMBOL_GPL(google_pcie_get_max_link_speed);

/**
 * google_pcie_get_max_link_width - Get max link width
 *
 * This will get the max width that link is capable of for the specified PCIe
 * controller.  The max width is the lower of the RC max width capability and
 * the max-width configured in the device tree.
 *
 * @num: The domain number representing the PCIe controller in the system.
 * returns maximum link width on success or negative error codes.
 */
int google_pcie_get_max_link_width(int num)
{
	struct google_pcie *gpcie = google_pcie_get_handle(num);

	if (!gpcie)
		return -EINVAL;

	return gpcie->max_link_width;
}
EXPORT_SYMBOL_GPL(google_pcie_get_max_link_width);

/**
 * google_pcie_rc_change_link_speed - Change pcie link speed.
 *
 * This will set the link speed to desired values when link is up in L0.
 * If link is not up, the desired link speed change will be in effect from next link up.
 * This function must not be called when Link is up but not in L0 state.
 *
 * This function will fail if requested speed is more than the speed at link up.
 * For ex. if link up was done in Gen3x1, then speed change to Gen4 is prevented
 * by hardware and failure is returned. In this case, gen3->gen2->gen1->gen3 etc.
 * transitions by this function call will be successful.
 *
 * @num: The domain number representing the PCIe controller in the system.
 * @speed: The desired speed to be set for PCIe link.
 * returns 0 on success or negative error codes.
 */
int google_pcie_rc_change_link_speed(int num, unsigned int speed)
{
	struct google_pcie *gpcie = NULL;
	struct dw_pcie *pci = NULL;
	int i = 0;
	u32 val = 0;
	u16 linkstat = 0;
	u32 old_speed = 0;
	u32 new_speed = 0;
	u8 exp_cap_offset = 0;
	int ret = 0;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	if (speed < 1 || speed > gpcie->max_link_speed) {
		dev_err(gpcie->dev, "Invalid speed %d. Valid range: [1-%d].\n",
			speed, gpcie->max_link_speed);
		return -EINVAL;
	}

	if (speed == gpcie->current_link_speed)
		return 0;

	mutex_lock(&gpcie->link_lock);
	old_speed = gpcie->target_link_speed;
	gpcie->target_link_speed = speed;
	if (!gpcie->is_link_up) {
		dev_info(gpcie->dev, "%s: Link speed changed to %d from next link up\n",
			 __func__, speed);
		ret = 0;
		goto unlock;
	}

	if (!google_pcie_is_link_in_l0(gpcie)) {
		dev_err(gpcie->dev, "%s: Link is not in L0.\n", __func__);
		gpcie->target_link_speed = old_speed;
		ret = -EINVAL;
		goto unlock;
	}

	pci = gpcie->pci;

	/* 1. modify link speed: LINK_CONTROL2_LINK_STATUS2_REG -> PCIE_CAP_TARGET_LINK_SPEED */
	exp_cap_offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	val = dw_pcie_read_dbi(pci, exp_cap_offset + PCI_EXP_LNKCTL2, 2);
	val &= ~PCI_EXP_LNKCTL2_TLS;
	val |= speed;
	dw_pcie_write_dbi(pci, exp_cap_offset + PCI_EXP_LNKCTL2, 2, val);

	/* 2. Deassert:  GEN2_CTRL_OFF -> DIRECT_SPEED_CHANGE */
	val = dw_pcie_read_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, 4);
	val &= ~PORT_LOGIC_SPEED_CHANGE;
	dw_pcie_write_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, 4, val);

	/* 3. Assert:  GEN2_CTRL_OFF -> DIRECT_SPEED_CHANGE */
	val = dw_pcie_read_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, 4);
	val |= PORT_LOGIC_SPEED_CHANGE;
	dw_pcie_write_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, 4, val);

	/* Check the target link speed against current link speed */
	for (i = 0; i < MAX_TIMEOUT_WIDTH_SPEED_CHANGE; i++) {
		linkstat = dw_pcie_read_dbi(pci, exp_cap_offset + PCI_EXP_LNKSTA, 2);
		new_speed = linkstat & PCI_EXP_LNKSTA_CLS;

		if (new_speed == speed)
			break;

		udelay(WIDTH_SPEED_CHANGE_WAIT_US);
	}

	if (new_speed != speed) {
		/*
		 * Change didn't happen. Most probable reason of failure:
		 * requested value is more than the value at link up
		 */
		dev_err(gpcie->dev, "%s: Fail: target speed: %d current speed: %d\n",
			__func__, speed, gpcie->current_link_speed);
		gpcie->target_link_speed = old_speed;
		ret = -EINVAL;
		goto unlock;
	}

	gpcie->current_link_speed = speed;
	mutex_unlock(&gpcie->link_lock);
	dev_dbg(gpcie->dev, "%s: link speed changed to %d\n", __func__, speed);

	return 0;
unlock:
	mutex_unlock(&gpcie->link_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(google_pcie_rc_change_link_speed);

/**
 * google_pcie_rc_change_link_width - Change pcie link width.
 *
 * This will set the link width to desired values when link is up in L0.
 * If link is not up, the desired link width change will be in effect from next link up.
 * This function must not be called when Link is up but not in L0 state.
 *
 * This function will fail if requested width is more than the width at link up.
 * For ex. if link up was done in Gen4x1, then width change to Gen4x2 is prevented
 * by hardware and failure is returned.
 *
 * @num: The domain number representing the PCIe controller in the system
 * @width: The desired width to be set for PCIe link
 * returns 0 on success or negative error codes
 */
int google_pcie_rc_change_link_width(int num, unsigned int width)
{
	struct google_pcie *gpcie = NULL;
	struct dw_pcie *pci = NULL;
	int i = 0;
	u32 val = 0;
	int ret = 0;
	u16 linkstat = 0;
	u32 old_width = 0;
	u32 new_width = 0;
	u8 exp_cap_offset = 0;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	if (width < 1 || width > gpcie->max_link_width) {
		dev_err(gpcie->dev, "Invalid width %d. Valid range: [1-%d].\n",
			width, gpcie->max_link_width);
		return -EINVAL;
	}

	if (width == gpcie->current_link_width)
		return 0;

	mutex_lock(&gpcie->link_lock);
	old_width = gpcie->target_link_width;
	gpcie->target_link_width = width;
	if (!gpcie->is_link_up) {
		dev_info(gpcie->dev, "%s: Link width changed to %d from next link up\n",
			 __func__, width);
		ret = 0;
		goto unlock;
	}

	if (!google_pcie_is_link_in_l0(gpcie)) {
		dev_err(gpcie->dev, "%s: Link is not in L0.\n", __func__);
		ret = -EINVAL;
		gpcie->target_link_width = old_width;
		goto unlock;
	}

	pci = gpcie->pci;

	exp_cap_offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);

	/* 1. Program the PCIE_PORT_MULTI_LANE_CTRL -> TARGET_LINK_WIDTH */
	val = dw_pcie_read_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4);
	val &= ~PORT_TARGET_LINK_WIDTH_MASK;
	val |= width;
	dw_pcie_write_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4, val);

	/* 2. Deassert the PCIE_PORT_MULTI_LANE_CTRL -> DIRECT_LINK_WIDTH_CHANGE */
	val = dw_pcie_read_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4);
	val &= ~PORT_DIRECT_LINK_WIDTH_CHANGE;
	dw_pcie_write_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4, val);

	/* 3. Assert the PCIE_PORT_MULTI_LANE_CTRL -> DIRECT_LINK_WIDTH_CHANGE */
	val = dw_pcie_read_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4);
	val |= PORT_DIRECT_LINK_WIDTH_CHANGE;
	dw_pcie_write_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, 4, val);

	/* Check the negotiated link width */
	for (i = 0; i < MAX_TIMEOUT_WIDTH_SPEED_CHANGE; i++) {
		linkstat = dw_pcie_read_dbi(pci, exp_cap_offset + PCI_EXP_LNKSTA, 2);
		new_width = (linkstat & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;

		if (new_width == width)
			break;

		udelay(WIDTH_SPEED_CHANGE_WAIT_US);
	}

	if (new_width != width) {
		/*
		 * Change didn't happen. Most probable reason of failure:
		 * requested value is more than the value at link up
		 */
		dev_err(gpcie->dev, "%s: Fail: target width: %d current width: %d\n",
			__func__, width, gpcie->current_link_width);
		gpcie->target_link_width = old_width;
		ret = -EINVAL;
		goto unlock;
	}

	gpcie->current_link_width = width;
	mutex_unlock(&gpcie->link_lock);
	dev_dbg(gpcie->dev, "%s: link width changed to %d\n", __func__, width);

	return 0;
unlock:
	mutex_unlock(&gpcie->link_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(google_pcie_rc_change_link_width);

/**
 * google_pcie_poweron_withspeed - Power on RC with certain speed
 *
 * This will set the target link speed as specified in @speed argument,
 * and then tries to bringup PCIe link with that speed.
 *
 * @num: The domain number representing the PCIe controller in the system
 * @speed: The link speed to be used for link-up
 * returns 0 on success or negative error codes
 */
int google_pcie_poweron_withspeed(int num, unsigned int speed)
{
	struct google_pcie *gpcie = NULL;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	if (speed < 1 || speed > gpcie->max_link_speed) {
		dev_err(gpcie->dev, "Invalid max_link_speed %d. Should be in range [1-%d]\n",
			speed, gpcie->max_link_speed);
		return -EINVAL;
	}

	gpcie->target_link_speed = speed;
	return google_pcie_rc_poweron(num);
}
EXPORT_SYMBOL_GPL(google_pcie_poweron_withspeed);

/**
 * google_pcie_register_callback - Register a callback where events will be sent
 *
 * This will register a callback function that will be invoked upon various
 * events like completion timeout, link_down, etc.
 *
 * @num: The domain number representing the PCIe controller in the system
 * @cb_func: Pointer to the callback function which should be registered
 * @priv: Private data that will be passed as-is when the callback function
 *    is invoked
 * returns 0 on success or negative error codes
 */
int google_pcie_register_callback(int num, google_pcie_callback_func cb_func,
				  void *priv)
{
	struct google_pcie *gpcie;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	gpcie->cb_func = cb_func;
	gpcie->cb_priv = priv;
	return 0;
}
EXPORT_SYMBOL_GPL(google_pcie_register_callback);

/**
 * google_pcie_unregister_callback - Disable callback registration
 *
 * This will unregister the callback function and stop invoking any previously
 * registered callback functions.
 *
 * @num: The domain number representing the PCIe controller in the system
 * returns 0 on success or negative error codes
 */
int google_pcie_unregister_callback(int num)
{
	struct google_pcie *gpcie;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	gpcie->cb_func =  NULL;
	gpcie->cb_priv = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(google_pcie_unregister_callback);

/**
 * google_pcie_link_status - Query link status by PCIe controller number
 *
 * Query link status for the given PCI controller
 *
 * @num: The domain number representing the PCIe controller in the system
 * returns 1 for link up; 0 for link down or negative for error
 */
int google_pcie_link_status(int num)
{
	struct google_pcie *gpcie;

	gpcie = google_pcie_get_handle(num);
	if ((!gpcie) || (!gpcie->pci)) {
		pr_err("Invalid PCI handle for link status on chan %d\n", num);
		return -EINVAL;
	}
	return (dw_pcie_link_up(gpcie->pci));
}
EXPORT_SYMBOL_GPL(google_pcie_link_status);

void google_pcie_cpl_timeout_work(struct work_struct *work)
{
	struct google_pcie *gpcie = container_of(work, struct google_pcie,
						 cpl_timeout_work.work);

	dev_dbg(gpcie->dev, "Starting cpl_timeout work\n");
	if (gpcie->cb_func) {
		dev_dbg(gpcie->dev, "Invoking registered callback\n");
		gpcie->cb_func(GPCIE_CB_CPL_TIMEOUT, gpcie->cb_priv);
	}
}

static irqreturn_t google_pcie_cpl_timeout_handler(int irq, void *data)
{
	struct google_pcie *gpcie = (struct google_pcie *)data;

	queue_delayed_work(gpcie->pcie_wq, &gpcie->cpl_timeout_work, 0);
	return IRQ_HANDLED;
}

void google_pcie_link_down_work(struct work_struct *work)
{
	struct google_pcie *gpcie = container_of(work, struct google_pcie, link_down_work.work);
	int ret = 0;
	u32 val = 0;
	unsigned long flags = 0;

	dev_dbg(gpcie->dev, "Starting link_down work\n");

	spin_lock_irqsave(&gpcie->link_up_lock, flags);
	if (!gpcie->is_link_up) {
		spin_unlock_irqrestore(&gpcie->link_up_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&gpcie->link_up_lock, flags);

	if (gpcie->cb_func) {
		dev_dbg(gpcie->dev, "Invoking link down callback\n");
		gpcie->cb_func(GPCIE_CB_LINK_DOWN, gpcie->cb_priv);
	}

	/* Wait till link is up again */
	ret = readl_poll_timeout(gpcie->sii_base + LINK_RST_STATUS, val, (val & SMLH_REQ_RST_NOT),
				 LINK_UP_WAIT_TIME_US, LINK_UP_WAIT_TIME_US * LINK_UP_MAX_RETRIES);

	if (!ret)
		enable_irq(gpcie->link_down_irq);
}

static irqreturn_t google_pcie_link_down_handler(int irq, void *data)
{
	struct google_pcie *gpcie = (struct google_pcie *)data;
	u32 val = 0;

	disable_irq_nosync(gpcie->link_down_irq);

	/* If it's residual interrupt, and link is up, return */
	val = readl(gpcie->sii_base + LINK_RST_STATUS);
	if (val & SMLH_REQ_RST_NOT) {
		enable_irq(gpcie->link_down_irq);
		return IRQ_HANDLED;
	}

	queue_delayed_work(gpcie->pcie_wq, &gpcie->link_down_work, 0);
	return IRQ_HANDLED;
}

static ssize_t link_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct google_pcie *gpcie = dev_get_drvdata(dev);
	u32 link_state = UNKNOWN;
	int ret;

	link_state = google_pcie_get_link_state(gpcie);
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", link_state);

	return ret;
}

static void power_stats_init(struct google_pcie *gpcie)
{
	gpcie->link_up.count = 0;
	gpcie->link_up.duration = 0;
	gpcie->link_up.last_entry_ms = 0;
	gpcie->link_down.count = 1;  // since system starts with link_down
	gpcie->link_down.duration = 0;
	gpcie->link_down.last_entry_ms = 0;
}

static ssize_t power_stats_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct google_pcie *gpcie = dev_get_drvdata(dev);
	struct power_stats link_up_copy;
	struct power_stats link_down_copy;
	unsigned long flags;
	u64 current_ts;
	u64 link_up_delta = 0;
	u64 link_down_delta = 0;
	int ret;

	spin_lock_irqsave(&gpcie->power_stats_lock, flags);
	memcpy(&link_up_copy, &gpcie->link_up, sizeof(struct power_stats));
	memcpy(&link_down_copy, &gpcie->link_down, sizeof(struct power_stats));
	spin_unlock_irqrestore(&gpcie->power_stats_lock, flags);

	current_ts = power_stats_get_ts();

	if (gpcie->is_link_up)
		link_up_delta = current_ts - link_up_copy.last_entry_ms;
	else
		link_down_delta = current_ts - link_down_copy.last_entry_ms;

	ret = scnprintf(buf, PAGE_SIZE, "Version: 1\n");

	ret += scnprintf(buf + ret, PAGE_SIZE - ret,
			 "Link up:\n  Cumulative count: 0x%llx\n"
			 "  Cumulative duration msec: 0x%llx\n"
			 "  Last entry timestamp msec: 0x%llx\n",
			 link_up_copy.count,
			 link_up_copy.duration + link_up_delta,
			 link_up_copy.last_entry_ms);

	ret += scnprintf(buf + ret, PAGE_SIZE - ret,
			 "Link down:\n  Cumulative count: 0x%llx\n"
			 "  Cumulative duration msec: 0x%llx\n"
			 "  Last entry timestamp msec: 0x%llx\n",
			 link_down_copy.count,
			 link_down_copy.duration + link_down_delta,
			 link_down_copy.last_entry_ms);

	return ret;
}

DEVICE_ATTR_RO(link_state);
DEVICE_ATTR_RO(power_stats);

static const char * const gpcie_init_rsts[] = {
	"top_csr", "button",
};

static void gpcie_reset_acg(void *data)
{
	struct google_pcie *gpcie = (struct google_pcie *)data;

	if (gpcie->pcie_flags & GPCIE_FLAG_REFCLK)
		goog_cpm_clk_toggle_auto_gate(gpcie->ref_clk, false);

	if (gpcie->pcie_flags & GPCIE_FLAG_PHY_FW_CLK)
		goog_cpm_clk_toggle_auto_gate(gpcie->phy_fw_clk, false);
}

static void gpcie_reset_control_assert(void *data)
{
	struct google_pcie *gpcie = (struct google_pcie *)data;

	if (gpcie->pcie_flags & GPCIE_FLAG_ONE_TIME_RESET)
		return;

	if (gpcie->pcie_flags & GPCIE_FLAG_BULK_RESET) {
		reset_control_bulk_assert(GPCIE_INIT_NUM_RSTS,
			gpcie->pcie_init_rsts);
		return;
	}

	reset_control_assert(gpcie->init_rst);
}

static void gpcie_regulator_disable(void *data)
{
	regulator_disable(data);
}

static void gpcie_mutex_destroy(void *data)
{
	mutex_destroy(data);
}

static void gpcie_list_del(void *data)
{
	list_del(data);
}

static int google_pcie_rst_init(struct google_pcie *gpcie)
{
	int ret;

	if (gpcie->pcie_flags & GPCIE_FLAG_BULK_RESET) {
		ret = reset_control_bulk_deassert(GPCIE_INIT_NUM_RSTS,
						  gpcie->pcie_init_rsts);
		if (ret)
			return ret;
	} else {
		ret = reset_control_deassert(gpcie->init_rst);
		if (ret)
			return ret;
	}

	return 0;
}

static void google_pcie_init_speed_width(struct google_pcie *gpcie, struct pci_dev *pci_dev)
{
	enum pci_bus_speed max_speed = PCI_SPEED_UNKNOWN;
	u8 supported_max_speed = 0;
	u8 supported_max_width = 0;

	/* populate max speed and width from min of capabilities and dt */
	max_speed = pcie_get_speed_cap(pci_dev);

	/* convert from pci_bus_speed enum to gen#  */
	supported_max_speed = (u8) (max_speed - PCIE_SPEED_2_5GT + 1);
	supported_max_width = (u8) pcie_get_width_cap(pci_dev);

	/*
	 * If "max-link-speed" DT property (link_gen) is set to some value,
	 * set to minimum of max supported by HW and DT property.
	 * If any invalid value is passed in DT, set it to max HW supported.
	 */
	gpcie->max_link_speed = (gpcie->pci->link_gen > 0) ?
				 min((u8) gpcie->pci->link_gen, supported_max_speed) :
				 supported_max_speed;

	/* Do the same for max width. DT property for max width is: "num-lanes" */
	gpcie->max_link_width = (gpcie->pci->num_lanes > 0) ?
				 min((u8) gpcie->pci->num_lanes, supported_max_width) :
				 supported_max_width;

	dev_dbg(gpcie->dev, "%s: max link speed:%d, max link width:%d\n",
		__func__, gpcie->max_link_speed, gpcie->max_link_width);

	/* Initialize target_link* as max values */
	gpcie->target_link_speed = gpcie->max_link_speed;
	gpcie->target_link_width = gpcie->max_link_width;
}

static int google_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct google_pcie *gpcie;
	struct dw_pcie *pci;
	struct pci_bus *bus = NULL;
	struct pci_dev *pci_dev = NULL;
	struct resource *phy_sram_res;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	struct dw_pcie_rp *pp;
#else
	struct pcie_port *pp;
#endif
	int ret;
	int i = 0;

	gpcie = devm_kzalloc(dev, sizeof(*gpcie), GFP_KERNEL);
	if (!gpcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	gpcie->pcie_flags = (u32)(uintptr_t)device_get_match_data(dev);
	gpcie->top_base = devm_platform_ioremap_resource_byname(pdev, "top");
	if (IS_ERR(gpcie->top_base))
		return PTR_ERR(gpcie->top_base);

	phy_sram_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy_sram");
	if (phy_sram_res) {
		gpcie->phy_sram_base = devm_ioremap_resource(dev, phy_sram_res);
		if (IS_ERR(gpcie->phy_sram_base))
			return PTR_ERR(gpcie->phy_sram_base);

		gpcie->phy_sram_size = resource_size(phy_sram_res);
	}

	gpcie->sii_base = devm_platform_ioremap_resource_byname(pdev, "sii");
	if (IS_ERR(gpcie->sii_base))
		return PTR_ERR(gpcie->sii_base);

	gpcie->pcie_wq = create_freezable_workqueue("pcie_wq");
	if (IS_ERR(gpcie->pcie_wq))
		return PTR_ERR(gpcie->pcie_wq);

	pci->dev = dev;
	pci->ops = &google_dw_pcie_ops;
	pci->pp.ops = &google_pcie_host_ops;
	gpcie->dev = dev;
	gpcie->pci = pci;
	gpcie->enumeration_done = false;

	INIT_DELAYED_WORK(&gpcie->cpl_timeout_work,
			  google_pcie_cpl_timeout_work);
	INIT_DELAYED_WORK(&gpcie->link_down_work,
			  google_pcie_link_down_work);

	if (of_property_read_u32(np, "google,perst-delay-us", &gpcie->perst_delay_us)) {
		gpcie->perst_delay_us = DEFAULT_PERST_DELAY_US;
		dev_info(dev, "PERST delay is NOT defined...default to %u ms\n",
			 gpcie->perst_delay_us / 1000);
	}

	platform_set_drvdata(pdev, gpcie);

	gpcie->perstn_gpio = devm_gpiod_get(dev, "perstn", GPIOD_OUT_HIGH);
	if (IS_ERR(gpcie->perstn_gpio)) {
		dev_err(dev, "Failed to request perstn GPIO\n");
		return -EINVAL;
	}

	gpcie->vdd0p75 = devm_regulator_get(dev, "vdd0p75");
	if (IS_ERR(gpcie->vdd0p75)) {
		ret = PTR_ERR(gpcie->vdd0p75);
		dev_err(dev, "Failed to get 0.75V PHY regulator, ret: %d\n", ret);
		return ret;
	}

	gpcie->vdd1p2 = devm_regulator_get(dev, "vdd1p2");
	if (IS_ERR(gpcie->vdd1p2)) {
		ret = PTR_ERR(gpcie->vdd1p2);
		dev_err(dev, "Failed to get 1.2V PHY regulator, ret: %d\n", ret);
		return ret;
	}

	scnprintf(gpcie->cpl_timeout_irqname, sizeof(gpcie->cpl_timeout_irqname),
		  "%s_cpl_timeout", dev_name(dev));
	gpcie->cpl_timeout_irq = platform_get_irq_byname(pdev, "cpl_timeout");
	ret = devm_request_irq(dev, gpcie->cpl_timeout_irq,
			       google_pcie_cpl_timeout_handler,
			       0, gpcie->cpl_timeout_irqname, gpcie);
	if (ret)
		dev_err(dev, "Failed to request cpl_timeout interrupt\n");

	scnprintf(gpcie->link_down_irqname, sizeof(gpcie->link_down_irqname),
		  "%s_link_down", dev_name(dev));
	gpcie->link_down_irq = platform_get_irq_byname(pdev, "link_down");
	ret = devm_request_irq(dev, gpcie->link_down_irq,
			       google_pcie_link_down_handler,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       gpcie->link_down_irqname, gpcie);
	if (ret)
		dev_err(dev, "Failed to request link_down interrupt\n");

	if (gpcie->pcie_flags & GPCIE_FLAG_PHY_FW_CLK) {
		gpcie->phy_fw_clk = devm_clk_get(dev, "phy_fw");
		if (IS_ERR(gpcie->phy_fw_clk)) {
			dev_err(dev, "Failed to get phy_fw clk\n");
			return PTR_ERR(gpcie->phy_fw_clk);
		}
	}

	if (gpcie->pcie_flags & GPCIE_FLAG_REFCLK) {
		gpcie->ref_clk = devm_clk_get(dev, "ref");
		if (IS_ERR(gpcie->ref_clk)) {
			dev_err(dev, "Failed to get ref clk\n");
			return PTR_ERR(gpcie->ref_clk);
		}
	}

	ret = devm_add_action_or_reset(dev, gpcie_reset_acg, gpcie);
	if (ret)
		return ret;

	if (gpcie->pcie_flags & GPCIE_FLAG_BULK_RESET) {
		for (i = 0; i < GPCIE_INIT_NUM_RSTS; i++)
			gpcie->pcie_init_rsts[i].id = gpcie_init_rsts[i];

		ret = devm_reset_control_bulk_get_exclusive(dev,
			GPCIE_INIT_NUM_RSTS, gpcie->pcie_init_rsts);
		if (ret) {
			dev_err(dev, "could not get init reset");
			return ret;
		}
	} else {
		gpcie->init_rst = devm_reset_control_get(dev, "init");
		if (IS_ERR(gpcie->init_rst))
			return PTR_ERR(gpcie->init_rst);
	}

	gpcie->pwr_up_rst = devm_reset_control_get(dev, "power_up");
	if (IS_ERR(gpcie->pwr_up_rst))
		return PTR_ERR(gpcie->pwr_up_rst);

	gpcie->perst_rst = devm_reset_control_get(dev, "perst");
	if (IS_ERR(gpcie->perst_rst))
		return PTR_ERR(gpcie->perst_rst);

	gpcie->app_hold_phy_rst = devm_reset_control_get(dev, "app_hold_phy");
	if (IS_ERR(gpcie->app_hold_phy_rst))
		return PTR_ERR(gpcie->app_hold_phy_rst);

	ret = devm_add_action_or_reset(dev, gpcie_reset_control_assert, gpcie);
	if (ret)
		return ret;

	gpcie->l1_pwrgate_disable = device_property_read_bool(dev, "google,l1-pwrgate-disable");

	gpcie->skip_link_eq = device_property_read_bool(dev, "google,skip-link-equalization");

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	ret = devm_add_action_or_reset(dev, gpcie_regulator_disable, gpcie->vdd0p75);
	if (ret)
		goto pm_disable;

	ret = devm_add_action_or_reset(dev, gpcie_regulator_disable, gpcie->vdd1p2);
	if (ret)
		goto pm_disable;

	of_property_read_u8(np, "google,link-timeout-ms", &gpcie->link_timeout_ms);

	spin_lock_init(&gpcie->power_on_lock);
	spin_lock_init(&gpcie->power_stats_lock);
	spin_lock_init(&gpcie->link_up_lock);
	mutex_init(&gpcie->link_lock);
	ret = devm_add_action_or_reset(dev, gpcie_mutex_destroy, &gpcie->link_lock);
	if (ret)
		goto pm_disable;

	ret = dw_pcie_host_init(&pci->pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		goto pm_disable;
	}

	pp = &gpcie->pci->pp;
	gpcie->domain = pp->bridge->bus->domain_nr;

	power_stats_init(gpcie);
	ret = device_create_file(dev, &dev_attr_power_stats);
	if (ret) {
		dev_err(dev, "Failed to create power_stats sysfs file\n");
		goto host_deinit;
	}

	ret = device_create_file(dev, &dev_attr_link_state);
	if (ret) {
		dev_err(dev, "Failed to create link_state sysfs file\n");
		goto power_stats_remove;
	}

	list_add_tail(&gpcie->node, &gpcie_inst_list);

	ret = devm_add_action_or_reset(dev, gpcie_list_del, &gpcie->node);
	if (ret)
		goto link_state_remove;

	bus = pci->pp.bridge->bus;
	pci_dev = pci_get_slot(bus, 0);
	if (!pci_dev) {
		dev_err(gpcie->dev, "Could not find the pci_dev");
		ret = -ENODEV;
		goto link_state_remove;
	}

	google_pcie_init_speed_width(gpcie, pci_dev);
	ret = google_pcie_init_debugfs(gpcie);
	if (ret)
		goto put_pci_dev;

	ret = devm_add_action_or_reset(dev, google_pcie_exit_debugfs, gpcie);
	if (ret)
		goto put_pci_dev;

	gpcie->saved_state = pci_store_saved_state(pci_dev);

	pm_runtime_put(dev);
	pci_dev_put(pci_dev);

	return 0;

put_pci_dev:
	pci_dev_put(pci_dev);
link_state_remove:
	device_remove_file(dev, &dev_attr_link_state);
power_stats_remove:
	device_remove_file(dev, &dev_attr_power_stats);
host_deinit:
	dw_pcie_host_deinit(&pci->pp);
pm_disable:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);

	return ret;
}

static int google_pcie_remove(struct platform_device *pdev)
{
	struct google_pcie *gpcie = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	device_remove_file(dev, &dev_attr_link_state);
	device_remove_file(dev, &dev_attr_power_stats);
	pm_runtime_get_sync(dev);
	dw_pcie_host_deinit(&gpcie->pci->pp);
	pm_runtime_disable(dev);
	flush_delayed_work(&gpcie->link_down_work);
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);

	return 0;
}

static const struct of_device_id google_pcie_of_match[] = {
	{
		.compatible = "google,rdo-pcie",
		.data = (void *)GPCIE_RDO_FLAGS,
	},
	{
		.compatible = "google,lga-pcie",
		.data = (void *)GPCIE_LGA_FLAGS,
	},
	{},
};

static int google_pcie_runtime_suspend(struct device *dev)
{
	struct google_pcie *gpcie = dev_get_drvdata(dev);
	struct generic_pm_domain *genpd = pd_to_genpd(dev->pm_domain);
	unsigned long flags;

	/*
	 * If the PCIe power domain cannot be turned OFF as a part of runtime suspend,
	 * keep 'gpcie->powered_on' set to TRUE so that the 'pci_save_state' performed during
	 * system suspend can actually access the DBI registers.
	 */

	trace_pci_suspend_start(dev_name(dev));

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	if (!(genpd->flags & GENPD_FLAG_RPM_ALWAYS_ON))
		gpcie->powered_on = false;
	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);

	google_pcie_set_regulators(gpcie, false);

	trace_pci_suspend_end(dev_name(dev));

	return 0;
}

static int google_pcie_suspend(struct device *dev)
{
	struct google_pcie *gpcie = dev_get_drvdata(dev);
	unsigned long flags;

	// TODO(b/313027848): Add support for PM suspend when enumeration is done
	if (gpcie->enumeration_done)
		return -EINVAL;

	spin_lock_irqsave(&gpcie->power_on_lock, flags);
	gpcie->powered_on = false;
	spin_unlock_irqrestore(&gpcie->power_on_lock, flags);

	google_pcie_set_regulators(gpcie, false);

	return 0;
}

static void google_pcie_patch_phy_fw(struct google_pcie *gpcie)
{
	int i, ret;
	u32 val;

	if (!gpcie->phy_sram_base)
		return;

	ret = readl_poll_timeout(gpcie->top_base + PCIE_PHY_STATUS1,
				 val, (val & SRAM_INIT_DONE),
				 SRAM_INIT_DELAY_US, SRAM_INIT_TIMEOUT_US);
	if (ret)
		return;

	for (i = 0; i < gpcie->phy_sram_size / 2; i++)
		writew(phy_fw_patch[i].val, gpcie->phy_sram_base + phy_fw_patch[i].addr);

	val = readl(gpcie->top_base + PCIE_PHY_CFG1);
	val |= SRAM_EXT_LD_DONE;
	writel(val, gpcie->top_base + PCIE_PHY_CFG1);
}

static int google_pcie_runtime_resume(struct device *dev)
{
	struct google_pcie *gpcie = dev_get_drvdata(dev);
	int ret;
	u32 val;
	u32 vc_settings = FIELD_PREP(PCIE_AW_USER_VC, 1) |
				FIELD_PREP(PCIE_AR_USER_VC, 1);

	trace_pci_resume_start(dev_name(dev));

	ret = google_pcie_rst_init(gpcie);
	if (ret)
		return ret;

	ret = google_pcie_set_regulators(gpcie, true);
	if (ret)
		return ret;

	trace_pci_reg_enable(dev_name(dev));

	/*
	 * On certain platforms, asserting-deasserting the top_csr reset across
	 * L2-L0 sequence leads to bus hang when the pcie_top registers are
	 * accessed. The same happens with DBI registers as well.
	 * For such platforms, group these non-functional resets (button, top_csr)
	 * and de-assert them once.
	 */
	if (gpcie->pcie_flags & GPCIE_FLAG_ONE_TIME_RESET)
		goto functional_resets;

	if (gpcie->pcie_flags & GPCIE_FLAG_BULK_RESET) {
		reset_control_bulk_assert(GPCIE_INIT_NUM_RSTS,
					  gpcie->pcie_init_rsts);

		ret = reset_control_bulk_deassert(GPCIE_INIT_NUM_RSTS,
						  gpcie->pcie_init_rsts);
		if (ret)
			return ret;
	} else {
		reset_control_assert(gpcie->init_rst);

		ret = reset_control_deassert(gpcie->init_rst);
		if (ret)
			return ret;
	}

functional_resets:
	reset_control_assert(gpcie->pwr_up_rst);
	reset_control_assert(gpcie->perst_rst);
	reset_control_assert(gpcie->app_hold_phy_rst);
	reset_control_deassert(gpcie->pwr_up_rst);
	reset_control_deassert(gpcie->perst_rst);

	trace_pci_reset_complete(dev_name(dev));

	google_pcie_patch_phy_fw(gpcie);

	trace_pci_phy_fw_write(dev_name(dev));

	val = readl(gpcie->top_base + PCIE_PHY_CFG1);
	val |= PG_MODE_EN;
	writel(val, gpcie->top_base + PCIE_PHY_CFG1);

	reset_control_deassert(gpcie->app_hold_phy_rst);

	/* Configure VC to 1 for read and write every time after wake */
	val = readl(gpcie->top_base + PCIE_AXI_QOS);
	val &= ~(PCIE_AW_USER_VC | PCIE_AR_USER_VC);
	val |= vc_settings;
	writel(val, gpcie->top_base + PCIE_AXI_QOS);

	/* Disable Power Gating to the controller parts in L1 */
	if (gpcie->l1_pwrgate_disable) {
		val = readl(gpcie->sii_base + PM_CTRL_STATUS);
		val &= ~L1_PWR_OFF_EN;
		writel(val, gpcie->sii_base + PM_CTRL_STATUS);
	}

	trace_pci_resume_end(dev_name(dev));

	gpcie->powered_on = true;

	return 0;
}

int google_pcie_inbound_atu_cfg(int ch_num,
				 unsigned long long src_addr,
				 unsigned long long dst_addr,
				 unsigned long size,
				 int atu_index)
{
	u64 limit_addr;
	u32 retries;
	u32 val;
	u32 offset;
	struct google_pcie *gpcie;
	struct dw_pcie *pci;

	gpcie = google_pcie_get_handle(ch_num);
	if (!gpcie) {
		pr_err("Failed to program inbound ATU, invalid PCIe handle!\n");
		return -EINVAL;
	}
	pci = gpcie->pci;

	/* Setup inbound ATU region for mem transactions */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	offset = PCIE_ATU_UNROLL_BASE(PCIE_ATU_REGION_DIR_IB, atu_index);
#else
	offset = PCIE_GET_ATU_INB_UNR_REG_OFFSET(atu_index);
#endif
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_BASE, 4,
		      lower_32_bits(src_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_UPPER_BASE, 4,
		      upper_32_bits(src_addr));
	limit_addr = src_addr + size - 1;
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_LIMIT, 4,
		      lower_32_bits(limit_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_LOWER_TARGET, 4,
		      lower_32_bits(dst_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_UPPER_TARGET, 4,
		      upper_32_bits(dst_addr));
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL1, 4,
		      PCIE_ATU_TYPE_MEM);
	dw_pcie_write(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL2, 4,
		      PCIE_ATU_ENABLE);

	/* Make sure ATU enable takes effect before any subsequent config and I/O accesses */
	for (retries = 0; retries < LINK_WAIT_MAX_IATU_RETRIES; retries++) {
		dw_pcie_read(pci->atu_base + offset + PCIE_ATU_UNR_REGION_CTRL2,
			     4, &val);
		if (val & PCIE_ATU_ENABLE) {
			dev_info(pci->dev, "Inbound ATU(%d) programmed for %#llx -> %#llx\n",
					atu_index, src_addr, dst_addr);
			return 0;
		}

		mdelay(LINK_WAIT_IATU);
	}

	dev_err(pci->dev, "Inbound iATU is not being enabled\n");
	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(google_pcie_inbound_atu_cfg);

int google_pcie_set_msi_ctrl_addr(int num, u64 msi_ctrl_addr)
{
	struct google_pcie *gpcie = NULL;

	gpcie = google_pcie_get_handle(num);
	if (!gpcie)
		return -EINVAL;

	gpcie->pci->pp.msi_data = msi_ctrl_addr;
	dev_dbg(gpcie->dev, "Updated MSI Control Addr: %pad\n",
		&gpcie->pci->pp.msi_data);

	return 0;
}
EXPORT_SYMBOL_GPL(google_pcie_set_msi_ctrl_addr);


static const struct dev_pm_ops google_pcie_dev_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(google_pcie_suspend, google_pcie_runtime_resume)
	SET_RUNTIME_PM_OPS(google_pcie_runtime_suspend, google_pcie_runtime_resume,
			   NULL)
};

static struct platform_driver google_pcie_driver = {
	.driver = {
		.name	= "google-pcie",
		.of_match_table = google_pcie_of_match,
		.suppress_bind_attrs = true,
		.pm = &google_pcie_dev_pm_ops,
	},
	.probe = google_pcie_probe,
	.remove = google_pcie_remove,
};

module_platform_driver(google_pcie_driver);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google PCIe Driver");
MODULE_LICENSE("GPL");
