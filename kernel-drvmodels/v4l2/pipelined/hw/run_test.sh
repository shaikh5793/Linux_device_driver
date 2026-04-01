#!/bin/bash
# HW: SoC Hardware Platform — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the soc_hw_platform kernel module
#   2. Loads the hardware module
#   3. Runs automated verification checks
#   4. Unloads the module
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
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "soc_hw_platform unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " HW: SoC Hardware Platform"
echo " Automated Test"
echo "============================================"
echo ""

# Step 1: Build
echo "--- Step 1: Build ---"
make 2>&1 | tail -1
echo ""

# Step 2: Load modules
echo "--- Step 2: Load Modules ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod soc_hw_platform.ko
log_info "soc_hw_platform loaded"
sleep 0.5
echo ""

# Step 3: Verify
echo "--- Step 3: Verification ---"

# Test 1: Module is loaded
if lsmod | grep -q soc_hw_platform; then
    log_pass "Module loaded in kernel"
else
    log_fail "Module not found in lsmod"
fi

# Test 2: Check dmesg for I2C bus creation
if sudo dmesg | grep -q "soc_hw_platform.*I2C bus"; then
    log_pass "I2C bus created"
else
    log_fail "I2C bus creation not found in dmesg"
fi

# Test 3: Check dmesg for platform devices
if sudo dmesg | grep -q "soc_hw_platform.*Platform"; then
    log_pass "Platform devices registered"
else
    log_fail "Platform devices not found in dmesg"
fi

# Test 4: Check dmesg for DMA IRQ
if sudo dmesg | grep -q "soc_hw_platform.*DMA IRQ"; then
    log_pass "DMA IRQ allocated"
else
    log_fail "DMA IRQ not found in dmesg"
fi

# Test 5: Check dmesg for successful load message
if sudo dmesg | grep -q "soc_hw_platform.*virtual SoC loaded"; then
    log_pass "Module initialization complete"
else
    log_fail "Module init completion message not found"
fi

# Test 6: Check /sys/bus/i2c/devices for vsoc_sensor device
if ls /sys/bus/i2c/devices/ 2>/dev/null | grep -q .; then
    log_pass "I2C devices directory has entries"
else
    log_fail "No I2C devices found"
fi

# Test 7: Check vsoc_sensor I2C device exists
SENSOR_FOUND=0
for dev in /sys/bus/i2c/devices/*/name; do
    if [ -f "$dev" ] && grep -q "vsoc_sensor" "$dev" 2>/dev/null; then
        SENSOR_FOUND=1
        break
    fi
done
if [ $SENSOR_FOUND -eq 1 ]; then
    log_pass "vsoc_sensor I2C device registered"
else
    log_fail "vsoc_sensor I2C device not found in sysfs"
fi

# Test 8: Check platform devices exist
if ls /sys/bus/platform/devices/ 2>/dev/null | grep -q vsoc; then
    log_pass "VSOC platform devices exist"
else
    log_fail "No VSOC platform devices found"
fi

# Show relevant kernel messages
echo ""
echo "--- Kernel Log ---"
sudo dmesg 2>/dev/null | grep -i "soc_hw_platform" | tail -20
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
