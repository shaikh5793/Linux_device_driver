# SPDX-License-Identifier: GPL-2.0
#
# VSOC-3000 Part 8: Media Controller Basics
#
# Builds:
#   - vsoc_sensor.ko   (I2C sensor subdev with media entity)
#   - vsoc_bridge.ko   (platform bridge with media_device)
#   - test_mc           (userspace media controller test)
#
# Usage:
#   make               — build kernel modules + test app
#   make modules       — build kernel modules only
#   make test          — build test app only
#   make clean         — clean all build artifacts

ifneq ($(KERNELRELEASE),)
# ---- Called from kbuild ----
obj-m := vsoc_sensor.o vsoc_bridge.o

else
# ---- Called from command line ----
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
EXTRA_SYMBOLS := $(PWD)/../hw/Module.symvers

.PHONY: all modules test clean

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBOLS) modules

test: test_mc

test_mc: test_mc.c
	$(CC) -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_mc

endif
