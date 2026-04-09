#!/bin/bash
# Part 6: V4L2 Controls — Automated Test with Feature Demonstration
# Usage: sudo ./run_test.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PASS=0; FAIL=0; TOTAL=0
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
log_pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC}: $1"; }
log_fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC}: $1"; }
log_info() { echo -e "  ${YELLOW}INFO${NC}: $1"; }

find_vcam_dev() {
    local driver="$1"
    for i in $(seq 0 9); do
        [ -e "/dev/video$i" ] || continue
        if v4l2-ctl -d "/dev/video$i" --all 2>/dev/null | grep -q "Driver name.*: $driver"; then
            echo "/dev/video$i"; return
        fi
    done
}

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    sudo rmmod vcam_ctrl 2>/dev/null || true
    sudo rmmod vcam_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 6: V4L2 Controls"
echo " NEW CONCEPT: V4L2 control framework (brightness, horizontal flip)"
echo "============================================"
echo ""

# --- Step 1: Build ---
echo "--- Build ---"
make -C ../hw -s 2>&1 | tail -1
make -s 2>&1 | tail -1
gcc -Wall -o test_controls test_controls.c 2>&1 || true
echo ""

# --- Step 2: Load Modules ---
echo "--- Load Modules ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam_ctrl.ko
sleep 0.3
log_info "Modules loaded"
echo ""

# --- Step 3: Run Test ---
echo "--- Test Output ---"
set +e
sudo ./test_controls 2>&1
TEST_RC=$?
set -e
if [ $TEST_RC -eq 0 ]; then log_pass "test_controls exited successfully"; else log_fail "test_controls failed (exit $TEST_RC)"; fi
echo ""

# --- Step 4: Feature Demonstration ---
echo -e "${CYAN}=== Feature Demonstration: V4L2 Control Framework (Brightness, Hflip) ===${NC}"
if command -v v4l2-ctl &>/dev/null; then
    VDEV=$(find_vcam_dev "vcam_ctrl")
    if [ -n "$VDEV" ]; then
        log_info "Found vcam_ctrl device at $VDEV"
        echo ""
        echo "  -- v4l2-ctl --list-ctrls --"
        v4l2-ctl -d "$VDEV" --list-ctrls 2>&1
        echo ""
        echo "  -- Set brightness=200, then read back --"
        v4l2-ctl -d "$VDEV" --set-ctrl brightness=200 2>&1
        v4l2-ctl -d "$VDEV" --get-ctrl brightness 2>&1
        echo ""
        echo "  -- Set horizontal_flip=1, then read back --"
        v4l2-ctl -d "$VDEV" --set-ctrl horizontal_flip=1 2>&1
        v4l2-ctl -d "$VDEV" --get-ctrl horizontal_flip 2>&1
    else
        log_info "Could not find vcam_ctrl device via v4l2-ctl"
    fi
else
    log_info "v4l2-ctl not installed, skipping feature demo"
fi
echo ""

# --- Step 5: Kernel Log ---
echo "--- Kernel Log (filtered) ---"
sudo dmesg | grep -i "vcam_ctrl" | tail -15
echo ""

# --- Summary ---
echo "============================================"
echo " Results: $PASS passed, $FAIL failed (of $TOTAL)"
if [ $FAIL -eq 0 ]; then echo -e " ${GREEN}ALL TESTS PASSED${NC}"; else echo -e " ${RED}SOME TESTS FAILED${NC}"; fi
echo "============================================"
exit $FAIL
