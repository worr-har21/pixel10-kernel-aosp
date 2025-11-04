KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

KCFLAGS += -I$(KERNEL_SRC)/../private/google-modules/soc/gs/include
KCFLAGS += -I$(KERNEL_SRC)/../private/google-modules/soc/gs/include/uapi

RDO_SYMBOLS = $(O)/../private/google-modules/soc/rdo/Module.symvers
ifneq ($(wildcard $(RDO_SYMBOLS)),)
EXTRA_SYMBOLS += $(RDO_SYMBOLS)
endif
EXTRA_SYMBOLS += $(O)/../private/google-modules/trusty/Module.symvers

EXTRA_CFLAGS+="-Wno-missing-prototypes"

modules modules_install clean compile_commands.json:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) W=1 \
	$(KBUILD_OPTIONS) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(@)
