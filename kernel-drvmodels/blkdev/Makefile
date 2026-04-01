# Copyright (c) 2024 TECH VEDA
# Author: Raghu Bharadwaj
# This code is dual-licensed under the MIT License and GPL v2

obj-m := sbd_ramdisk.o
KDIR= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD)  modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: sbd sbd_ramdisk
sbd:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

sbd_ramdisk:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

.PHONY: sbd_special
sbd_special:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	
