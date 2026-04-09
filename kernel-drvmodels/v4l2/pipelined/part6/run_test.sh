#!/bin/bash
# Part 6: Video Capture Bridge — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads hw + sensor + bridge
#   3. Verifies /dev/video* exists and captures frames
#   4. Unloads all modules
#   5. Reports PASS/FAIL

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PASS=0
FAIL=0
TOTAL=0

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC}: $1"; }
log_fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC}: $1"; }
log_info() { echo -e "  ${YELLOW}INFO${NC}: $1"; }

# Find the /dev/videoN node belonging to our vsoc_bridge driver
find_vsoc_video() {
    for dev in /sys/class/video4linux/video*; do
        [ -e "$dev/name" ] || continue
        if grep -q "vsoc\|VSOC" "$dev/name" 2>/dev/null; then
            echo "/dev/$(basename "$dev")"
            return
        fi
    done
}

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    sudo rmmod vsoc_bridge 2>/dev/null || true
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 6: Video Capture Bridge"
echo " Automated Test"
echo "============================================"
echo ""

# Step 1: Build
echo "--- Step 1: Build ---"
make -C ../hw 2>&1 | tail -1
make 2>&1 | tail -1
echo ""

# Step 2: Load modules
echo "--- Step 2: Load Modules ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/soc_hw_platform.ko
log_info "soc_hw_platform loaded"
sudo insmod vsoc_sensor.ko
log_info "vsoc_sensor loaded"
sudo insmod vsoc_bridge.ko
log_info "vsoc_bridge loaded"
sleep 0.5
echo ""

# Step 3: Verify
echo "--- Step 3: Verification ---"

# Test 1: Sensor detected
if sudo dmesg | grep -q "VSOC-3000 sensor detected"; then
    log_pass "Sensor probe: chip ID detected"
else
    log_fail "Sensor probe: chip ID not found in dmesg"
fi

# Test 2: Find vsoc_bridge video device node
VIDEODEV=$(find_vsoc_video)
if [ -n "$VIDEODEV" ]; then
    log_pass "VSOC video device node exists: $VIDEODEV"
else
    log_fail "No VSOC video device node found"
    VIDEODEV=""
fi

# Test 3: Bridge probe message
if sudo dmesg | grep -q "bridge\|VSOC-3000 bridge"; then
    log_pass "Bridge driver probed"
else
    log_fail "Bridge probe message not found in dmesg"
fi

# Test 4: Run test_capture binary against the vsoc_bridge device
if [ -x ./test_capture ] && [ -n "$VIDEODEV" ]; then
    log_info "Running test_capture $VIDEODEV (capturing frames)..."
    if ./test_capture "$VIDEODEV" 2>&1; then
        log_pass "test_capture exited successfully (exit code 0)"
    else
        log_fail "test_capture failed (exit code $?)"
    fi
elif [ ! -x ./test_capture ]; then
    log_fail "test_capture binary not found or not executable"
else
    log_fail "test_capture skipped — no VSOC video device"
fi

# Test 5: Check dmesg for streaming ON/OFF
if sudo dmesg | grep -q "streaming started\|streaming ON\|s_stream.*1"; then
    log_pass "Streaming ON detected in dmesg"
else
    log_fail "Streaming ON not found in dmesg"
fi

if sudo dmesg | grep -q "streaming stopped\|streaming OFF\|s_stream.*0"; then
    log_pass "Streaming OFF detected in dmesg"
else
    log_fail "Streaming OFF not found in dmesg"
fi

echo ""
echo -e "${CYAN}=== Feature Demonstration: Bridge Capture with DMA Ring ===${NC}"
VIDEODEV=$(find_vsoc_video)
if [ -n "$VIDEODEV" ] && command -v v4l2-ctl &>/dev/null; then
    echo "  Device capabilities:"
    v4l2-ctl -d "$VIDEODEV" --all 2>/dev/null | grep -E "Driver|Card|Bus|Capabilities|Video Capture|Streaming" || true
    echo ""
    echo "  Supported formats:"
    v4l2-ctl -d "$VIDEODEV" --list-formats-ext 2>/dev/null || true
fi
echo ""
echo "  Driver lifecycle (from kernel log):"
sudo dmesg 2>/dev/null | grep -i "vsoc" | grep -iE "streaming|registered|sensor|bridge" | tail -10

# Show relevant kernel messages
echo ""
echo "--- Kernel Log ---"
sudo dmesg 2>/dev/null | grep -i "vsoc\|v4l2" | tail -20
echo ""

echo ""
echo "============================================"
echo " Results: $PASS passed, $FAIL failed (of $TOTAL)"
if [ $FAIL -eq 0 ]; then
    echo -e " ${GREEN}ALL TESTS PASSED${NC}"
else
    echo -e " ${RED}SOME TESTS FAILED${NC}"
fi
echo "============================================"
exit $FAIL
