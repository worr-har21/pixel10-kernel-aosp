# Use relative paths instead since the sources are out of kernel source trees.
ifeq ($(KBUILD_OVERLAY_OUT_OF_TREE),1)
 include $(src)/../config_kernel.mk
 ccflags-y += -include ../config_kernel.h \
  -I$(src) \
  -I$(src)/.. \
  -I$(src)/../pvrsrvkm \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH) \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH)/hwdefs

 ccflags-$(CONFIG_ION:m=y) += \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH)/hwdefs/km
else
 include $(srctree)/drivers/staging/imgtec/config_kernel.mk
 ccflags-y += -include drivers/staging/imgtec/config_kernel.h \
  -I$(src) \
  -I$(srctree)/drivers/staging/imgtec \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH) \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH)/hwdefs

 ccflags-$(CONFIG_ION:m=y) += \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH)/hwdefs/km
endif

ccflags-y += -include $(srctree)/include/generated/uapi/linux/version.h

obj-$(CONFIG_POWERVR_TC) += tc.o
tc-y += tc_drv.o tc_apollo.o tc_odin.o
tc-$(CONFIG_ION) += ion_lma_heap.o tc_ion.o
tc-$(CONFIG_DMABUF_HEAPS) += tc_dmabuf_heap.o dma_lma_heap.o
