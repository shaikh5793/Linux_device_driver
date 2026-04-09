ifneq ($(KERNELRELEASE),)
obj-m := buf_reader.o
EXTRA_CFLAGS += -DDEBUG
else

KDIR          ?= /lib/modules/$(shell uname -r)/build
PWD           := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod buf_reader.ko

remove:
	sudo rmmod buf_reader

.PHONY: all clean install remove

endif
