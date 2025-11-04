// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2021-2025 Google LLC
 *
 * Override some functionality from upstream pcie-designware-host driver.
 * Ideally, code in here is structured such that it may eventually fit
 * within the upstream driver if/when the underlying hardware driver/feature
 * can be integrated upstream.
 */

#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdesc.h>
#include <linux/pm_runtime.h>
#include "pcie-designware-host-customized.h"

/*
 * Some chips are configured for edge-triggered interrupts,
 * but the underlying signal is level-based. So with multiple
 * msi interrupts come in back to back, there is possibility
 * that the interrupt signal didn't goes low to high to create
 * edge for each and every incoming msi to allow GIC layer to
 * detect. To avoid that, we have this workaround to set irqchip
 * state to PENDING if necessary.
 */
static int goog_pci_check_if_need_retrigger(int msi_ctrl, struct dw_pcie_rp *pp)
{
	int err = 0, virq;
	unsigned long flags;
	u32 status, mask, irq_type;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	raw_spin_lock_irqsave(&pp->lock, flags);
	virq = pp->msi_irq[msi_ctrl];
	irq_type = irq_get_trigger_type(virq);
	if (!(irq_type & IRQ_TYPE_EDGE_BOTH))
		goto unlock;
	if (pm_runtime_get_if_active(pci->dev, true) <= 0)
		goto unlock;
	status = dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_STATUS +
					   (msi_ctrl * MSI_REG_CTRL_BLOCK_SIZE));
	mask = pp->irq_mask[msi_ctrl];
	pm_runtime_put(pci->dev);

	status &= ~mask;
	if (!status)
		goto unlock;
	err = irq_set_irqchip_state(virq, IRQCHIP_STATE_PENDING, true);
unlock:
	raw_spin_unlock_irqrestore(&pp->lock, flags);
	return err;
}

/* MSI int handler */
static irqreturn_t goog_handle_msi_irq(struct dw_pcie_rp *pp)
{
	int i, pos, err;
	unsigned long val;
	u32 status, num_ctrls;
	irqreturn_t ret = IRQ_NONE;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;

	for (i = 0; i < num_ctrls; i++) {
		status = dw_pcie_readl_dbi(pci, PCIE_MSI_INTR0_STATUS +
					   (i * MSI_REG_CTRL_BLOCK_SIZE));
		if (!status)
			continue;

		ret = IRQ_HANDLED;
		val = status;
		pos = 0;
		while ((pos = find_next_bit(&val, MAX_MSI_IRQS_PER_CTRL,
					    pos)) != MAX_MSI_IRQS_PER_CTRL) {
			generic_handle_domain_irq(pp->irq_domain,
						  (i * MAX_MSI_IRQS_PER_CTRL) +
						  pos);
			pos++;
		}

		err = goog_pci_check_if_need_retrigger(i, pp);
		if (err)
			dev_err(pci->dev, "Failed to set irqchip state %d\n", err);
	}

	return ret;
}

static void goog_chained_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dw_pcie_rp *pp;

	chained_irq_enter(chip, desc);

	pp = irq_desc_get_handler_data(desc);
	goog_handle_msi_irq(pp);

	chained_irq_exit(chip, desc);
}

void goog_setup_chained_irq_handler(struct dw_pcie_rp *pp)
{
	u32 ctrl, num_ctrls;

	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;
	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {
		if (pp->msi_irq[ctrl] > 0)
			irq_set_chained_handler_and_data(pp->msi_irq[ctrl],
							goog_chained_msi_isr, pp);
	}
}
EXPORT_SYMBOL_GPL(goog_setup_chained_irq_handler);

void goog_pci_bottom_unmask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	unsigned int res, bit, ctrl;
	unsigned long flags;
	int err;

	raw_spin_lock_irqsave(&pp->lock, flags);

	ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;
	res = ctrl * MSI_REG_CTRL_BLOCK_SIZE;
	bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;

	pp->irq_mask[ctrl] &= ~BIT(bit);
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + res, pp->irq_mask[ctrl]);

	raw_spin_unlock_irqrestore(&pp->lock, flags);

	/* We may have missed an MSI edge while masked. */
	err = goog_pci_check_if_need_retrigger(ctrl, pp);
	if (err)
		dev_err(pci->dev, "Failed to set irqchip state %d\n", err);
}
EXPORT_SYMBOL_GPL(goog_pci_bottom_unmask);
