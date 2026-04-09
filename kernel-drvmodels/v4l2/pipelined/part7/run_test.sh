#!/bin/bash
# Part 7: Async Subdev Registration — Automated Test
# Usage: sudo ./run_test.sh
#
# This script:
#   1. Builds the kernel modules and test app
#   2. Test A: Loads hw -> sensor -> bridge (normal order)
#   3. Verifies /dev/video* exists and runs test
#   4. Unloads all modules
#   5. Test B: Loads hw -> bridge -> sensor (reverse order)
#   6. Verifies /dev/video* exists after sensor loads
#   7. Unloads all modules
#   8. Reports PASS/FAIL

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

unload_all() {
    sudo rmmod vsoc_bridge 2>/dev/null || true
    sudo rmmod vsoc_sensor 2>/dev/null || true
    sudo rmmod soc_hw_platform 2>/dev/null || true
}

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    unload_all
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 7: Async Subdev Registration"
echo " Automated Test"
echo "============================================"
echo ""

# Step 1: Build
echo "--- Step 1: Build ---"
make -C ../hw 2>&1 | tail -1
make 2>&1 | tail -1
echo ""

# ==============================
# Test A: Normal load order
# ==============================
echo "--- Test A: Normal Load Order (hw -> sensor -> bridge) ---"
sudo dmesg --clear 2>/dev/null || true
sudo modprobe v4l2-async 2>/dev/null || true
log_info "v4l2-async module loaded"
sudo insmod ../hw/soc_hw_platform.ko
log_info "soc_hw_platform loaded"
sudo insmod vsoc_sensor.ko
log_info "vsoc_sensor loaded"
sudo insmod vsoc_bridge.ko
log_info "vsoc_bridge loaded"
sleep 0.5

# Test A1: Sensor detected
if sudo dmesg | grep -q "VSOC-3000 sensor detected"; then
    log_pass "Test A: Sensor probe detected"
else
    log_fail "Test A: Sensor probe not found"
fi

# Test A2: /dev/video* exists
if ls /dev/video* 2>/dev/null | grep -q .; then
    log_pass "Test A: Video device node exists"
else
    log_fail "Test A: No /dev/video* found"
fi

# Test A3: Run test_async binary
if [ -x ./test_async ]; then
    log_info "Running test_async..."
    if ./test_async 2>&1; then
        log_pass "Test A: test_async exited successfully"
    else
        log_fail "Test A: test_async failed"
    fi
else
    log_fail "Test A: test_async binary not found"
fi

# Unload for Test B
echo ""
log_info "Unloading all modules for Test B..."
unload_all
sleep 0.5

# ==============================
# Test B: Reverse load order
# ==============================
echo ""
echo "--- Test B: Reverse Load Order (hw -> bridge -> sensor) ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/soc_hw_platform.ko
log_info "soc_hw_platform loaded"
sudo insmod vsoc_bridge.ko
log_info "vsoc_bridge loaded (before sensor)"
sleep 0.5

# Test B1: No video device yet (bridge waiting for sensor)
VIDEO_BEFORE=$(ls /dev/video* 2>/dev/null | wc -l)
log_info "Video devices before sensor: $VIDEO_BEFORE"

sudo insmod vsoc_sensor.ko
log_info "vsoc_sensor loaded (after bridge)"
sleep 0.5

# Test B2: Sensor detected
if sudo dmesg | grep -q "VSOC-3000 sensor detected"; then
    log_pass "Test B: Sensor probe detected (reverse order)"
else
    log_fail "Test B: Sensor probe not found (reverse order)"
fi

# Test B3: /dev/video* exists after sensor loads
if ls /dev/video* 2>/dev/null | grep -q .; then
    log_pass "Test B: Video device node exists (async completion)"
else
    log_fail "Test B: No /dev/video* after async registration"
fi

echo ""
echo -e "${CYAN}=== Feature Demonstration: Async Subdev Binding ===${NC}"
echo "  Async binding callbacks (from kernel log):"
sudo dmesg 2>/dev/null | grep -iE "bound|complete|async|waiting" | tail -10
echo ""
echo "  Key observation: The sensor and bridge can load in ANY order."
echo "  The v4l2_async_notifier framework defers binding until both are ready."

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
