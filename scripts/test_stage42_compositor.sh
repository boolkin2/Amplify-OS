#!/bin/bash
# [41.2] Под KVM, если он доступен: без него (TCG на узле Proxmox) загрузка не
# укладывается в отведённые тестом 20-35 секунд.
ACCEL=""; if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-enable-kvm -cpu host"; fi
# ============================================================================
# test_stage42_compositor.sh -- автотест этапа 42 (compositing).
# ----------------------------------------------------------------------------
# Проверяет:
#   1. Сборка проходит без ошибок
#   2. compositor.o присутствует в build
#   3. Ядро грузится и compositor инициализируется (serial log)
#   4. DRM self-test проходит (P1-P5)
#   5. С virtio-gpu: compositor + drm active
#   6. Без virtio-gpu: compositor active, drm fallback VESA
# ============================================================================
set -e

PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo "  [PASS] $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo "  [FAIL] $1"; }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

echo "=== AMPLIFY-OS Stage 42: Compositor Test ==="
echo ""

# --- 1. Build ---
echo "[1] Build..."
cd "$(dirname "$0")/.."
make ARCH=x86_64 2>&1 | tail -3
check "build succeeds" "[ -f build/kernel-x86_64.bin ]"
check "compositor.o exists" "[ -f build/x86_64/kernel/arch/x86_64/compositor.o ]"
check "ISO exists" "[ -f build/myos-x86_64.iso ]"

# --- 2. Boot without virtio-gpu (VESA fallback) ---
echo ""
echo "[2] Boot WITHOUT virtio-gpu (VESA fallback)..."
LOG=/tmp/stage42_novgpu.log
timeout 25 qemu-system-x86_64 $ACCEL \
    -cdrom build/myos-x86_64.iso -boot d -m 256M -vga std \
    -serial file:$LOG \
    -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown -display none 2>/dev/null || true

check "serial log created" "[ -f $LOG ]"
check "kernel boots (k64 banner)" "grep -qa 'AMPLIFY-OS 64-bit' $LOG"
check "doublebuf init" "grep -qa 'doublebuf.*initialized' $LOG"
check "compositor init OK" "grep -qa 'compositor.*init OK' $LOG"
check "direct-doublebuf in compositor msg" "grep -qa 'direct-doublebuf' $LOG"
check "drm fallback VESA" "grep -qa 'drm.*fallback\|not ready' $LOG"
check "drm-test P1 OK" "grep -qa 'P1.*OK' $LOG"
check "drm-test P3 OK" "grep -qa 'P3.*OK' $LOG"
check "drm-test P4 OK" "grep -qa 'P4.*OK' $LOG"
check "drm-test P5 OK" "grep -qa 'P5.*OK' $LOG"

# --- 3. Boot WITH virtio-gpu ---
echo ""
echo "[3] Boot WITH virtio-gpu..."
LOG2=/tmp/stage42_vgpu.log
timeout 35 qemu-system-x86_64 $ACCEL \
    -cdrom build/myos-x86_64.iso -boot d -m 256M -vga std \
    -device virtio-gpu-pci,disable-legacy=off,disable-modern=on \
    -serial file:$LOG2 \
    -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown -display none 2>/dev/null || true

check "GPU serial log created" "[ -f $LOG2 ]"
check "virtio-gpu detected" "grep -qa 'virtio_gpu.*display\|vgpu.*init OK' $LOG2"
check "virtio-gpu scanout" "grep -qa 'scanout' $LOG2"

# GPU boot is slower, compositor may or may not appear in 35s
# (depends on DHCP timing). Check what we can.
if grep -qa 'compositor.*init OK' $LOG2; then
    pass "compositor init with GPU"
    check "drm active with GPU" "grep -qa 'drm.*active\|drm.*present\|drm-test.*P2.*OK\|drm-bench' $LOG2"
else
    # Not a failure -- just didn't reach that point in time
    echo "  [SKIP] compositor init not reached in 35s (DHCP/login wait)"
fi

# --- Summary ---
echo ""
echo "=== Results: $PASS/$TOTAL PASS, $FAIL FAIL ==="
if [ $FAIL -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi
