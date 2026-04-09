ifneq ($(KERNELRELEASE),)
obj-m := vout_dmabuf.o
EXTRA_CFLAGS += -DDEBUG
else

KDIR          ?= /lib/modules/$(shell uname -r)/build
PWD           := $(shell pwd)
EXTRA_SYMBOLS := $(PWD)/../hw/Module.symvers

all:
	@if [ ! -f $(EXTRA_SYMBOLS) ]; then \
		echo "ERROR: Build hw/ first: make -C ../hw"; exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(EXTRA_SYMBOLS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_import

install:
	sudo insmod ../hw/vcam_hw_platform.ko 2>/dev/null || true
	sudo insmod vout_dmabuf.ko

remove:
	sudo rmmod vout_dmabuf
	sudo rmmod vcam_hw_platform 2>/dev/null || true

.PHONY: all clean install remove

endif
