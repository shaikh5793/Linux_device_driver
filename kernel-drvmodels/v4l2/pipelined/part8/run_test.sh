#!/bin/bash
# Part 8: Media Controller Basics — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Loads hw + sensor + bridge
#   3. Verifies /dev/media* exists
#   4. Runs test_mc binary
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
    sudo rmmod vsoc_bridge 2>/dev/null || true
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 8: Media Controller Basics"
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

# Test 2: /dev/media* exists
if ls /dev/media* 2>/dev/null | grep -q .; then
    MEDIADEV=$(ls /dev/media* 2>/dev/null | head -1)
    log_pass "Media device node exists: $MEDIADEV"
else
    log_fail "No /dev/media* device node found"
fi

# Test 3: Run test_mc binary
if [ -x ./test_mc ]; then
    log_info "Running test_mc..."
    if ./test_mc 2>&1; then
        log_pass "test_mc exited successfully (exit code 0)"
    else
        log_fail "test_mc failed (exit code $?)"
    fi
else
    log_fail "test_mc binary not found or not executable"
fi

# Test 4: media-ctl topology (optional)
if command -v media-ctl &>/dev/null; then
    MEDIADEV=$(ls /dev/media* 2>/dev/null | head -1)
    if [ -n "$MEDIADEV" ]; then
        log_info "media-ctl available, printing topology..."
        TOPO=$(media-ctl -d "$MEDIADEV" -p 2>&1 || true)
        if echo "$TOPO" | grep -qi "entity\|sensor\|bridge"; then
            log_pass "media-ctl: topology contains entities"
        else
            log_info "media-ctl: could not parse topology"
        fi
    fi
else
    log_info "media-ctl not installed, skipping optional checks"
fi

echo ""
echo -e "${CYAN}=== Feature Demonstration: Media Controller Entities ===${NC}"
if command -v media-ctl &>/dev/null; then
    MEDIADEV=$(ls /dev/media* 2>/dev/null | tail -1)
    if [ -n "$MEDIADEV" ]; then
        echo "  Full media topology ($MEDIADEV):"
        media-ctl -d "$MEDIADEV" -p 2>/dev/null || true
    fi
else
    log_info "Install v4l-utils for media-ctl topology display"
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
