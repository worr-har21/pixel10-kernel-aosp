// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC.
 */

#include "sequence.h"

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>

struct csr_psm_status {
	union {
		struct {
			u8 curr_state : 4;
			u8 state_valid : 1;
			u8 psm_sts : 2;
			u8 sw_seq_done : 1;
			u8 psm_err : 1;
			u8 seq_err : 1;
			u32 reserved : 22;
		};
		u32 reg_val;
	};
};

static_assert(sizeof(struct csr_psm_status) == sizeof(u32));

int poll_for_n(struct device *dev, void __iomem *addr, u32 mask, u32 target,
	       u32 poll_period_ms, u32 total_poll_ms)
{
	u32 poll_cnt = total_poll_ms / poll_period_ms;
	u32 val;

	while (poll_cnt--) {
		val = readl(addr);
		dev_dbg(dev, "Polled %#x (mask = %#x, target = %#x)\n", val,
			mask, target);
		if ((val & mask) == target) {
			dev_dbg(dev, "Poll success\n");
			return 0;
		}
		msleep(poll_period_ms);
	}
	dev_dbg(dev, "Poll failed\n");
	return -ETIMEDOUT;
}

int poll_for(struct device *dev, void __iomem *addr, u32 mask, u32 target)
{
	u32 poll_period_ms = 10;
	u32 total_poll_ms = 100;
	return poll_for_n(dev, addr, mask, target, poll_period_ms,
			  total_poll_ms);
}

int poll_for_psm_state(struct device *dev, void __iomem *psm_addr,
		       u8 target_psm_state)
{
	struct csr_psm_status status_mask = { .curr_state = GENMASK(3, 0),
					      .state_valid = 1 };
	struct csr_psm_status target = { .curr_state = target_psm_state,
					 .state_valid = 1 };

	return poll_for(dev, psm_addr, status_mask.reg_val, target.reg_val);
}
