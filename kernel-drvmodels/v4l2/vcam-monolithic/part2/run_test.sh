#!/bin/bash
# Part 2: Basic video_device — Automated Test with Feature Demonstration
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
    sudo rmmod vcam 2>/dev/null || true
    sudo rmmod vcam_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 2: Basic video_device"
echo " NEW CONCEPT: V4L2 device registration with video_register_device()"
echo "============================================"
echo ""

# --- Step 1: Build ---
echo "--- Build ---"
make -C ../hw -s 2>&1 | tail -1
make -s 2>&1 | tail -1
gcc -Wall -o test_vcam test_vcam.c 2>&1 || true
echo ""

# --- Step 2: Load Modules ---
echo "--- Load Modules ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod vcam.ko
sleep 0.3
log_info "Modules loaded"
echo ""

# --- Step 3: Run Test ---
echo "--- Test Output ---"
set +e
sudo ./test_vcam 2>&1
TEST_RC=$?
set -e
if [ $TEST_RC -eq 0 ]; then log_pass "test_vcam exited successfully"; else log_fail "test_vcam failed (exit $TEST_RC)"; fi
echo ""

# --- Step 4: Feature Demonstration ---
echo -e "${CYAN}=== Feature Demonstration: V4L2 Device Registration & Capabilities ===${NC}"
if command -v v4l2-ctl &>/dev/null; then
    VDEV=$(find_vcam_dev "vcam")
    if [ -n "$VDEV" ]; then
        log_info "Found vcam device at $VDEV"
        echo ""
        echo "  -- v4l2-ctl --all --"
        v4l2-ctl -d "$VDEV" --all 2>&1 | head -30
        echo ""
        echo "  -- v4l2-ctl --list-formats-ext --"
        v4l2-ctl -d "$VDEV" --list-formats-ext 2>&1
    else
        log_info "Could not find vcam device via v4l2-ctl"
    fi
else
    log_info "v4l2-ctl not installed, skipping feature demo"
fi
echo ""

# --- Step 5: Kernel Log ---
echo "--- Kernel Log (filtered) ---"
sudo dmesg | grep -i "vcam" | tail -15
echo ""

# --- Summary ---
echo "============================================"
echo " Results: $PASS passed, $FAIL failed (of $TOTAL)"
if [ $FAIL -eq 0 ]; then echo -e " ${GREEN}ALL TESTS PASSED${NC}"; else echo -e " ${RED}SOME TESTS FAILED${NC}"; fi
echo "============================================"
exit $FAIL
