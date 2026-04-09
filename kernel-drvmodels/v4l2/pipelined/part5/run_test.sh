#!/bin/bash
# Part 5: V4L2 Controls — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads hw + sensor
#   3. Verifies /dev/v4l-subdev* exists
#   4. Runs test_controls binary
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

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 5: V4L2 Controls"
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

# Test 2: /dev/v4l-subdev* exists
if ls /dev/v4l-subdev* 2>/dev/null | grep -q .; then
    SUBDEV=$(ls /dev/v4l-subdev* 2>/dev/null | head -1)
    log_pass "Subdev device node exists: $SUBDEV"
else
    log_fail "No /dev/v4l-subdev* device node found"
fi

# Test 3: Run test_controls binary
if [ -x ./test_controls ]; then
    log_info "Running test_controls..."
    if ./test_controls 2>&1; then
        log_pass "test_controls exited successfully (exit code 0)"
    else
        log_fail "test_controls failed (exit code $?)"
    fi
else
    log_fail "test_controls binary not found or not executable"
fi

# Test 4: v4l2-ctl list controls (optional)
if command -v v4l2-ctl &>/dev/null; then
    SUBDEV_DEV=$(ls /dev/v4l-subdev* 2>/dev/null | head -1)
    if [ -n "$SUBDEV_DEV" ]; then
        log_info "v4l2-ctl available, listing controls..."
        CTRL_OUTPUT=$(v4l2-ctl -d "$SUBDEV_DEV" --list-ctrls 2>&1 || true)
        if echo "$CTRL_OUTPUT" | grep -qi "brightness\|exposure\|gain\|contrast"; then
            log_pass "v4l2-ctl: controls reported"
        else
            log_info "v4l2-ctl: could not list controls (may need different device path)"
        fi
    fi
else
    log_info "v4l2-ctl not installed, skipping optional checks"
fi

echo ""
echo -e "${CYAN}=== Feature Demonstration: V4L2 Subdev Controls ===${NC}"
if command -v v4l2-ctl &>/dev/null; then
    SUBDEV=$(ls /dev/v4l-subdev* 2>/dev/null | head -1)
    if [ -n "$SUBDEV" ]; then
        echo "  Subdev: $SUBDEV"
        echo ""
        echo "  Registered controls:"
        v4l2-ctl -d "$SUBDEV" --list-ctrls 2>/dev/null || true
        echo ""
        echo "  Setting exposure=5000, reading back:"
        v4l2-ctl -d "$SUBDEV" --set-ctrl exposure=5000 2>/dev/null || true
        v4l2-ctl -d "$SUBDEV" --get-ctrl exposure 2>/dev/null || true
    fi
else
    log_info "Install v4l-utils for interactive control demo"
fi

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
