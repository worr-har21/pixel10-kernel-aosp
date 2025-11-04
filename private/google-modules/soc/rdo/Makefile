# SPDX-License-Identifier: GPL-2.0-only

# This file is called by the GKI build system

include $(KERNEL_SRC)/../private/google-modules/soc/gs/Makefile.include
ifneq ($(wildcard $(OUT_DIR)/../private/google-modules/perf/google_pm_qos_Module.symvers),)
EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/perf/google_pm_qos_Module.symvers
endif
ifneq ($(wildcard $(OUT_DIR)/../private/google-modules/perf/core/gs_domain_idle/gs_domain_idle_Module.symvers),)
EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/perf/core/gs_domain_idle/gs_domain_idle_Module.symvers
endif
ifneq ($(wildcard $(OUT_DIR)/../private/google-modules/perf/google_vote_manager_Module.symvers),)
EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/perf/google_vote_manager_Module.symvers
endif
ifneq ($(wildcard $(OUT_DIR)/../private/google-modules/soc/gs/drivers/soc/google/gsa/Module.symvers),)
EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/soc/gs/drivers/soc/google/gsa/Module.symvers
endif
BMS_SYMVERS = $(OUT_DIR)/../private/google-modules/bms/misc/Module.symvers
ifneq ("$(wildcard $(BMS_SYMVERS))","")
  EXTRA_SYMBOLS += $(OUT_DIR)/../private/google-modules/bms/misc/Module.symvers
endif

modules modules_install clean compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" ${@}
