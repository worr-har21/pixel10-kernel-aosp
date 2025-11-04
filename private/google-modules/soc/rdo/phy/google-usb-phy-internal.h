/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright 2023 Google LLC
 * Google's SoC-specific Glue Driver for the dwc3 USB controller
 */

/* HSIO_N_USB_CFG_CSR block */
#define USBDP_TOP_CFG_REG_OFFSET 0x44
/* HSIO_N_USB2_CFG_CSR block */
#define USB2PHY_CFG19_OFFSET   0x0
#define USB2PHY_CFG20_OFFSET   0x4
#define USB2PHY_CFG21_OFFSET   0x8

/* HSIO_N_DPTX_TCA_CSR block */
#define TCA_CLK_RST_OFFSET 0x0
#define TCA_INTR_STS_OFFSET	 0x8
#define TCA_GCFG_OFFSET		0x10
#define TCA_TCPC_OFFSET		 0x14
#define TCA_CTRLSYNCMODE_DBG0_OFFSET	0x28
#define TCA_CTRLSYNCMODE_DBG1_OFFSET	0x2c
#define TCA_CTRLSYNCMODE_DBG2_OFFSET	0x30
#define TCA_PSTATE_0_OFFSET		0x50
#define TCA_PSTATE_1_OFFSET		0x54
#define TCA_PSTATE_2_OFFSET		0x58
#define TCPC_MUX_NO_CONN		0x0
#define TCPC_MUX_USB_ONLY		0x1
#define TCPC_MUX_DP_ONLY		0x2
#define TCPC_MUX_USB_DP			0x3

/* HSIO_N_DP_TOP_CSR block */
#define DP_USBCS_PHY_CFG1_OFFSET 0x28
#define DP_USBCS_PHY_CFG2_OFFSET 0x2C
#define DP_USBCS_PHY_CFG3_OFFSET 0x30
#define DP_USBCS_PHY_CFG4_OFFSET 0x34
#define DP_USBCS_PHY_CFG5_OFFSET 0x38
#define DP_USBCS_PHY_CFG6_OFFSET 0x3C
#define DP_TOP_CSR_PHY_POWER_CONFIG_REG1_OFFSET 0x48U
#define DP_TOP_CSR_DP_CONFIG_OFFSET		0x10
#define DP_TOP_CSR_AUX_CTRL_OFFSET		0x24

/* HSIO_N_DWC_DPTX block */
#define PHYIF_CTRL_OFFSET	0xA00
#define TYPEC_CTRL_OFFSET	0xC08

/* HSIO_N DWC_DPTX_PHY block */
#define SUP_DIG_LVL_OVRD_IN				0x52
#define RAWCMN_DIG_CONFIG_MASTER_VERSION		0x224
#define RAWCMN_DIG_AON_FW_VERSION_0			0x332
#define RAWCMN_DIG_AON_FW_VERSION_1			0x334
#define LANE0_DIG_ASIC_TX_ASIC_OUT			0x201C
#define LANE1_DIG_ASIC_RX_ASIC_OUT_0			0x2588
#define RAWLANE0_DIG_TX_PCS_XF_OUT_0_OFFSET		0x4018
#define RAWLANE1_DIG_RX_PCS_XF_OUT_0_OFFSET		0x4514
#define RAWLANE0_DIG_FSM_MEM_ADDR_MON_OFFSET		0x428a
#define RAWLANE1_DIG_FSM_MEM_ADDR_MON_OFFSET		0x468a
#define RAWLANE0_DIG_TX_CTL_FW_PWRUP_DONE_OFFSET	0x408a
#define RAWLANE1_DIG_TX_CTL_FW_PWRUP_DONE_OFFSET	0x448a
#define RAWCMN_DIG_FW_PWRUP_DONE_OFFSET			0x214
#define SUP_DIG_MPLLA_MPLL_PWR_CTL_STAT_OFFSET		0x104
#define LANE1_DIG_RX_VCOCAL_RX_VCO_STAT_0_OFFSET	0x25de
#define LANE1_DIG_RX_VCOCAL_RX_VCO_STAT_1_OFFSET	0x25e0
#define LANE1_DIG_RX_VCOCAL_RX_VCO_STAT_2_OFFSET	0x25e2
#define RAWLANEAONX_DIG_RX_AFE_BUF_IDAC	0xE24E

#define LANEN_TX_ACK_MASK	     BIT(0)
#define LANEN_RX_ACK_MASK	     BIT(0)

#define LANEN_DIG_ASIC_TX_ASIC_IN_1(_x_)			(0x2016 + ((_x_) * 0x400))
#define TX_MAIN_CURSOR_MASK					0x7f00
#define TX_MAIN_CURSOR_OFFSET					8
#define LANEN_DIG_ASIC_TX_ASIC_IN_2(_x_)			(0x2018 + ((_x_) * 0x400))
#define TX_PRE_CURSOR_MASK					0x7f
#define TX_PRE_CURSOR_OFFSET					0
#define TX_POST_CURSOR_MASK					0x3f80
#define TX_POST_CURSOR_OFFSET					7

/* HSIO_N_DP_PIPE_LANEx block*/
#define UPCSLANE_PIPE_LPC_PHY_TX_CONTROL2_OFFSET		0x402
#define UPCSLANE_PIPE_LPC_PHY_HDP_TX_CONTROL2_OFFSET		0x602
#define TX_DEEMPH_MASK						0x3f
#define TX_DEEMPH_VAL(_x_)					((_x_) << 0)
#define DISABLE_SINGLE_TX_MASK					BIT(6)
#define DISABLE_SINGLE_TX_VAL(_x_)				((_x_) << 6)
#define UPCSLANE_PIPE_LPC_PHY_TX_CONTROL8_OFFSET		0x408
#define UPCSLANE_PIPE_LPC_PHY_HDP_TX_CONTROL8_OFFSET		0x608
#define TX_MARGIN_MASK						0x7
#define TX_MARGIN_VAL(_x_)					((_x_) << 0)

#define UPCSLANE_PIPE_LPC_PHY_VDR_OVRD_OFFSET			0xD71
#define TX_EQ_OVRD_G1_MASK					0x1
#define TX_EQ_OVRD_G1_VAL(_x_)					((_x_) << 0)
#define TX_HDP_EQ_OVRD_G1_MASK					0x4
#define TX_HDP_EQ_OVRD_G1_VAL(_x_)				((_x_) << 2)
/* PRE/MAIN/POST registers have same definition */
#define UPCSLANE_PIPE_LPC_PHY_VDR_PRE_OVRD_OFFSET		0xD80
#define UPCSLANE_PIPE_LPC_PHY_VDR_MAIN_OVRD_OFFSET		0xD81
#define UPCSLANE_PIPE_LPC_PHY_VDR_POST_OVRD_OFFSET		0xD82
#define UPCSLANE_PIPE_LPC_PHY_HDP_VDR_PRE_OVRD_OFFSET		0xD90
#define UPCSLANE_PIPE_LPC_PHY_HDP_VDR_MAIN_OVRD_OFFSET		0xD91
#define UPCSLANE_PIPE_LPC_PHY_HDP_VDR_POST_OVRD_OFFSET		0xD92
#define TX_EQ_OVRD_MASK						0x3f
#define TX_EQ_OVRD_VAL(_x_)					((_x_) << 0)

/* LANEN_DIG_ASIC_TX_ASIC_OUT */
typedef union {
	struct {
		u32 volatile tx_ack : 1;
		u32 volatile detrx_result : 1;
		u32 volatile calib_sts : 2;
	} bf;
	u32 reg_value;
} LANEN_DIG_ASIC_TX_ASIC_OUT;

/* LANEN_DIG_ASIC_RX_ASIC_OUT_0 */
typedef union {
	struct {
		u32 volatile ack : 1;
		u32 volatile valid : 1;
		u32 volatile adapt_sts : 3;
	} bf;
	u32 reg_value;
} LANEN_DIG_ASIC_RX_ASIC_OUT_0;

/* DP_CONFIG_REG DESCRIPTION: DP TX IP configuration register */
union DP_CONFIG_REG {
	struct {
		u32  dp_encryption_mode : 1;
		u32  dsc_enable_0 : 1;
		u32  dsc_enable_1 : 1;
		u32  reserved_3 : 1;
		u32  reg_dp_hpd : 1;
		u32  reserved_5_32 : 27;
	} bf;
	u32 reg_value;
};

/* AUX_CTRL DESCRIPTION: USB 3.2 PHY AUX Pin configurations */
union AUX_CTRL {
	struct {
		u32  phy_aux_ctrl : 4;
		u32  phy_aux_dp_dn_swap : 1;
		u32  phy_aux_hys_tune : 2;
		u32  phy_aux_pwdnb : 1;
		u32  phy_aux_vod_tune : 1;
		u32  reserved_9_32 : 23;
	} bf;
	u32 reg_value;
};

/* RAWLANEN_DIG_TX_PCS_XF_OUT_0 */
typedef union {
	struct {
		u16 volatile ack : 1;
	} bf;
	u16 reg_value;
} RAWLANEN_DIG_TX_PCS_XF_OUT_0;

/* RAWLANEN_DIG_RX_PCS_XF_OUT_0 */
typedef union {
	struct {
		u16 volatile ack : 1;
	} bf;
	u16 reg_value;
} RAWLANEN_DIG_RX_PCS_XF_OUT_0;

/* RAWLANEN_DIG_FSM_MEM_ADDR_MON */
typedef union {
	struct {
		u16 volatile mem_addr : 16;
	} bf;
	u16 reg_value;
} RAWLANEN_DIG_FSM_MEM_ADDR_MON;

/* RAWLANEN_DIG_TX_CTL_FW_PWRUP_DONE */
typedef union {
	struct {
		u16 volatile done : 1;
	} bf;
	u16 reg_value;
} RAWLANEN_DIG_TX_CTL_FW_PWRUP_DONE;

/* RAWCMN_DIG_FW_PWRUP_DONE */
typedef union {
	struct {
		u16 volatile done : 1;
	} bf;
	u16 reg_value;
} RAWCMN_DIG_FW_PWRUP_DONE;

/* SUP_DIG_MPLLA_MPLL_PWR_CTL_STAT */
typedef union {
	struct {
		u16 volatile fsm_state : 4;
		u16 volatile mpll_tooslow : 1;
		u16 volatile chkfrq_done : 1;
		u16 volatile mpll_cal_rdy : 1;
		u16 volatile mpll_r_lanes : 1;
		u16 volatile mpll_l_lanes : 1;
		u16 volatile mpll_pclk_en : 1;
		u16 volatile mpll_output_en : 1;
		u16 volatile mpll_fbclk_en : 1;
		u16 volatile mpll_cal : 1;
		u16 volatile mpll_rst : 1;
		u16 volatile mpll_ana_en : 1;
		u16 volatile mpll_ana_vreg_speedup : 1;
	} bf;
	u16 reg_value;
} SUP_DIG_MPLLA_MPLL_PWR_CTL_STAT;

/* LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_0 */
typedef union {
	struct {
		u16 volatile rx_ana_cdr_freq_tune : 10;
		u16 volatile rx_ana_vco_cntr_pd : 1;
		u16 volatile rx_ana_vco_cntr_en : 1;
		u16 volatile rx_ana_cdr_startup : 1;
		u16 volatile rx_ana_cdr_vco_en : 1;
		u16 volatile rx_ana_clk_en : 1;
		u16 volatile reserved_15_15 : 1;
	} bf;
	u16 reg_value;
} LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_0;

/* LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_1 */
typedef union {
	struct {
		u16 volatile rx_vco_fsm_state : 4;
		u16 volatile rx_vco_freq_rst : 1;
		u16 volatile rx_vco_cal_rst : 1;
		u16 volatile rx_vco_contical_en : 1;
		u16 volatile rx_vco_cal_done : 1;
		u16 volatile dpll_freq_rst : 1;
		u16 volatile reserved_15_9 : 7;
	} bf;
	u16 reg_value;
} LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_1;

/* LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_2 */
typedef union {
	struct {
		u16 volatile vco_cntr_final : 13;
		u16 volatile vcoclk_too_fast : 1;
		u16 volatile rx_vco_correct : 1;
		u16 volatile rx_vco_up : 1;
	} bf;
	u16 reg_value;
} LANEN_DIG_RX_VCOCAL_RX_VCO_STAT_2;

/* TCA_PSTATE_0 */
typedef union {
	struct {
		u32 upcs_lane0_powerdown : 4;
		u32 upcs_lane0_maxpclkreq : 2;
		u32 upcs_lane0_maxpclkack : 2;
		u32 upcs_lane0_phystatus : 1;
		u32 upcs_lane0_busy : 1;
		u32 reserved_15_10 : 6;
		u32 upcs_lane1_powerdown : 4;
		u32 upcs_lane1_maxpclkreq : 2;
		u32 upcs_lane1_maxpclkack : 2;
		u32 upcs_lane1_phystatus : 1;
		u32 upcs_lane1_busy : 1;
		u32 reserved_31_26 : 6;
	} bf;
	u32 reg_value;
} TCA_PSTATE_0;

/* TCA_PSTATE_1 */
typedef union {
	struct {
		u32 usb32_lane0_powerdown : 4;
		u32 usb32_lane0_maxpclkreq : 2;
		u32 usb32_lane0_maxpclkack : 2;
		u32 usb32_lane0_phystatus : 1;
		u32 reserved_15_9 : 7;
		u32 usb32_lane1_powerdown : 4;
		u32 usb32_lane1_maxpclkreq : 2;
		u32 usb32_lane1_maxpclkack : 2;
		u32 usb32_lane1_phystatus : 1;
		u32 reserved_31_25 : 7;
	} bf;
	u32 reg_value;
} TCA_PSTATE_1;

/* DWC3 Link LCSR_TX_DEEMPH offset*/
#define DWC3_LINK_LCSR_TX_DEEMPH_OFFSET	0x0
#define DWC3_LINK_LCSR_TX_DEEMPH_1_OFFSET	0x4
#define DWC3_LINK_LCSR_TX_DEEMPH_2_OFFSET	0x8
#define DWC3_LINK_LCSR_TX_DEEMPH_3_OFFSET	0xc

/*  USBCS_PHY_CFG1 DESCRIPTION :USB 3.2 PHY Pin configurations */
typedef union {
	struct {
		u32 volatile phy0_mplla_force_en : 1;
		u32 volatile phy0_mplla_ssc_en : 1;
		u32 volatile phy0_mpllb_force_en : 1;
		u32 volatile phy0_mpllb_ssc_en : 1;
		u32 volatile phy0_ref_alt_clk_lp_sel : 1;
		u32 volatile phy0_ref_use_pad : 1;
		u32 volatile phy0_ref_repeat_clk_en : 1;
		u32 volatile phy0_ref_clkdet_en : 1;
		u32 volatile phy_lane0_rx2tx_par_lb_en : 1;
		u32 volatile phy_lane1_rx2tx_par_lb_en : 1;
		u32 volatile phy0_sram_bypass_mode : 2;
		u32 volatile phy0_sram_ext_ld_done : 1;
		u32 volatile phy0_sram_bootload_bypass_mode : 2;
		u32 volatile phy0_sram_init_done : 1;
		u32 volatile typec_flip_invert : 1;
		u32 volatile sys_vbusvalid : 1;
		u32 volatile phy0_apb0_if_mode : 2;
		u32 volatile phy0_apb0_if_sel : 1;
		u32 volatile dp_pipe_pclk_on : 1;
		u32 resv22 : 10;
	} bf;
	u32 reg_value;
} USBCS_PHY_CFG1;


/*  USBCS_PHY_CFG2 DESCRIPTION :USB 3.2 PHY pclk config */
typedef union {
	struct {
		u32 volatile dp_pipe_lane0_pclkchangeack : 1;
		u32 resv1 : 1;
		u32 volatile dp_pipe_lane0_maxpclkreq : 2;
		u32 volatile dp_pipe_lane0_maxpclkack : 2;
		u32 volatile dp_pipe_lane0_pclkchangeok : 1;
		u32 volatile dp_pipe_lane1_pclkchangeack : 1;
		u32 volatile dp_pipe_lane1_maxpclkreq : 2;
		u32 volatile dp_pipe_lane1_maxpclkack : 2;
		u32 volatile dp_pipe_lane1_pclkchangeok : 1;
		u32 volatile dp_pipe_lane0_disable : 1;
		u32 volatile dp_pipe_lane1_disable : 1;
		u32 volatile ext_pclk_req : 1;
		u32 volatile phy_lane0_power_present : 1;
		u32 volatile phy_lane1_power_present : 1;
		u32 volatile usb32_pipe_lane0_maxpclk_div_ratio : 4;
		u32 volatile dp_pipe_lane0_maxpclk_div_ratio : 4;
		u32 volatile dp_pipe_lane1_maxpclk_div_ratio : 4;
		u32 volatile dp_pipe_tx0_detectrx : 1;
		u32 volatile dp_pipe_tx1_detectrx : 1;
	} bf;
	u32 reg_value;
} USBCS_PHY_CFG2;

/*  USBCS_PHY_CFG3 DESCRIPTION :USB 3.2 PHY RCAL registers */
union USBCS_PHY_CFG3 {
	struct {
		u32 phy_res_req_in : 1;
		u32 resv1 : 1;
		u32 phy_res_req_out : 1;
		u32 phy_rtune_req : 1;
		u32 phy_rtune_ack : 1;
		u32 phy0_res_ext_en : 1;
		u32 phy0_ref_pre_vreg_bypass : 1;
		u32 resv7 : 1;
		u32 phy0_res_ext_rcal : 7;
		u32 resv15 : 17;
	} bf;
	u32 reg_value;
};

/*  USBCS_PHY_CFG4 DESCRIPTION :USB 3.2 PHY Protocol config registers */
union USBCS_PHY_CFG4 {
	struct {
		u32 resv0 : 5;
		u32 protocol1_ext_tx_eq_main_g1_lane0 : 6;
		u32 protocol1_ext_tx_eq_main_g1_lane1 : 6;
		u32 protocol1_ext_tx_eq_main_g2_lane0 : 6;
		u32 protocol1_ext_tx_eq_main_g2_lane1 : 6;
		u32 resv29 : 3;
	} bf;
	u32 reg_value;
};

/*  USBCS_PHY_CFG5 DESCRIPTION :USB 3.2 PHY Protocol config registers */
union USBCS_PHY_CFG5 {
	struct {
		u32 protocol1_ext_tx_eq_ovrd_g1 : 2;
		u32 protocol1_ext_tx_eq_ovrd_g2 : 2;
		u32 protocol1_ext_tx_eq_post_g1_lane0 : 6;
		u32 protocol1_ext_tx_eq_post_g1_lane1 : 6;
		u32 protocol1_ext_tx_eq_post_g2_lane0 : 6;
		u32 protocol1_ext_tx_eq_post_g2_lane1 : 6;
		u32 resv28 : 4;
	} bf;
	u32 reg_value;
};

/*  USBCS_PHY_CFG6 DESCRIPTION :USB 3.2 PHY Protocol config registers */
union USBCS_PHY_CFG6 {
	struct {
		u32 protocol1_ext_tx_eq_pre_g1_lane0 : 6;
		u32 protocol1_ext_tx_eq_pre_g1_lane1 : 6;
		u32 resv12 : 4;
		u32 protocol1_ext_tx_eq_pre_g2_lane0 : 6;
		u32 protocol1_ext_tx_eq_pre_g2_lane1 : 6;
		u32 resv28 : 4;
	} bf;
	u32 reg_value;
};

/*  USBCS_USB2PHY_CFG19 DESCRIPTION :USB 2.0 PHY Pin configurations */
typedef union {
	struct {
		u32 volatile phy_cfg_cr_clk_sel : 1;
		u32 volatile phy_cfg_pll_cpbias_cntrl : 7;
		u32 volatile phy_cfg_pll_fb_div : 12;
		u32 volatile phy_cfg_pll_gmp_cntrl : 2;
		u32 volatile phy_cfg_pll_int_cntrl : 6;
		u32 volatile phy_cfg_por_in_lx : 1;
		u32 volatile phy_cfg_rcal_bypass : 1;
		u32 volatile phy_cfg_rptr_mode : 1;
		u32 volatile phy_cfg_rx_hs_term_en : 1;
	} bf;
	u32 reg_value;
} USB2PHY_CFG19;

/*  USBCS_USB2PHY_CFG20 DESCRIPTION :USB 2.0 PHY Pin configurations */
typedef union {
	struct {
		u32 volatile phy_cfg_pll_prop_cntrl : 6;
		u32 volatile phy_cfg_pll_ref_div : 4;
		u32 volatile phy_cfg_pll_vco_cntrl : 3;
		u32 volatile phy_cfg_pll_vref_tune : 2;
		u32 volatile phy_cfg_rcal_code : 4;
		u32 volatile phy_cfg_rcal_offset : 4;
		u32 volatile phy_cfg_rx_eq_ctle : 2;
		u32 volatile phy_cfg_rx_hs_tune : 3;
		u32 volatile phy_cfg_tx_fsls_slew_rate_tune : 1;
		u32 volatile phy_cfg_tx_fsls_vref_tune : 2;
		u32 volatile phy_cfg_tx_fsls_vreg_bypass : 1;
	} bf;
	u32 reg_value;
} USB2PHY_CFG20;

/*  USBCS_USB2PHY_CFG21 DESCRIPTION :USB 2.0 PHY Pin configurations */
typedef union {
	struct {
		u32 volatile phy_cfg_tx_hs_vref_tune : 3;
		u32 volatile phy_cfg_tx_hs_xv_tune : 2;
		u32 volatile phy_cfg_tx_preemp_tune : 3;
		u32 volatile phy_cfg_tx_res_tune : 2;
		u32 volatile phy_cfg_tx_rise_tune : 2;
		u32 volatile phy_enable : 1;
		u32 volatile ref_freq_sel : 3;
		u32 volatile retenable_n : 1;
		u32 volatile utmi_clk_force_en : 1;
		u32 volatile vbus_valid_ext : 1;
		u32 volatile phy_tx_dig_bypass_sel : 1;
		u32 volatile phy_cfg_jtag_apb_sel : 1;
		u32 volatile phy_cfg_tx_hs_drv_ofst : 2;
		u32 resv23 : 9;
	} bf;
	u32 reg_value;
} USB2PHY_CFG21;

typedef union {
	struct {
		u32 volatile suspend_clk_en : 1;
		u32 volatile tca_ref_clk_en : 1;
		u32 volatile tca_clk_auto_gate_en : 1;
		u32 volatile reserved_7_3 : 5;
		u32 volatile phy_rst_sw : 1;
		u32 volatile xa_rst_sw : 1;
		u32 volatile vba_rst_sw : 1;
		u32 volatile reserved_31_11 : 21;
	} bf;
	u32 reg_value;
} TCA_CLK_RST;

typedef union {
	struct {
		u32 volatile xa_ack_evt : 1;
		u32 volatile xa_timeout_evt : 1;
		u32 volatile const reserved_7_2 : 6;
		u32 volatile sys_vbusvalid_evt : 1;
		u32 volatile const sys_vbusvalid : 1;
		u32 volatile tca_vbusvalid_evt : 1;
		u32 volatile const tca_vbusvalid : 1;
		u32 volatile tca_drv_host_vbus_evt : 1;
		u32 volatile const tca_drv_host_vbus : 1;
		u32 volatile const reserved_15_14 : 2;
		u32 volatile ss_lane1_active_evt : 1;
		u32 volatile const ss_lane1_active : 1;
		u32 volatile const reserved_31_18 : 14;
	} bf;
	u32 reg_value;
} TCA_INTR_STS;

typedef union {
	struct {
		u32 volatile op_mode : 2;
		u32 volatile reserved_7_2 : 6;
		u32 volatile physafe_reset_en : 1;
		u32 volatile reserved_11_9 : 3;
		u32 volatile auto_mode_en : 1;
		u32 volatile reserved_31_13 : 19;
	} bf;
	u32 reg_value;
} TCA_GCFG;

typedef union {
	struct {
		u32 volatile tcpc_mux_control : 3;
		u32 volatile tcpc_connector_orientation : 1;
		u32 volatile tcpc_valid : 1;
		u32 volatile const reserved_31_5 : 27;
	} bf;
	u32 reg_value;
} TCA_TCPC;

typedef union {
	struct {
		u32 volatile phy0_mplla_force_en : 1;
		u32 volatile phy0_mplla_ssc_en : 1;
		u32 volatile phy0_mpllb_force_en : 1;
		u32 volatile phy0_mpllb_ssc_en : 1;
		u32 volatile phy0_ref_alt_clk_lp_sel : 1;
		u32 volatile phy0_ref_use_pad : 1;
		u32 volatile phy0_ref_repeat_clk_en : 1;
		u32 volatile phy0_ref_clkdet_en : 1;
		u32 volatile phy_lane0_rx2tx_par_lb_en : 1;
		u32 volatile phy_lane1_rx2tx_par_lb_en : 1;
		u32 volatile phy0_sram_bypass_mode : 2;
		u32 volatile phy0_sram_ext_ld_done : 1;
		u32 volatile phy0_sram_bootload_bypass_mode : 2;
		u32 volatile phy0_sram_init_done : 1;
		u32 volatile typec_flip_invert : 1;
		u32 volatile sys_vbusvalid : 1;
		u32 volatile phy0_apb0_if_mode : 2;
		u32 volatile phy0_apb0_if_sel : 1;
		u32 volatile dp_pipe_pclk_on : 1;
		u32 resv22 : 10;
	} bf;
	u32 reg_value;
} DP_USBCS_PHY_CFG1;

/* HSIO_N_DWC_DPTX block */
typedef union {
	struct {
		u32 disable_ack : 1; // Controls tca_disable_ack_o o/p pin
		u32 disable_status : 1; // Status of the primary input pin tca_disable_i
		u32 interrupt_status : 1; // Interrupt status pin for TYPE-C
		u32 reserved : 29;
	} bf;
	u32 reg_value;
} TYPEC_CTRL;

typedef union {
	struct {
		u32 tps_sel : 4; // Select PHY training pattern
		u32 phyrate : 2; // Rate setting for the PHY
		u32 phy_lanes : 2; // Number of lanes active
		u32 xmit_enable : 4; // Per-lane PHY xmitter enable
		u32 phy_busy : 4; // Per-lane PHY Status
		u32 ssc_dis : 1; //Disable SSC on the PHY.
		u32 phy_powerdown : 4;// Per-lane PHY power-down control
		u32 reserved_24_21 : 4;
		u32 phy_width : 1; // Data width of main link interface to PHY
		u32 edp_phy_rate : 3; //Support for the eDP rates
		u32 reserved_31_29 : 3;
	} bf;
	u32 reg_value;
} PHYIF_CTRL;

// TODO(mnkumar): This is only for LGA, refactor this after separating the headers
typedef union {
	struct {
		u32 pmgt_ref_clk_req_n : 1;
		u32 ss_lane0_active : 1;
		u32 ss_lane1_active : 1;
		u32 usb32_pipe_lane0_disable : 1;
		u32 usb32_pipe_lane0_tx2rx_loopbk : 1;
		u32 usb32_pipe_rx0_es_mode : 1;
		u32 resv6 : 26;
	} bf;
	u32 reg_value;
} USBDP_TOP_CFG_REG;

typedef union {
	struct {
		u32 volatile phy0_ana_pwr_en : 1;
		u32 volatile pg_mode_en : 1;
		u32 volatile tca_dp4_por : 1;
		u32 resv3 : 11;
		u32 volatile upcs_pipe_config : 18;
	} bf;
	u32 reg_value;
} PHY_power_config_reg1;
