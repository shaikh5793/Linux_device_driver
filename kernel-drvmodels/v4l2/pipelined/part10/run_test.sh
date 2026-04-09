#!/bin/bash
# Part 10: Pipeline Validation — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads hw + sensor + csi2 + bridge
#   3. Runs test_validate (tests both valid and invalid pipeline configs)
#   4. Verifies EPIPE on format mismatch
#   5. Unloads all modules
#   6. Reports PASS/FAIL

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
        if grep -qi "vsoc\|VSOC" "$dev/name" 2>/dev/null; then
            echo "/dev/$(basename "$dev")"
            return
        fi
    done
}

# Find a /dev/v4l-subdevN node by matching a name pattern
find_vsoc_subdev() {
    local pattern="$1"
    for dev in /sys/class/video4linux/v4l-subdev*; do
        [ -e "$dev/name" ] || continue
        if grep -qi "$pattern" "$dev/name" 2>/dev/null; then
            echo "/dev/$(basename "$dev")"
            return
        fi
    done
}

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    sudo rmmod vsoc_bridge 2>/dev/null || true
    sudo rmmod vsoc_csi2 2>/dev/null || true
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 10: Pipeline Validation"
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
sudo modprobe v4l2-async 2>/dev/null || true
log_info "v4l2-async module loaded"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/soc_hw_platform.ko
log_info "soc_hw_platform loaded"
sudo insmod vsoc_csi2.ko
log_info "vsoc_csi2 loaded"
sudo insmod vsoc_bridge.ko
log_info "vsoc_bridge loaded"
sudo insmod vsoc_sensor.ko
log_info "vsoc_sensor loaded"
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

# Test 2: All modules loaded
if lsmod | grep -q vsoc_sensor && lsmod | grep -q vsoc_csi2 && lsmod | grep -q vsoc_bridge; then
    log_pass "All driver modules loaded"
else
    log_fail "Not all driver modules are loaded"
fi

# Test 3: /dev/media* exists
if ls /dev/media* 2>/dev/null | grep -q .; then
    log_pass "Media device node exists"
else
    log_fail "No /dev/media* device node found"
fi

# Discover VSOC device nodes
VIDEODEV=$(find_vsoc_video)
SENSOR_SUBDEV=$(find_vsoc_subdev "sensor")
CSI2_SUBDEV=$(find_vsoc_subdev "csi2")

if [ -n "$VIDEODEV" ]; then
    log_pass "Found VSOC video device: $VIDEODEV"
else
    log_fail "No VSOC video device found"
fi

if [ -n "$SENSOR_SUBDEV" ]; then
    log_info "Found sensor subdev: $SENSOR_SUBDEV"
else
    log_info "No sensor subdev node found (test will use video device only)"
fi

if [ -n "$CSI2_SUBDEV" ]; then
    log_info "Found CSI-2 subdev: $CSI2_SUBDEV"
else
    log_info "No CSI-2 subdev node found"
fi

# Test 4: Run test_validate binary
if [ -x ./test_validate ] && [ -n "$VIDEODEV" ]; then
    ARGS="$VIDEODEV"
    [ -n "$SENSOR_SUBDEV" ] && ARGS="$ARGS $SENSOR_SUBDEV"
    [ -n "$SENSOR_SUBDEV" ] && [ -n "$CSI2_SUBDEV" ] && ARGS="$ARGS $CSI2_SUBDEV"
    log_info "Running test_validate $ARGS..."
    set +e
    TEST_OUTPUT=$(./test_validate $ARGS 2>&1)
    TEST_RC=$?
    set -e
    echo "$TEST_OUTPUT"
    if [ $TEST_RC -eq 0 ]; then
        log_pass "test_validate exited successfully (exit code 0)"
    else
        log_fail "test_validate failed (exit code $TEST_RC)"
    fi

    # Check for EPIPE validation (format mismatch detection)
    if echo "$TEST_OUTPUT" | grep -qi "EPIPE\|mismatch\|invalid.*fail\|validation.*fail"; then
        log_pass "Pipeline validation: format mismatch correctly detected"
    else
        log_info "Could not confirm EPIPE mismatch test from output"
    fi
else
    log_fail "test_validate binary not found or not executable"
fi

# Test 5: Check dmesg for pipeline validation messages
if sudo dmesg | grep -qi "pipeline\|validate\|link_validate"; then
    log_pass "Pipeline validation messages found in dmesg"
else
    log_info "No pipeline validation messages in dmesg (may be expected)"
fi

echo ""
echo -e "${CYAN}=== Feature Demonstration: Pipeline Validation ===${NC}"
if command -v media-ctl &>/dev/null; then
    MEDIADEV=$(ls /dev/media* 2>/dev/null | tail -1)
    if [ -n "$MEDIADEV" ]; then
        echo "  Subdev pad formats:"
        for entity in "vsoc_sensor" "vsoc_csi2"; do
            echo "    $entity:"
            media-ctl -d "$MEDIADEV" --get-v4l2 "\"$entity\":0" 2>/dev/null || echo "      (not available)"
        done
    fi
fi
echo ""
echo "  Pipeline validation result (from kernel log):"
sudo dmesg 2>/dev/null | grep -iE "pipeline|validate|mismatch|EPIPE|format" | tail -10

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
