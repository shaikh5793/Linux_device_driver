# SPDX-License-Identifier: GPL-2.0
#
# VSOC-3000 Part 10: Pipeline Validation
#
# Builds:
#   - vsoc_sensor.ko    (I2C sensor subdev)
#   - vsoc_csi2.ko      (CSI-2 receiver with link_validate)
#   - vsoc_bridge.ko    (platform bridge with media_pipeline_start/stop)
#   - test_validate      (userspace pipeline validation test)
#
# Usage:
#   make               — build kernel modules + test app
#   make modules       — build kernel modules only
#   make test          — build test app only
#   make clean         — clean all build artifacts

ifneq ($(KERNELRELEASE),)
# ---- Called from kbuild ----
obj-m := vsoc_sensor.o vsoc_csi2.o vsoc_bridge.o

else
# ---- Called from command line ----
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all modules test clean

all: modules test

EXTRA_SYMBOLS := $(PWD)/../hw/Module.symvers

modules:
	@if [ ! -f $(EXTRA_SYMBOLS) ]; then \
		echo "ERROR: Build hw/ first: make -C ../hw"; exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBOLS) modules

test: test_validate

test_validate: test_validate.c
	$(CC) -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_validate

endif
