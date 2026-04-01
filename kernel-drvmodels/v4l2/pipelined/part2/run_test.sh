#!/bin/bash
# Part 2: v4l2_subdev Basics — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads the hardware module and sensor driver
#   3. Runs automated verification checks
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
echo " Part 2: v4l2_subdev Basics"
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

# Test 1: Check dmesg for sensor probe
if sudo dmesg | grep -q "VSOC-3000 sensor detected"; then
    log_pass "Sensor probe: chip ID detected"
else
    log_fail "Sensor probe: chip ID not found in dmesg"
fi

# Test 2: Check dmesg for subdev registration
if sudo dmesg | grep -q "v4l2_subdev.*registered"; then
    log_pass "v4l2_subdev registered"
else
    log_fail "v4l2_subdev registration not found in dmesg"
fi

# Test 3: Check I2C device in sysfs
SENSOR_FOUND=0
for dev in /sys/bus/i2c/devices/*/name; do
    if [ -f "$dev" ] && grep -q "vsoc_sensor" "$dev" 2>/dev/null; then
        SENSOR_FOUND=1
        break
    fi
done
if [ $SENSOR_FOUND -eq 1 ]; then
    log_pass "vsoc_sensor I2C device in sysfs"
else
    log_fail "vsoc_sensor I2C device not found in sysfs"
fi

# Test 4: Module is loaded
if lsmod | grep -q vsoc_sensor; then
    log_pass "vsoc_sensor module loaded"
else
    log_fail "vsoc_sensor module not in lsmod"
fi

# Test 5: Run test_sensor binary
if [ -x ./test_sensor ]; then
    log_info "Running test_sensor..."
    if ./test_sensor 2>&1 | grep -q "PASS"; then
        log_pass "test_sensor binary reports PASS"
    else
        log_fail "test_sensor binary did not report PASS"
    fi
else
    log_fail "test_sensor binary not found or not executable"
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
