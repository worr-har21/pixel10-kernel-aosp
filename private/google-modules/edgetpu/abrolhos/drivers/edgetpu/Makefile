# SPDX-License-Identifier: GPL-2.0
#
# Makefile for EdgeTPU framework and dependent drivers.
#

KBUILD_OPTIONS += CONFIG_ABROLHOS=m

include $(KERNEL_SRC)/../private/google-modules/soc/gs/Makefile.include

# TODO(b/361346732): This should be only used during a transient migration phase. Support for this
# may be discontinued without notice. It is recommended that all kernel_module's are converted to
# ddk_module.
EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/power/mitigation/google_bcl_Module.symvers

ifneq ("$(wildcard $(OUT_DIR)/../private/google-modules/soc/gs/drivers/soc/google/gsa/Module.symvers)","")
EXTRA_SYMBOLS	+= $(OUT_DIR)/../private/google-modules/soc/gs/drivers/soc/google/gsa/Module.symvers
endif

modules modules_install clean compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) W=1 \
	$(KBUILD_OPTIONS) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
