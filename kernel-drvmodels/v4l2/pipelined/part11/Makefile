# SPDX-License-Identifier: GPL-2.0
#
# VSOC-3000 Part 11: Multi-Subdev Pipeline (Sensor -> CSI-2 -> ISP -> DMA)
#
# Builds:
#   - vsoc_sensor.ko    (I2C sensor subdev)
#   - vsoc_csi2.ko      (CSI-2 receiver subdev)
#   - vsoc_isp.ko       (ISP processing subdev)
#   - vsoc_bridge.ko    (platform bridge with 4-entity pipeline)
#   - test_pipeline      (userspace pipeline test)
#
# Usage:
#   make               — build kernel modules + test app
#   make modules       — build kernel modules only
#   make test          — build test app only
#   make clean         — clean all build artifacts

ifneq ($(KERNELRELEASE),)
# ---- Called from kbuild ----
obj-m := vsoc_sensor.o vsoc_csi2.o vsoc_isp.o vsoc_bridge.o

EXTRA_SYMBOLS := $(src)/../hw/Module.symvers
KBUILD_EXTRA_SYMBOLS := $(EXTRA_SYMBOLS)

else
# ---- Called from command line ----
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all modules test clean

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: test_pipeline

test_pipeline: test_pipeline.c
	$(CC) -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_pipeline

endif
