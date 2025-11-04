# Use relative paths instead since the sources are out of kernel source trees.
ifeq ($(KBUILD_OVERLAY_OUT_OF_TREE),1)
 include $(src)/../config_kernel.mk
 ccflags-y += -include ../config_kernel.h \
  -I$(src) \
  -I$(src)/.. \
  -I$(src)/../pvrsrvkm \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH) \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH)/hwdefs \
  -I$(src)/../pvrsrvkm/$(PVR_ARCH)/hwdefs/km

  bridges := $(addprefix -I,$(wildcard $(src)/../pvrsrvkm/$(PVR_ARCH)/generated/*))
else
 include $(srctree)/drivers/staging/imgtec/config_kernel.mk
 ccflags-y += -include drivers/staging/imgtec/config_kernel.h \
  -I$(src) \
  -I$(srctree)/drivers/staging/imgtec \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH) \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH)/hwdefs \
  -I$(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH)/hwdefs/km

 bridges := $(addprefix -I,$(wildcard $(srctree)/drivers/staging/imgtec/pvrsrvkm/$(PVR_ARCH)/generated/*))
endif

ccflags-y += -include $(srctree)/include/generated/uapi/linux/version.h
ccflags-y += $(bridges)

obj-$(CONFIG_POWERVR_MT8173) += mt8173.o
mt8173-y += mt8173_mfgsys.o
mt8173-y += mt8173_sysconfig.o
