/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#ifndef __DW_MIPI_CDPHY_OP__
#define __DW_MIPI_CDPHY_OP__

enum {
	DW_MIPI_CDPHY_OP_PLL_ENABLE = 0x0,	/* enable MIPI PHY's PLL */
	DW_MIPI_CDPHY_OP_PLL_DISABLE,		/* disable MIPI PHY's PLL */
	DW_MIPI_CDPHY_OP_OVR_ULPS_ENTER,	/* set MIPI PHY override ctrl to ULPS state */
	DW_MIPI_CDPHY_OP_OVR_ULPS_EXIT,		/* unset MIPI PHY override ctrl ULPS state */
};

#endif /* __DW_MIPI_CDPHY_OP__ */
