#!/usr/bin/env bash
# [41.2] Под KVM, если он доступен: без него (TCG на узле Proxmox) загрузка не
# укладывается в отведённые тестом 20-35 секунд.
ACCEL=""; if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-enable-kvm -cpu host"; fi
# ============================================================================
# scripts/test_stage41_gpu.sh — автотест для этапа 41 (virtio-gpu + DRM).
# ----------------------------------------------------------------------------
# Что проверяется (статический анализ + boot-тесты):
#   1)  Файлы virtio.{c,h}, virtio_gpu.{c,h}, drm.{c,h} созданы и непустые.
#   2)  Makefile.x86_64 содержит run-gpu и подключает virtio*.c/drm.c.
#   3)  syscall.h содержит AOS_SYS_DRM_OPEN/GET_INFO/PRESENT (0x140..0x142).
#   4)  doublebuf.c вызывает drm_present_primary.
#   5)  kernel_main64 вызывает virtio_gpu_init и drm_init.
#   6)  initrd содержит stage41_gputest.elf.
#   7)  Сборка ARCH=x86_64 проходит без ошибок (если make/gcc доступны).
#   8)  QEMU boot с -device virtio-gpu-pci: в serial-логе появляется
#       "[vgpu] init OK", "[drm] active:", "[virtio_gpu] display WxH",
#       property-based "[drm-test] P1..P6: OK" и "[drm-bench] frames=..."
#       (GPU vs CPU benchmark).
#   9)  QEMU boot БЕЗ -device virtio-gpu-pci: появляется
#       "[vgpu] virtio-gpu PCI device not found", далее НЕ "[vgpu] init OK",
#       и property-test проходит P1, P3, P4, P5.
#   10) QEMU boot c virtio-gpu-pci xres=1920,yres=1080 — высокое разрешение
#       (если поддерживается данной версией QEMU).
#
# Запуск: bash scripts/test_stage41_gpu.sh
#         (из корня репозитория)
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PASS=0
FAIL=0
SKIP=0

ok()    { PASS=$((PASS+1)); printf "  [OK]    %s\n" "$1"; }
fail()  { FAIL=$((FAIL+1)); printf "  [FAIL]  %s\n" "$1"; }
skip()  { SKIP=$((SKIP+1)); printf "  [SKIP]  %s\n" "$1"; }

echo "=== AMPLIFY-OS stage 41 (virtio-gpu) test ==="
echo

# 1. Files exist and non-empty.
echo "[1/10] file presence"
for f in \
    kernel/arch/x86_64/virtio.h \
    kernel/arch/x86_64/virtio.c \
    kernel/arch/x86_64/virtio_gpu.h \
    kernel/arch/x86_64/virtio_gpu.c \
    kernel/arch/x86_64/drm.h \
    kernel/arch/x86_64/drm.c \
    userspace/x86_64/stage41_gputest.c
do
    if [[ -s "$f" ]]; then ok "$f exists"; else fail "$f missing/empty"; fi
done
echo

# 2. Makefile.x86_64 wiring.
echo "[2/10] Makefile.x86_64 wiring"
if grep -aq 'kernel/arch/x86_64/virtio.c'      Makefile.x86_64; then ok "virtio.c in C_SRC";      else fail "virtio.c NOT in C_SRC"; fi
if grep -aq 'kernel/arch/x86_64/virtio_gpu.c'  Makefile.x86_64; then ok "virtio_gpu.c in C_SRC";  else fail "virtio_gpu.c NOT in C_SRC"; fi
if grep -aq 'kernel/arch/x86_64/drm.c'         Makefile.x86_64; then ok "drm.c in C_SRC";          else fail "drm.c NOT in C_SRC"; fi
if grep -aq '^run-gpu:'                        Makefile.x86_64; then ok "run-gpu target defined";  else fail "run-gpu target missing"; fi
if grep -aq 'virtio-gpu-pci'                   Makefile.x86_64; then ok "qemu has -device virtio-gpu-pci"; else fail "qemu missing virtio-gpu-pci"; fi
echo

# 3. Syscall numbers.
echo "[3/10] syscall.h numbers"
if grep -aq 'AOS_SYS_DRM_OPEN\s*0x140'     kernel/arch/x86_64/syscall.h; then ok "AOS_SYS_DRM_OPEN=0x140";     else fail "AOS_SYS_DRM_OPEN missing"; fi
if grep -aq 'AOS_SYS_DRM_GET_INFO\s*0x141' kernel/arch/x86_64/syscall.h; then ok "AOS_SYS_DRM_GET_INFO=0x141"; else fail "AOS_SYS_DRM_GET_INFO missing"; fi
if grep -aq 'AOS_SYS_DRM_PRESENT\s*0x142'  kernel/arch/x86_64/syscall.h; then ok "AOS_SYS_DRM_PRESENT=0x142";  else fail "AOS_SYS_DRM_PRESENT missing"; fi
if grep -aq 'AOS_SYS_DRM_DESTROY\s*0x148'  kernel/arch/x86_64/syscall.h; then ok "AOS_SYS_DRM_DESTROY=0x148";  else fail "AOS_SYS_DRM_DESTROY missing"; fi
echo

# 4. doublebuf integration.
echo "[4/10] doublebuf.c integration"
if grep -aq 'drm_present_primary'        kernel/arch/x86_64/doublebuf.c; then ok "doublebuf calls drm_present_primary";       else fail "doublebuf does not call drm_present_primary"; fi
if grep -aq 'drm_active'                 kernel/arch/x86_64/doublebuf.c; then ok "doublebuf checks drm_active";                 else fail "doublebuf does not check drm_active"; fi
echo

# 5. kernel_main integration.
echo "[5/10] kernel_main64 integration"
if grep -aq 'virtio_gpu_init'  kernel/kernel64_main.c; then ok "kernel_main calls virtio_gpu_init"; else fail "kernel_main missing virtio_gpu_init"; fi
if grep -aq 'drm_init'         kernel/kernel64_main.c; then ok "kernel_main calls drm_init";        else fail "kernel_main missing drm_init"; fi
echo

# 6. PBT-friendly counters in API.
echo "[6/10] PBT counters in virtio_gpu.h"
if grep -aq 'virtio_gpu_cmd_count'   kernel/arch/x86_64/virtio_gpu.h; then ok "cmd_count exposed";  else fail "cmd_count not exposed"; fi
if grep -aq 'virtio_gpu_resp_count'  kernel/arch/x86_64/virtio_gpu.h; then ok "resp_count exposed"; else fail "resp_count not exposed"; fi
if grep -aq 'virtio_gpu_inflight'    kernel/arch/x86_64/virtio_gpu.h; then ok "inflight exposed";   else fail "inflight not exposed"; fi
echo

# 7. Build.
echo "[7/10] build (make ARCH=x86_64)"
if command -v make >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1 && command -v nasm >/dev/null 2>&1; then
    LOG="$(mktemp)"
    if make ARCH=x86_64 >"$LOG" 2>&1; then
        ok "build succeeded"
        # Дополнительно: проверим, что ELF собрался.
        if [[ -s build/kernel-x86_64.bin ]]; then ok "kernel-x86_64.bin produced"; else fail "kernel-x86_64.bin missing"; fi
        if [[ -s build/initrd-x86_64.img ]]; then ok "initrd-x86_64.img produced"; else fail "initrd-x86_64.img missing"; fi
        if [[ -s build/myos-x86_64.iso  ]]; then ok "myos-x86_64.iso produced";   else fail "myos-x86_64.iso missing"; fi
        if [[ -s build/x86_64/userspace/stage41_gputest.elf ]]; then ok "stage41_gputest.elf produced"; else fail "stage41_gputest.elf missing"; fi
    else
        fail "build FAILED — see $LOG"
        echo "    --- last 20 lines ---"
        tail -n 20 "$LOG" | sed 's/^/    /'
    fi
else
    skip "make/gcc/nasm not all present — build skipped"
fi
echo

# 8. QEMU boot WITH virtio-gpu (look for [vgpu] init OK in serial).
echo "[8/10] QEMU boot WITH -device virtio-gpu-pci"
if command -v qemu-system-x86_64 >/dev/null 2>&1 && [[ -s build/kernel-x86_64.bin ]] && [[ -s build/initrd-x86_64.img ]]; then
    LOG="$(mktemp)"
    timeout 45s qemu-system-x86_64 $ACCEL \
        -cdrom build/myos-x86_64.iso -boot d \
        -m 256M -vga std -device virtio-gpu-pci,disable-legacy=off,disable-modern=on \
        -display none -serial file:"$LOG" \
        -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    # Поллим лог 20 секунд, ищем оба маркера.
    for i in $(seq 1 80); do
        sleep 0.5
        grep -aq "\[drm-bench\]" "$LOG" 2>/dev/null && break
    done
    kill -9 $QEMU_PID 2>/dev/null || true
    wait $QEMU_PID 2>/dev/null || true
    # `timeout 25s qemu ...` запущен через background, $! указывает на
    # timeout, не на qemu. kill -9 на timeout не пробрасывает сигнал в
    # qemu, поэтому qemu остаётся жить ещё ~24 секунды и держит lock на
    # disk image. Прибиваем все qemu принудительно.
    pkill -9 -f qemu-system-x86_64 2>/dev/null || true

    if grep -aq "\[vgpu\] init OK"      "$LOG"; then ok "vgpu reports init OK";      else fail "vgpu init NOT seen — see $LOG"; fi
    if grep -aq "\[drm\] active"        "$LOG"; then ok "drm reports active";         else fail "drm NOT active — see $LOG"; fi
    if grep -aq "\[doublebuf\]"         "$LOG"; then ok "doublebuf init seen";        else skip "doublebuf init line missing (but not fatal)"; fi
    # Спец-сообщение из §15 CLAUDE.md.
    if grep -aq "\[virtio_gpu\] display" "$LOG"; then ok "spec message [virtio_gpu] display WxH present"; else fail "spec message missing"; fi
    # Property-based smoke test (kernel side).
    P_FAIL=0
    for p in "P1 (active bool valid): OK" \
             "P2 (active sane dims): OK"  \
             "P4 (cmd >= resp): OK"       \
             "P5 (inflight <= 256): OK"   \
             "P6 (4 frames -> +4 ok, +8 cmd, +8 resp): OK"; do
        if grep -aq "$p" "$LOG"; then
            ok "drm-test $p"
        else
            fail "drm-test $p MISSING"
            P_FAIL=$((P_FAIL+1))
        fi
    done
    # Benchmark — должен напечатать строку. Само значение ratio варьируется
    # (на QEMU без KVM 1.5-4x, на bare KVM 5-10x), поэтому проверяем только
    # наличие строки.
    if grep -aq "\[drm-bench\] frames="  "$LOG"; then ok "GPU vs CPU benchmark ran"; else fail "drm-bench line missing"; fi
    # Прохождение firstboot/login до wm_main (требование §15 CLAUDE.md).
    if grep -aq "\[doublebuf\] initialized" "$LOG"; then ok "boot reaches GUI init"; else fail "boot did not reach GUI init"; fi
else
    skip "qemu-system-x86_64 / build artifacts missing — boot test skipped"
fi
echo

# Подождём пока QEMU из теста 8 окончательно отпустит lock на disk image
# (kill -9 + wait иногда оставляют файл-локу занятой ~секунду на медленных
# машинах). Без этой задержки тест 9 стартует и сразу падает с
# "Failed to get write lock", и serial-лог получается пустой.
sleep 2

# 9. QEMU boot WITHOUT virtio-gpu — should fall back gracefully.
echo "[9/10] QEMU boot WITHOUT -device virtio-gpu-pci (fallback)"
if command -v qemu-system-x86_64 >/dev/null 2>&1 && [[ -s build/kernel-x86_64.bin ]] && [[ -s build/initrd-x86_64.img ]]; then
    LOG="$(mktemp)"
    timeout 45s qemu-system-x86_64 $ACCEL \
        -cdrom build/myos-x86_64.iso -boot d \
        -m 256M -vga std \
        -display none -serial file:"$LOG" \
        -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    for i in $(seq 1 80); do
        sleep 0.5
        grep -aq "\[drm-test\] end" "$LOG" 2>/dev/null && break
    done
    kill -9 $QEMU_PID 2>/dev/null || true
    wait $QEMU_PID 2>/dev/null || true
    # `timeout 25s qemu ...` запущен через background, $! указывает на
    # timeout, не на qemu. kill -9 на timeout не пробрасывает сигнал в
    # qemu, поэтому qemu остаётся жить ещё ~24 секунды и держит lock на
    # disk image. Прибиваем все qemu принудительно.
    pkill -9 -f qemu-system-x86_64 2>/dev/null || true

    if grep -aq "virtio-gpu PCI device not found" "$LOG"; then
        ok "vgpu correctly reports device not found";
    else
        fail "vgpu fallback message missing — see $LOG";
    fi
    if grep -aq "\[vgpu\] init OK" "$LOG"; then
        fail "vgpu unexpectedly initialized without device";
    else
        ok "vgpu correctly stays inactive without device";
    fi
    # Property-test и в режиме fallback должен пройти P1, P3, P4, P5
    # (P2/P6 — active-only, не проверяем).
    for p in "P1 (active bool valid): OK" \
             "P3 (inactive zero frames): OK" \
             "P4 (cmd >= resp): OK" \
             "P5 (inflight <= 256): OK"; do
        if grep -aq "$p" "$LOG"; then
            ok "drm-test fallback $p"
        else
            fail "drm-test fallback $p MISSING"
        fi
    done
else
    skip "qemu / build missing — fallback test skipped"
fi
echo

# Подождём перед следующим запуском.
sleep 2

# 10. Высокое разрешение (хост-virtio-gpu умеет xres/yres через параметры).
#     Спецификация требует «окна перетаскиваются плавно при 1920×1080».
#     Сам compositor использует VESA-fb 1024×768, но GPU scanout у virtio-gpu
#     должен принять xres=1920,yres=1080 и об этом отчитаться в логе.
echo "[10/10] virtio-gpu xres=1920,yres=1080 — high-resolution scanout"
if command -v qemu-system-x86_64 >/dev/null 2>&1 && [[ -s build/kernel-x86_64.bin ]] && [[ -s build/initrd-x86_64.img ]]; then
    LOG="$(mktemp)"
    timeout 45s qemu-system-x86_64 $ACCEL \
        -cdrom build/myos-x86_64.iso -boot d \
        -m 256M -vga std \
        -device virtio-gpu-pci,xres=1920,yres=1080 \
        -display none -serial file:"$LOG" \
        -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    for i in $(seq 1 80); do
        sleep 0.5
        grep -aq "\[virtio_gpu\] display" "$LOG" 2>/dev/null && break
    done
    kill -9 $QEMU_PID 2>/dev/null || true
    wait $QEMU_PID 2>/dev/null || true
    pkill -9 -f qemu-system-x86_64 2>/dev/null || true

    if grep -aq "\[virtio_gpu\] display 1920x1080" "$LOG"; then
        ok "scanout reports 1920x1080"
    else
        # Не все QEMU умеют xres= параметр для virtio-gpu-pci. Если устройство
        # игнорирует — это не fatal: сам GPU-путь должен всё равно работать.
        # Проверяем хотя бы что GPU вообще активен.
        if grep -aq "\[vgpu\] init OK" "$LOG"; then
            skip "qemu virtio-gpu-pci does not honor xres=1920,yres=1080 (GPU still active)"
        else
            fail "1920x1080 mode failed and GPU not active either — see $LOG"
        fi
    fi
else
    skip "qemu missing — 1920x1080 test skipped"
fi
echo

echo "=== Result: $PASS pass, $FAIL fail, $SKIP skip ==="
exit $(( FAIL > 0 ? 1 : 0 ))
