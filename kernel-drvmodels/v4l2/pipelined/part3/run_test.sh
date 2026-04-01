#!/bin/bash
# Part 3: v4l2_subdev_ops — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads hw + sensor + test_bridge
#   3. Verifies dmesg shows s_stream ON, log_status, s_stream OFF
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
    sudo rmmod vsoc_test_bridge 2>/dev/null || true
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 3: v4l2_subdev_ops"
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
sudo insmod vsoc_test_bridge.ko
log_info "vsoc_test_bridge loaded"
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

# Test 2: Bridge found sensor subdev
if sudo dmesg | grep -q "found sensor subdev"; then
    log_pass "Bridge found sensor subdev"
else
    log_fail "Bridge did not find sensor subdev"
fi

# Test 3: s_stream ON called
if sudo dmesg | grep -q "s_stream, 1\|streaming ON"; then
    log_pass "s_stream ON called successfully"
else
    log_fail "s_stream ON not found in dmesg"
fi

# Test 4: log_status called
if sudo dmesg | grep -q "log_status\|Sensor Status"; then
    log_pass "log_status called successfully"
else
    log_fail "log_status call not found in dmesg"
fi

# Test 5: s_stream OFF called
if sudo dmesg | grep -q "s_stream, 0\|streaming OFF"; then
    log_pass "s_stream OFF called successfully"
else
    log_fail "s_stream OFF not found in dmesg"
fi

# Test 6: Test complete message
if sudo dmesg | grep -q "test complete\|test bridge"; then
    log_pass "Bridge test sequence completed"
else
    log_fail "Bridge test completion not found in dmesg"
fi

# Test 7: Run test_stream binary
if [ -x ./test_stream ]; then
    log_info "Running test_stream..."
    OUTPUT=$(./test_stream 2>&1)
    echo "$OUTPUT" | head -5 | while read -r line; do log_info "$line"; done
    log_pass "test_stream binary executed"
else
    log_fail "test_stream binary not found or not executable"
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
