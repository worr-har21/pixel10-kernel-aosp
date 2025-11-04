# SPDX-License-Identifier: GPL-2.0
subdir-ccflags-y += -Wno-missing-prototypes -Wno-unused-but-set-variable
obj-y += arm/arm-smmu-v3/
obj-$(CONFIG_IOMMU_IO_PGTABLE_PIXEL) += io-pgtable-pixel.o
io-pgtable-pixel-y += io-pgtable.o io-pgtable-arm.o io-pgtable-arm-common.o
CFLAGS_io-pgtable-arm.o := -Wframe-larger-than=2048
obj-$(CONFIG_GOOGLE_ACFW_DEBUG) += acfw-debug.o
