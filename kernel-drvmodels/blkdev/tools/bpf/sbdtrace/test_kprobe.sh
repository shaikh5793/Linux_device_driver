#!/bin/bash
# Copyright (c) 2024 TECH VEDA
# Author: Raghu Bharadwaj
# This code is dual-licensed under the MIT License and GPL v2
#
# Test script for sbd_kprobe tracing

echo "=== Testing sbd_kprobe BPF tracer ==="
echo

# Check if module is loaded
if ! lsmod | grep -q sbd_ramdisk; then
    echo "ERROR: sbd_ramdisk module not loaded"
    echo "Please run: sudo insmod ../../sbd_ramdisk.ko"
    exit 1
fi

# Check if do_request symbol is available
echo "Checking for do_request symbol..."
if ! cat /proc/kallsyms | grep -q "do_request.*sbd_ramdisk"; then
    echo "WARNING: do_request not found in kallsyms"
    echo "Available symbols:"
    cat /proc/kallsyms | grep sbd_ramdisk | head -10
    echo
    echo "The module may need to be rebuilt and reloaded."
    exit 1
fi

echo "✓ do_request symbol found"
echo

# Run the kprobe tracer
echo "Starting kprobe tracer (Ctrl-C to stop)..."
echo "Generate I/O with: dd if=/dev/sbd of=/dev/null bs=4k count=10"
echo
sudo ./sbd_kprobe
