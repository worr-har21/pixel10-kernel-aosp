# SPDX-License-Identifier: GPL-2.0
subdir-ccflags-y += -I$(srctree)/drivers/iommu
obj-$(CONFIG_ARM_SMMU_V3_PIXEL) += arm_smmu_v3.o
arm_smmu_v3-y += arm-smmu-v3.o
arm_smmu_v3-y += arm-smmu-v3-common.o
obj-$(CONFIG_ARM_SMMU_V3_PKVM_PIXEL) += arm_smmu_v3_kvm.o
ccflags-$(CONFIG_ARM_SMMU_V3_PKVM_PIXEL) += -Iarch/arm64/kvm/
arm_smmu_v3_kvm-y += arm-smmu-v3-kvm.o
arm_smmu_v3_kvm-y += arm-smmu-v3-common.o

#Build hypervisor objects for module
ifeq ($(CONFIG_ARM_SMMU_V3_PKVM_PIXEL),m)
$(obj)/pkvm/kvm_nvhe.o: FORCE
	$(Q)$(MAKE) $(build)=$(obj)/pkvm $(obj)/pkvm/kvm_nvhe.o

arm_smmu_v3_kvm-y += pkvm/kvm_nvhe.o
endif
