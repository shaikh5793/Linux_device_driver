#!/bin/bash
# V4L2 Part 1: Userspace V4L2 Discovery
# Demonstrates: device enumeration, capabilities, formats, controls
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== V4L2 Part 1: Userspace V4L2 Discovery ==="
echo ""

# Build
echo "[build] Building userspace tools..."
make -C "$DIR" -s
echo ""

# Find a V4L2 video device
DEV=""
for d in /dev/video*; do
    [ -e "$d" ] || continue
    DEV="$d"
    break
done

if [ -z "$DEV" ]; then
    echo "[warn] No /dev/videoN device found."
    echo "       Load Part 2 module first (sudo insmod ../part2/vcam.ko)"
    echo "       or plug in a USB camera."
    echo ""
    echo "Built tools are ready in $DIR:"
    echo "  ./device-enum"
    echo "  ./capabilities /dev/video0"
    echo "  ./formats /dev/video0"
    echo "  ./basic-controls /dev/video0"
    exit 0
fi

echo "[run] Using device: $DEV"
echo ""

echo "--- device-enum ---"
"$DIR/device-enum" || true
echo ""

echo "--- capabilities ---"
"$DIR/capabilities" "$DEV" || true
echo ""

echo "--- formats ---"
"$DIR/formats" "$DEV" || true
echo ""

echo "--- basic-controls ---"
"$DIR/basic-controls" "$DEV" || true
echo ""

echo "Done. Key points:"
echo "  - device-enum scans /dev/videoN and queries VIDIOC_QUERYCAP"
echo "  - capabilities shows driver name, card, and feature flags"
echo "  - formats lists supported pixel formats and resolutions"
echo "  - basic-controls enumerates and reads device controls"
