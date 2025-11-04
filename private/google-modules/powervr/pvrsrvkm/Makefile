
# Use relative paths instead since the sources are out of kernel source trees.
ifneq ($(CONFIG_POWERVR_RDO),)
  PIXEL_SOC=rdo
  BUILD_POWERVR_PIXEL=y
  GTC_FREQUENCY_HZ=25600000
endif

ifneq ($(CONFIG_POWERVR_LGA),)
  PIXEL_SOC=lga
  BUILD_POWERVR_PIXEL=y
  GTC_FREQUENCY_HZ=38400000
endif

ifeq ($(CONFIG_POWERVR_BRINGUP_MODE),y)
  MODE_DIR=bringup
else
  MODE_DIR=production
endif

ifeq ($(KBUILD_OVERLAY_OUT_OF_TREE),1)
  include $(src)/../config_kernel.mk
  ccflags-y += -include $(src)/../config_kernel.h \
   -I$(src)/$(PVR_ARCH)/hwdefs/km \
   -I$(src)/$(PVR_ARCH)/hwdefs \
   -I$(src)/$(PVR_ARCH) \
   -I$(src)/.. \
   -I$(src)
  tmp := $(addprefix -I,$(wildcard $(src)/$(PVR_ARCH)/generated/*))

  ccflags-$(CONFIG_X86_5LEVEL) += \
	-include $(srctree)/arch/x86/include/asm/cpufeatures.h

  ccflags-$(CONFIG_POWERVR_TC:m=y) += -I$(src)/../tc
  ccflags-$(CONFIG_POWERVR_RK3368) += -I$(src)/../rk3368
  ccflags-$(CONFIG_POWERVR_MT8173) += -I$(src)/../mt8173
else
  include $(srctree)/$(src)/../pixel/$(PIXEL_SOC)/$(MODE_DIR)/config_kernel.mk
  ccflags-y += -include $(srctree)/$(src)/../pixel/$(PIXEL_SOC)/$(MODE_DIR)/config_kernel.h \
   -I$(srctree)/$(src)/$(PVR_ARCH)/hwdefs/km \
   -I$(srctree)/$(src)/$(PVR_ARCH)/hwdefs \
   -I$(srctree)/$(src)/$(PVR_ARCH) \
   -I$(srctree)/$(src)/.. \
   -I$(srctree)/$(src)
  tmp := $(addprefix -I,$(wildcard $(srctree)/$(src)/$(PVR_ARCH)/generated/*))

  ccflags-$(CONFIG_POWERVR_TC:m=y) += -I$(srctree)/$(src)/../tc
  ccflags-$(CONFIG_POWERVR_RK3368) += -I$(srctree)/$(src)/../rk3368
  ccflags-$(CONFIG_POWERVR_MT8173) += -I$(srctree)/$(src)/../mt8173
  ccflags-$(BUILD_POWERVR_PIXEL)  += \
      -I$(srctree)/drivers/devfreq \
      -I$(srctree)/$(src)/../pixel \
      -I$(srctree)/$(src)/../pixel/${PIXEL_SOC} \
      -DPIXEL_GPU_GENERATION="\"${PIXEL_SOC}\"" \
      -DGTC_FREQUENCY_HZ="${GTC_FREQUENCY_HZ}" \
      -I$(KERNEL_SRC)/../private/google-modules/soc/rdo/include/ \
      -I$(KERNEL_SRC)/../private/google-modules/trusty/include/ \
      -I$(KERNEL_SRC)/../private/google-modules/soc/gs/include/ \
      -I$(KERNEL_SRC)/../private/google-modules/perf/include/ \
      -I$(srctree)/$(src)/../customer/pixel/include
endif

ccflags-y += $(tmp)

ifeq ($(PVR_BUILD_TYPE),debug)
ccflags-y += -DDYNAMIC_DEBUG_MODULE
endif

ccflags-$(CONFIG_X86) += -mno-soft-float

ccflags-$(PVR_ARCH:rogue=y)    += -DPVR_ARCH_ROGUE
ccflags-$(PVR_ARCH:volcanic=y) += -DPVR_ARCH_VOLCANIC

obj-$(CONFIG_POWERVR_ROGUE) += pvrsrvkm.o

pvrsrvkm-y += allocmem.o
pvrsrvkm-y += cache_km.o
pvrsrvkm-y += connection_server.o
pvrsrvkm-y += debug_common.o
pvrsrvkm-y += devicemem.o
pvrsrvkm-y += devicemem_heapcfg.o
pvrsrvkm-y += devicemem_history_server.o
pvrsrvkm-y += devicemem_server.o
pvrsrvkm-y += devicemem_utils.o
pvrsrvkm-y += di_server.o
pvrsrvkm-y += dkf_server.o
ifeq ($(SUPPORT_DMA_TRANSFER),1)
pvrsrvkm-y += dma_km.o
endif
pvrsrvkm-y += event.o
pvrsrvkm-y += fwload.o
pvrsrvkm-y += handle.o
pvrsrvkm-y += handle_idr.o
pvrsrvkm-y += hash.o
pvrsrvkm-y += hash_functions.o
ifeq ($(PVRSRV_ENABLE_HTB),1)
pvrsrvkm-y += htbserver.o
pvrsrvkm-y += htbuffer.o
pvrsrvkm-y += htb_debug.o
endif
pvrsrvkm-y += info_page_km.o
pvrsrvkm-y += km_apphint.o
pvrsrvkm-y += lists.o
pvrsrvkm-y += mem_utils.o
pvrsrvkm-y += mmu_common.o
pvrsrvkm-y += module_common.o
pvrsrvkm-y += osconnection_server.o
pvrsrvkm-y += osfunc.o
pvrsrvkm-y += osmmap_stub.o
pvrsrvkm-y += physheap.o
pvrsrvkm-y += physmem.o
pvrsrvkm-y += physmem_lma.o
pvrsrvkm-y += physmem_ramem.o
ifeq ($(PVRSRV_ENABLE_DYNAMIC_PHYSHEAPS),1)
pvrsrvkm-y += physmem_dlm.o
pvrsrvkm-y += physmem_ima.o
endif
pvrsrvkm-y += physmem_osmem.o
pvrsrvkm-y += physmem_osmem_linux.o
pvrsrvkm-y += physmem_hostmem.o
pvrsrvkm-y += physmem_test.o
pvrsrvkm-y += pmr.o
pvrsrvkm-y += pmr_env.o
pvrsrvkm-y += pmr_os.o
pvrsrvkm-y += power.o
pvrsrvkm-y += process_stats.o
pvrsrvkm-y += pvr_bridge_k.o
pvrsrvkm-y += pvr_debug.o
pvrsrvkm-y += pvr_notifier.o
pvrsrvkm-y += pvrsrv.o
pvrsrvkm-y += pvrsrv_bridge_init.o
pvrsrvkm-y += pvrsrv_error.o
pvrsrvkm-y += pvrsrv_pool.o
pvrsrvkm-y += ra.o
pvrsrvkm-y += rgx_bridge_init.o
pvrsrvkm-y += rgxbreakpoint.o
pvrsrvkm-y += rgxbvnc.o
pvrsrvkm-y += rgxccb.o
pvrsrvkm-y += rgxcompute.o
pvrsrvkm-y += rgxdebug_common.o
pvrsrvkm-y += rgxfwcmnctx.o
pvrsrvkm-y += rgxfwdbg.o
pvrsrvkm-y += rgxfwriscv.o
pvrsrvkm-y += rgxfwtrace_strings.o
pvrsrvkm-y += rgxkicksync.o
pvrsrvkm-y += rgxmem.o
pvrsrvkm-y += rgxmmuinit.o
pvrsrvkm-y += rgxpower.o
pvrsrvkm-y += rgxregconfig.o
pvrsrvkm-y += rgxshader.o
pvrsrvkm-y += rgxsyncutils.o
pvrsrvkm-y += rgxtdmtransfer.o
pvrsrvkm-y += rgxtimecorr.o
pvrsrvkm-y += rgxutils.o
ifeq ($(SUPPORT_WORKLOAD_ESTIMATION),1)
 pvrsrvkm-y += rgxworkest.o
endif
pvrsrvkm-y += srvcore.o
pvrsrvkm-y += sync.o
pvrsrvkm-y += sync_checkpoint.o
pvrsrvkm-y += sync_server.o
pvrsrvkm-y += sysconfig_cmn.o
pvrsrvkm-y += tlclient.o
pvrsrvkm-y += tlintern.o
pvrsrvkm-y += tlserver.o
pvrsrvkm-y += tlstream.o
pvrsrvkm-y += rgxtimerquery.o
pvrsrvkm-y += uniq_key_splay_tree.o
pvrsrvkm-y += vmm_pvz_server.o
pvrsrvkm-y += vmm_pvz_client.o
pvrsrvkm-y += vz_vmm_pvz.o
pvrsrvkm-y += vz_vmm_vm.o
pvrsrvkm-y += rgxhwperf_common.o

pvrsrvkm-y += $(PVR_ARCH)/rgxdebug.o
pvrsrvkm-y += $(PVR_ARCH)/rgxfwutils.o
pvrsrvkm-y += $(PVR_ARCH)/rgxhwperf.o
pvrsrvkm-y += $(PVR_ARCH)/rgxinit.o
pvrsrvkm-y += $(PVR_ARCH)/rgxmulticore.o
pvrsrvkm-y += $(PVR_ARCH)/rgxstartstop.o
pvrsrvkm-y += $(PVR_ARCH)/rgxta3d.o
pvrsrvkm-y += $(PVR_ARCH)/vmm_type_stub.o

# Kernel srvinit
pvrsrvkm-y += rgxfwimageutils.o
pvrsrvkm-y += rgxlayer_impl_common.o
pvrsrvkm-y += $(PVR_ARCH)/rgxlayer_impl.o
pvrsrvkm-y += $(PVR_ARCH)/rgxsrvinit.o

pvrsrvkm-y += $(PVR_ARCH)/dma_support.o

pvrsrvkm-y += pvr_platform_drv.o

pvrsrvkm-$(CONFIG_DRM) += pvr_drm.o

pvrsrvkm-$(CONFIG_DMA_SHARED_BUFFER) += physmem_dmabuf.o
pvrsrvkm-$(CONFIG_DMA_SHARED_BUFFER) += physmem_dmabuf_fbc_tracker.o

# Platform specific
pvrsrvkm-$(CONFIG_POWERVR_TC:m=y) += ../tc/sysconfig.o

pvrsrvkm-$(CONFIG_POWERVR_RK3368) += ../rk3368/rk3368.o
pvrsrvkm-$(CONFIG_POWERVR_MT8173) += ../mt8173/mt8173.o

pvrsrvkm-$(BUILD_POWERVR_PIXEL) += \
    ../customer/pixel/dvfs.o \
    ../pixel/dvfs.o \
    ../pixel/dvfs_governor.o \
    ../pixel/debug.o \
    ../pixel/genpd.o \
    ../pixel/gpu_uevent.o \
    ../pixel/ioctl.o \
    ../pixel/of.o \
    ../pixel/mba.o \
    ../pixel/physmem.o \
    ../pixel/sscd.o \
    ../pixel/sysconfig.o \
    interrupt_support.o

# Support per-UID TIS
ifeq ($(PVRSRV_ANDROID_TRACE_GPU_WORK_PERIOD),1)
pvrsrvkm-$(BUILD_POWERVR_PIXEL) += ../pixel/uid_time_in_state.o
endif

pvrsrvkm-$(CONFIG_POWERVR_PIXEL_SLC) += \
    ../pixel/slc.o

pvrsrvkm-$(CONFIG_POWERVR_PIXEL_IIF) += \
    ../pixel/fence_manager.o

pvrsrvkm-$(CONFIG_TRUSTY:m=y) += ../pixel/gpu_secure.o

ifeq ($(PVRSRV_ENABLE_GPU_MEMORY_INFO),1)
# RI strings
pvrsrvkm-y += ri_server.o
endif

# Device memory history
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_DEVICEMEM_HISTORY) += devicemem_history_server.o

ifneq (, $(filter 1,$(PVRSRV_TRACE_ROGUE_EVENTS) $(PVRSRV_ANDROID_TRACE_GPU_WORK_PERIOD)))
# GPU tracing in systrace
pvrsrvkm-$(CONFIG_FTRACE) += pvr_gputrace.o
endif

# GPU freq
ifeq ($(PVRSRV_ANDROID_TRACE_GPU_FREQ),1)
pvrsrvkm-$(CONFIG_FTRACE) += pvr_gpufreq.o
endif

# Event tracing
pvrsrvkm-$(CONFIG_EVENT_TRACING) += trace_events.o

# Debug info interface
pvrsrvkm-$(CONFIG_DEBUG_FS) += pvr_debugfs.o
pvrsrvkm-$(CONFIG_PROC_FS) += pvr_procfs.o

# arch-specific wrapper functions
pvrsrvkm-$(CONFIG_X86) += osfunc_x86.o
pvrsrvkm-$(CONFIG_ARM) += osfunc_arm.o
pvrsrvkm-$(CONFIG_ARM64) += osfunc_arm64.o
pvrsrvkm-$(CONFIG_METAG) += osfunc_metag.o
pvrsrvkm-$(CONFIG_MIPS) += osfunc_mips.o

# PDump
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += devicemem_pdump.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += devicememx_pdump.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += pdump_server.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += pdump_mmu.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += pdump_physmem.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += rgxpdump_common.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/rgxpdump.o

# Android native synchronisation
ifeq ($(SUPPORT_NATIVE_FENCE_SYNC),1)
 pvrsrvkm-y += pvr_sync_ioctl_common.o
 ifeq ($(USE_PVRSYNC_DEVNODE),1)
  pvrsrvkm-y += pvr_sync_ioctl_dev.o
 else
  pvrsrvkm-y += pvr_sync_ioctl_drm.o
 endif
 pvrsrvkm-y += \
  pvr_sync_file.o \
  pvr_counting_timeline.o \
  pvr_sw_fence.o \
  pvr_export_fence.o \
  pvr_fence.o
endif

ifeq ($(SUPPORT_LINUX_DVFS),1)
 pvrsrvkm-y += pvr_dvfs_device.o \
  pvr_dvfs_common.o
else
 ifeq ($(SUPPORT_PDVFS),1)
  pvrsrvkm-y += \
   pvr_dvfs_proactive.o \
   pvr_dvfs_common.o \
   rgxpdvfs.o
 endif
endif

ifeq ($(PVRSRV_ENABLE_PVR_ION_STATS),1)
 pvrsrvkm-y += pvr_ion_stats.o
endif

ifeq ($(SUPPORT_DI_BRG_IMPL),1)
 pvrsrvkm-y += di_impl_brg.o
endif

# Generated bridge code
pvrsrvkm-y += $(PVR_ARCH)/generated/devicememhistory_bridge/client_devicememhistory_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/devicememhistory_bridge/server_devicememhistory_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/mm_bridge/server_mm_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/mm_bridge/client_mm_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/cmm_bridge/server_cmm_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxta3d_bridge/server_rgxta3d_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxcmp_bridge/server_rgxcmp_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/srvcore_bridge/server_srvcore_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/sync_bridge/server_sync_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/sync_bridge/client_sync_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/cache_bridge/client_cache_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/cache_bridge/server_cache_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/pvrtl_bridge/server_pvrtl_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/pvrtl_bridge/client_pvrtl_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxfwdbg_bridge/server_rgxfwdbg_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxhwperf_bridge/server_rgxhwperf_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxregconfig_bridge/server_rgxregconfig_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxtimerquery_bridge/server_rgxtimerquery_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/rgxtq2_bridge/server_rgxtq2_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/synctracking_bridge/client_synctracking_direct_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/synctracking_bridge/server_synctracking_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/devicememhistory_bridge/server_devicememhistory_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/devicememhistory_bridge/client_devicememhistory_direct_bridge.o
ifeq ($(PVRSRV_ENABLE_GPU_MEMORY_INFO),1)
pvrsrvkm-y += $(PVR_ARCH)/generated/ri_bridge/server_ri_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/ri_bridge/client_ri_direct_bridge.o
endif
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdumpmm_bridge/server_pdumpmm_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdumpmm_bridge/client_pdumpmm_direct_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdump_bridge/server_pdump_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdump_bridge/client_pdump_direct_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdumpctrl_bridge/client_pdumpctrl_direct_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/pdumpctrl_bridge/server_pdumpctrl_bridge.o
pvrsrvkm-$(CONFIG_POWERVR_ROGUE_PDUMP) += $(PVR_ARCH)/generated/rgxpdump_bridge/server_rgxpdump_bridge.o
pvrsrvkm-$(CONFIG_DMA_SHARED_BUFFER) += $(PVR_ARCH)/generated/dmabuf_bridge/server_dmabuf_bridge.o


# Support kick sync bridge
ifeq ($(SUPPORT_RGXKICKSYNC_BRIDGE),1)
PVRSRV_SUPPORT_KICK_SYNC_BRIDGE := 1
else ifeq ($(CONFIG_POWERVR_ROGUE_PDUMP),y)
PVRSRV_SUPPORT_KICK_SYNC_BRIDGE := 1
else ifeq ($(CONFIG_POWERVR_ROGUE_DEBUG),y)
PVRSRV_SUPPORT_KICK_SYNC_BRIDGE := 1
else
PVRSRV_SUPPORT_KICK_SYNC_BRIDGE := 0
endif
pvrsrvkm-$(PVRSRV_SUPPORT_KICK_SYNC_BRIDGE:1=y) += $(PVR_ARCH)/generated/rgxkicksync_bridge/server_rgxkicksync_bridge.o

# Support mmextmem bridge
ifeq ($(SUPPORT_WRAP_EXTMEM),1)
PVRSRV_SUPPORT_MMEXTMEM_BRIDGE := 1
else
PVRSRV_SUPPORT_MMEXTMEM_BRIDGE := 0
endif
pvrsrvkm-$(PVRSRV_SUPPORT_MMEXTMEM_BRIDGE:1=y) += $(PVR_ARCH)/generated/mmextmem_bridge/server_mmextmem_bridge.o
pvrsrvkm-$(PVRSRV_SUPPORT_MMEXTMEM_BRIDGE:1=y) += physmem_extmem_linux.o
pvrsrvkm-$(PVRSRV_SUPPORT_MMEXTMEM_BRIDGE:1=y) += physmem_extmem.o

ifeq ($(SUPPORT_DI_BRG_IMPL),1)
pvrsrvkm-y += $(PVR_ARCH)/generated/di_bridge/server_di_bridge.o
endif

# Support htb bridge
ifeq ($(PVRSRV_ENABLE_HTB),1)
pvrsrvkm-y += $(PVR_ARCH)/generated/htbuffer_bridge/server_htbuffer_bridge.o
pvrsrvkm-y += $(PVR_ARCH)/generated/htbuffer_bridge/client_htbuffer_direct_bridge.o
endif

# Support dma bridge
ifeq ($(SUPPORT_DMA_TRANSFER),1)
pvrsrvkm-y += $(PVR_ARCH)/generated/dma_bridge/server_dma_bridge.o
endif

# Rogue arch particular files
pvrsrvkm-$(PVR_ARCH:rogue=y) += $(PVR_ARCH)/rgxmipsmmuinit.o
pvrsrvkm-$(PVR_ARCH:rogue=y) += $(PVR_ARCH)/rgxtransfer.o
pvrsrvkm-$(PVR_ARCH:rogue=y) += $(PVR_ARCH)/rgx_hwperf_table.o
pvrsrvkm-$(PVR_ARCH:rogue=y) += $(PVR_ARCH)/generated/rgxtq_bridge/server_rgxtq_bridge.o
pvrsrvkm-$(PVR_ARCH:rogue=y) += $(PVR_ARCH)/generated/rgxbreakpoint_bridge/server_rgxbreakpoint_bridge.o

# Volcanic arch particular files
ifeq ($(SUPPORT_RGXRAY_BRIDGE),1)
PVRSRV_SUPPORT_RAY_BRIDGE := 1
else
PVRSRV_SUPPORT_RAY_BRIDGE := 0
endif
pvrsrvkm-$(PVRSRV_SUPPORT_RAY_BRIDGE:1=y) += \
  $(PVR_ARCH)/generated/rgxray_bridge/server_rgxray_bridge.o
pvrsrvkm-$(PVRSRV_SUPPORT_RAY_BRIDGE:1=y) += $(PVR_ARCH)/rgxray.o
ifeq ($(SUPPORT_WORKLOAD_ESTIMATION),1)
 pvrsrvkm-$(PVRSRV_SUPPORT_RAY_BRIDGE:1=y) += $(PVR_ARCH)/rgxworkest_ray.o
endif
