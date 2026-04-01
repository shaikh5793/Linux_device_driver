<!--
Copyright (c) 2024 TECH VEDA
Author: Raghu Bharadwaj
This code is dual-licensed under the MIT License and GPL v2
-->

# Simple Block Device (SBD) - Ramdisk Driver

A Linux kernel block device driver that implements a RAM-based disk for kernel 6.x.

## Quick Start

```bash
# Build
make

# Load driver
sudo insmod sbd.ko

# Use the device
sudo mkfs.ext4 /dev/sbd
sudo mount /dev/sbd /mnt/ramdisk
echo "Hello World" | sudo tee /mnt/ramdisk/test.txt

# Clean up
sudo umount /mnt/ramdisk
sudo rmmod sbd
```
## Device Properties

- **Device Name**: `/dev/sbd`
- **Capacity**: 2MB (512 × 4KB pages)
- **Sector Size**: 512 bytes
- **Major Number**: Auto-assigned
- **Minor Number**: 0

## Testing

```bash
# Check device
lsblk | grep sbd
ls -la /dev/sbd

# Monitor driver activity
sudo dmesg -w

# I/O statistics
iostat -x 1 /dev/sbd


Dual MIT/GPL - Compatible with Linux kernel licensing.
