#!/bin/bash
# V4L2 Part 2: Virtual Capture Device
# Demonstrates: v4l2_device_register, video_register_device, format negotiation
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
TAG="vcam\|vcam_hw"

echo "=== V4L2 Part 2: Virtual Capture Device ==="
echo ""

# Build
echo "[build] Building hardware platform module..."
make -C "$DIR/../hw" -s
echo "[build] Building kernel module..."
make -C "$DIR" -s
echo "[build] Building test program..."
gcc -Wall -o "$DIR/test_vcam" "$DIR/test_vcam.c"
echo ""

# Clear dmesg marker
sudo dmesg -C 2>/dev/null || true

# Load
echo "[load] Loading hardware platform (creates vcam_hw platform device)..."
sudo insmod "$DIR/../hw/vcam_hw_platform.ko"
sleep 0.3
echo "[load] Loading vcam driver (binds to vcam_hw device via platform bus)..."
sudo insmod "$DIR/vcam.ko"
sleep 0.5

# Show binding
echo "[sysfs] Platform bus binding:"
ls -la /sys/bus/platform/devices/vcam_hw/driver 2>/dev/null || echo "  (no binding found)"
echo ""

# Run
echo "[run] Running test..."
echo ""
sudo "$DIR/test_vcam"
echo ""

# Show results
echo "[dmesg] Kernel messages:"
echo "-----------------------------------------------"
sudo dmesg | grep -i "$TAG" | tail -15
echo "-----------------------------------------------"

# Cleanup
echo ""
echo "[cleanup] Unloading modules (driver first, then hardware)..."
sudo rmmod vcam 2>/dev/null || true
sudo rmmod vcam_hw_platform 2>/dev/null || true
echo ""
echo "Done. Key points:"
echo "  - vcam_hw_platform.ko creates the platform device"
echo "  - vcam.ko registers a platform_driver that binds to it"
echo "  - Platform bus matches by name, probe() called automatically"
echo "  - Format negotiation works (querycap, enum_fmt, s_fmt)"
echo "  - No streaming yet -- Part 3 adds videobuf2"
