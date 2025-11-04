# SPDX-License-Identifier: GPL-2.0-only

KCFLAGS += \
	-I$(KERNEL_SRC)/../private/google-modules/soc/gs/include \
	-I$(KERNEL_SRC)/../private/google-modules/soc/rdo/include \
	-I$(KERNEL_SRC)/../private/google-modules/iif/include \
	-I$(KERNEL_SRC)/../private/google-modules/perf/include

module_syms = $(wildcard $(OUT_DIR)/../private/google-modules/$(1)Module.symvers)

# We depend on trusty to access the gpu_secure trusted firmware app
EXTRA_SYMBOLS += $(call module_syms,trusty/)
# Needed to execute PT API calls (for SLC)
EXTRA_SYMBOLS += $(call module_syms,soc/gs/)
# Needed for the standalone IIF API
EXTRA_SYMBOLS += $(call module_syms,iif/iif_)
# Needed for goog_gtc_get_counter()
EXTRA_SYMBOLS += $(call module_syms,soc/rdo/)
# Needed for google vote manager API calls
EXTRA_SYMBOLS += $(call module_syms,perf/google_vote_manager_)

# This file is called by the GKI build system
modules modules_install clean compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) \
	$(KBUILD_OPTIONS) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
