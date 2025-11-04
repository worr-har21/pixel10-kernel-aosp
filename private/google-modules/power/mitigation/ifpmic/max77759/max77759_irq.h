/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __MAX77759_H
#define __MAX77759_H

#include <linux/platform_device.h>
#include "bcl.h"

int max77759_ifpmic_setup(struct bcl_device *bcl_dev, struct platform_device *pdev);
int max77759_get_irq(struct bcl_device *bcl_dev, u8 *irq_val);
int max77759_clr_irq(struct bcl_device *bcl_dev, int idx);
int max77759_vimon_read(struct bcl_device *bcl_dev);
int google_bcl_max77759_vimon_init(struct bcl_device *bcl_dev);

#endif /* __MAX77759_H */
