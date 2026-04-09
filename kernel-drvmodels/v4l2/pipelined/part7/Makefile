ifneq ($(KERNELRELEASE),)
obj-m := vsoc_sensor.o vsoc_bridge.o
else

KDIR          ?= /lib/modules/$(shell uname -r)/build
PWD           := $(shell pwd)
EXTRA_SYMBOLS := $(PWD)/../hw/Module.symvers

all:
	@if [ ! -f $(EXTRA_SYMBOLS) ]; then \
		echo "ERROR: Build hw/ first: make -C ../hw"; exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBOLS) modules
	$(CC) -Wall -o test_async test_async.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_async

install:
	sudo insmod ../hw/soc_hw_platform.ko 2>/dev/null || true
	sudo insmod vsoc_sensor.ko
	sudo insmod vsoc_bridge.ko

install-reverse:
	sudo insmod ../hw/soc_hw_platform.ko 2>/dev/null || true
	sudo insmod vsoc_bridge.ko
	sudo insmod vsoc_sensor.ko

remove:
	sudo rmmod vsoc_bridge 2>/dev/null || true
	sudo rmmod vsoc_sensor 2>/dev/null || true
	sudo rmmod soc_hw_platform 2>/dev/null || true

.PHONY: all clean install install-reverse remove

endif
