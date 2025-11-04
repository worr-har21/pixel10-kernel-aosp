/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent configuration for TPU CPU.
 *
 * Copyright (C) 2024 Google LLC
 */

#ifndef __BUENOS_CONFIG_TPU_CPU_H__
#define __BUENOS_CONFIG_TPU_CPU_H__

#define EDGETPU_REG_RESET_CONTROL                       0x190018
#define EDGETPU_REG_INSTRUCTION_REMAP_CONTROL           0x190070
#define EDGETPU_REG_INSTRUCTION_REMAP_BASE              0x190080
#define EDGETPU_REG_INSTRUCTION_REMAP_LIMIT             0x190090
#define EDGETPU_REG_INSTRUCTION_REMAP_NEW_BASE          0x1900a0

#define EDGETPU_REG_INSTRUCTION_REMAP_AXIUSER_CORE0     0x1900b0
#define EDGETPU_REG_AXIUSER_CORE0                       0x190050

/* funcApbSlaves_tpuTop_tpuTop.LpmControlCsr */
#define EDGETPU_REG_LPM_CONTROL                         0x1E0028

/* funcApbSlaves_debugApbSlaves_apbaddr_dbg_0, 1 base addresses */
#define EDGETPU_REG_EXTERNAL_DEBUG_0_BASE		0x410000
#define EDGETPU_REG_EXTERNAL_DEBUG_1_BASE		0x510000

/* CSR offsets within external debug 0,1 */
#define EDGETPU_REG_EXTERNAL_DEBUG_PROGRAM_COUNTER      0x00a0
#define EDGETPU_REG_EXTERNAL_DEBUG_LOCK_ACCESS          0x0fb0
#define EDGETPU_REG_EXTERNAL_DEBUG_LOCK_STATUS          0x0fb4
#define EDGETPU_REG_EXTERNAL_DEBUG_AUTHSTATUS           0x0fb8
#define EDGETPU_REG_EXTERNAL_DEBUG_OS_LOCK_ACCESS       0x0300
#define EDGETPU_REG_EXTERNAL_DEBUG_PROCESSOR_STATUS     0x0314

#endif /* __BUENOS_CONFIG_TPU_CPU_H__ */
