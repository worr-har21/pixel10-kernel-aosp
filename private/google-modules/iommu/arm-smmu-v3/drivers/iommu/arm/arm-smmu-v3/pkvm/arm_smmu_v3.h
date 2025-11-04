/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/kvm_asm.h>
#include <arm-smmu-v3/io-pgtable.h>
#include <kvm/iommu.h>

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_PKVM_PIXEL)

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 * @caches_clean_on_power_on
 *			is it safe to elide cache and TLB invalidation commands
 *			while the SMMU is OFF
 *
 * Other members are filled and used at runtime by the SMMU driver.
 */
struct hyp_arm_smmu_v3_device {
	struct kvm_hyp_iommu	iommu;
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	unsigned long		features;
	bool			caches_clean_on_power_on;

	void __iomem		*base;
	u32			cmdq_prod;
	u64			*cmdq_base;
	size_t			cmdq_log2size;
	u64			*strtab_base;
	size_t			strtab_num_entries;
	size_t			strtab_num_l1_entries;
	u8			strtab_split;
	struct io_pgtable_cfg	pgtable_cfg_s1;
	struct io_pgtable_cfg	pgtable_cfg_s2;
	u32			ssid_bits; /* SSID has max of 20 bits*/
	u32			options;
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

enum kvm_arm_smmu_domain_stage {
	KVM_ARM_SMMU_DOMAIN_BYPASS = KVM_IOMMU_DOMAIN_IDMAP_TYPE,
	KVM_ARM_SMMU_DOMAIN_S1,
	KVM_ARM_SMMU_DOMAIN_S2,
};

enum hyp_arm_smmu_v3_err_type {
	HYP_ARM_SMMU_V3_ERR_NONE,
	HYP_ARM_SMMU_V3_ERR_CMDQ_TIMEOUT, /* command queue time out */
};

struct hyp_arm_smmu_v3_err {
	pkvm_handle_t smmu_id;
	enum hyp_arm_smmu_v3_err_type type;
	union {
		struct {
			u32 cmdq_prod;
			u32 cmdq_cons;
			u64 cmdq_prod_sw;
		};
	};
} ____cacheline_aligned_in_smp;;

extern struct hyp_arm_smmu_v3_err *kvm_nvhe_sym(kvm_hyp_smmu_last_err);
#define kvm_hyp_smmu_last_err kvm_nvhe_sym(kvm_hyp_smmu_last_err)

struct hyp_arm_smmu_v3_global_config {
	phys_addr_t	block_region_start;
	size_t		block_region_size;
};

extern struct hyp_arm_smmu_v3_global_config kvm_nvhe_sym(kvm_hyp_smmu_global_config);
#define kvm_hyp_smmu_global_config kvm_nvhe_sym(kvm_hyp_smmu_global_config)

#define ARM_SMMU_FORCE_CACHEABLE	BIT(31)

#endif /* CONFIG_ARM_SMMU_V3_PKVM_PIXEL */

#endif /* __KVM_ARM_SMMU_V3_H */
