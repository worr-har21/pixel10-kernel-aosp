/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2025 Google LLC
 */

#include "pcie-designware.h"

void goog_setup_chained_irq_handler(struct dw_pcie_rp *pp);
void goog_pci_bottom_unmask(struct irq_data *d);
