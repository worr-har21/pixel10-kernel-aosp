# SPDX-License-Identifier: GPL-2.0

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)
O ?= $(OUT_DIR)
OBJTREE ?= $(O)/$(M)

BMS_SYMVERS = $(OUT_DIR)/../private/google-modules/bms/misc/Module.symvers
ifneq ("$(wildcard $(BMS_SYMVERS))","")
  EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/bms/misc/Module.symvers
endif

TRUSTY_SYMVERS = $(OUT_DIR)/../private/google-modules/trusty/Module.symvers
ifneq ($(wildcard $(TRUSTY_SYMVERS)),)
EXTRA_SYMBOLS += $(TRUSTY_SYMVERS)
endif

GSA_SYMVERS = $(OUT_DIR)/../private/google-modules/soc/gs/drivers/soc/google/gsa/Module.symvers
ifneq ($(wildcard $(GSA_SYMVERS)),)
EXTRA_SYMBOLS += $(GSA_SYMVERS)
endif

GS_PERF_MON_SYMVERS = $(OUT_DIR)/../private/google-modules/perf/core/gs_perf_mon/gs_perf_mon_Module.symvers
ifneq ($(wildcard $(GS_PERF_MON_SYMVERS)),)
EXTRA_SYMBOLS += $(GS_PERF_MON_SYMVERS)
endif

GS_DOMAIN_IDLE_SYMVERS = $(OUT_DIR)/../private/google-modules/perf/core/gs_domain_idle/gs_domain_idle_Module.symvers
ifneq ($(wildcard $(GS_DOMAIN_IDLE_SYMVERS)),)
EXTRA_SYMBOLS += $(GS_DOMAIN_IDLE_SYMVERS)
endif

modules modules_install headers_install clean compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)

modules_install: modules_preinstall headers_install

include $(KERNEL_SRC)/$(M)/Makefile.preinstall
