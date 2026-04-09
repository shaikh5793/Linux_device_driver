#!/bin/bash
# Part 10: Full Pipeline (Capstone) — Automated Test with Feature Demonstration
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
    sudo rmmod vout_dmabuf 2>/dev/null || true
    sudo rmmod buf_reader 2>/dev/null || true
    sudo rmmod vcam_expbuf 2>/dev/null || true
    sudo rmmod vcam_hw_platform 2>/dev/null || true
    log_info "All modules unloaded"
}
trap cleanup EXIT

echo "============================================"
echo " Part 10: Full Pipeline (Capstone)"
echo " NEW CONCEPT: Complete V4L2 pipeline: Capture -> Export -> Import -> Output"
echo "============================================"
echo ""

# --- Step 1: Build ---
echo "--- Build ---"
make -C ../hw -s 2>&1 | tail -1
make -C ../part7 -s 2>&1 | tail -1
make -C ../part8 -s 2>&1 | tail -1
make -C ../part9 -s 2>&1 | tail -1
make -s 2>&1 | tail -1
echo ""

# --- Step 2: Load Modules ---
echo "--- Load Modules ---"
sudo dmesg --clear 2>/dev/null || true
sudo insmod ../hw/vcam_hw_platform.ko
sudo insmod ../part7/vcam_expbuf.ko
sudo insmod ../part8/buf_reader.ko
sudo insmod ../part9/vout_dmabuf.ko
sleep 0.3
log_info "All pipeline modules loaded"
echo ""

# --- Step 3: Run Test ---
echo "--- Test Output ---"
set +e
sudo ./test_pipeline 2>&1
TEST_RC=$?
set -e
if [ $TEST_RC -eq 0 ]; then log_pass "test_pipeline exited successfully"; else log_fail "test_pipeline failed (exit $TEST_RC)"; fi
echo ""

# --- Step 4: Feature Demonstration ---
echo -e "${CYAN}=== Feature Demonstration: Complete V4L2 Pipeline: Capture -> Export -> Import -> Output ===${NC}"
log_info "Data flows: capture -> EXPBUF fd -> buf_reader reads -> vout_dmabuf outputs"
echo ""
if command -v v4l2-ctl &>/dev/null; then
    echo "  -- v4l2-ctl --list-devices --"
    v4l2-ctl --list-devices 2>&1 || true
    echo ""
    for i in $(seq 0 9); do
        [ -e "/dev/video$i" ] || continue
        echo "  -- /dev/video$i --"
        v4l2-ctl -d "/dev/video$i" --all 2>/dev/null | head -10
        echo ""
    done
else
    log_info "v4l2-ctl not installed, skipping device enumeration"
fi
echo ""
echo "  -- buf_reader device --"
ls -la /dev/buf_reader 2>/dev/null || log_info "/dev/buf_reader not found"
echo ""

# --- Step 5: Kernel Log ---
echo "--- Kernel Log (filtered) ---"
sudo dmesg | grep -i "vcam_expbuf\|buf_reader\|vout_dmabuf" | tail -20
echo ""

# --- Summary ---
echo "============================================"
echo " Results: $PASS passed, $FAIL failed (of $TOTAL)"
if [ $FAIL -eq 0 ]; then echo -e " ${GREEN}ALL TESTS PASSED${NC}"; else echo -e " ${RED}SOME TESTS FAILED${NC}"; fi
echo "============================================"
exit $FAIL
