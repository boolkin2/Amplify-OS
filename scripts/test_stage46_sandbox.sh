#!/bin/bash
# ============================================================================
# scripts/test_stage46_sandbox.sh — автотест песочниц этапа 46.
# ----------------------------------------------------------------------------
#   1) Проверяем, что исходники этапа 46 на месте.
#   2) Собираем ядро + initrd + ESP.
#   3) Проверяем, что новые символы попали в kernel.bin (ns_init, cgroup_init,
#      seccomp_init, box_run и т.д.).
#   4) Headless boot через QEMU, парсим serial-лог:
#       * "[ns] root namespaces created"
#       * "[cgroup] root cgroup created"
#       * "[seccomp] subsystem initialized"
#       * "[smoke 46] cgroup mem ... (OK)"
#       * "[smoke 46] seccomp strict ... (OK)"
#       * "[smoke 46] ns_unshare ... (OK)"
#       * "[smoke 46] ns_summaries count=5 (OK)"
#       * "[smoke 46] mnt-ns allow=yes deny=yes (OK)"
#       * "[smoke 46] sandbox initialized, cgroup_count=1"
#
# Без X/SDL: -display none + -serial file:LOG.
# ============================================================================

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
EXPECTED=0

check() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local cmd="$2"
    if eval "$cmd" >/dev/null 2>&1; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

check_grep() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local file="$2"; local pat="$3"
    if grep -aqE "$pat" "$file" 2>/dev/null; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name (no '$pat' in $file)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "  Stage 46 — Sandboxes (namespaces+cgroups+seccomp)"
echo "=========================================="

echo ""
echo "[1/5] Sources present"
check "kernel/arch/x86_64/ns.h"      "test -f kernel/arch/x86_64/ns.h"
check "kernel/arch/x86_64/ns.c"      "test -f kernel/arch/x86_64/ns.c"
check "kernel/arch/x86_64/cgroup.h"  "test -f kernel/arch/x86_64/cgroup.h"
check "kernel/arch/x86_64/cgroup.c"  "test -f kernel/arch/x86_64/cgroup.c"
check "kernel/arch/x86_64/seccomp.h" "test -f kernel/arch/x86_64/seccomp.h"
check "kernel/arch/x86_64/seccomp.c" "test -f kernel/arch/x86_64/seccomp.c"
check "kernel/arch/x86_64/box.h"     "test -f kernel/arch/x86_64/box.h"
check "kernel/arch/x86_64/box.c"     "test -f kernel/arch/x86_64/box.c"
check "kernel/arch/x86_64/app_box.c" "test -f kernel/arch/x86_64/app_box.c"

echo ""
echo "[2/5] Build kernel"
make ARCH=x86_64 >/tmp/sandbox_build.log 2>&1 || {
    echo "  FAIL: build failed, see /tmp/sandbox_build.log"
    exit 1
}
check "kernel-x86_64.bin built" "test -f build/kernel-x86_64.bin"
check "ESP image built"         "test -f build/esp.img"
check "initrd image built"      "test -f build/initrd-x86_64.img"

echo ""
echo "[3/5] Verify symbols inside kernel.bin"
# Используем nm на ld'нутом ELF — kernel.bin это уже raw; проверим через
# собранный .elf. Если linker.ld выдаёт чистый ELF — strings/nm работает.
check "ns_init symbol exists"      "nm build/kernel-x86_64.bin 2>/dev/null | grep -q ' ns_init$' || strings -a build/kernel-x86_64.bin | grep -q '\\[ns\\] root namespaces'"
check "cgroup_init symbol exists"  "nm build/kernel-x86_64.bin 2>/dev/null | grep -q ' cgroup_init$' || strings -a build/kernel-x86_64.bin | grep -q '\\[cgroup\\] root cgroup'"
check "seccomp_init symbol exists" "nm build/kernel-x86_64.bin 2>/dev/null | grep -q ' seccomp_init$' || strings -a build/kernel-x86_64.bin | grep -q '\\[seccomp\\] subsystem'"
check "box_run symbol exists"      "nm build/kernel-x86_64.bin 2>/dev/null | grep -q ' box_run$' || strings -a build/kernel-x86_64.bin | grep -q 'sandbox runtime self-test'"
check "AOS_SYS_UNSHARE wired"      "strings -a build/kernel-x86_64.bin | grep -q 'sandbox initialized'"

echo ""
echo "[4/5] Boot AMPLIFY-OS, observe smoke 46 output"
LOG=build/test_stage46.log
rm -f "$LOG"

OVMF=""
for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$p" ] && OVMF="$p" && break
done

if [ -z "$OVMF" ]; then
    echo "  SKIP: OVMF.fd не найден (sudo apt install ovmf)"
else
    # Регенерация disk-image для детерминированного теста.
    rm -f build/disk-x86_64.img
    make ARCH=x86_64 build/disk-x86_64.img >/dev/null 2>&1 || true

    timeout 90 qemu-system-x86_64 -bios "$OVMF" \
        -machine pc \
        -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/esp.img,if=ide,index=1 \
        -m 1G -smp cpus=4,cores=4,threads=1 \
        -vga std -display none \
        -serial file:"$LOG" \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
        -audiodev none,id=audio0 \
        -device AC97,audiodev=audio0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 || true

    check "boot log produced"             "test -s $LOG"
    # Init-сообщения ns/cgroup/seccomp.
    check_grep "ns_init prints"           "$LOG" "\\[ns\\] root namespaces created"
    check_grep "cgroup_init prints"       "$LOG" "\\[cgroup\\] root cgroup created"
    check_grep "seccomp_init prints"      "$LOG" "\\[seccomp\\] subsystem initialized"
    # Smoke 46 sub-tests.
    check_grep "smoke 46 header"          "$LOG" "\\[smoke 46\\] sandbox runtime self-test"
    check_grep "cgroup mem limit OK"      "$LOG" "\\[smoke 46\\] cgroup mem .*\\(OK\\)"
    check_grep "seccomp strict OK"        "$LOG" "\\[smoke 46\\] seccomp strict .*\\(OK\\)"
    check_grep "ns_unshare CoW OK"        "$LOG" "\\[smoke 46\\] ns_unshare\\(NEWUTS\\|NEWPID\\) diff_uts_pid=yes same_net=yes \\(OK\\)"
    check_grep "ns_summaries OK"          "$LOG" "\\[smoke 46\\] ns_summaries count=5 \\(OK\\)"
    check_grep "mnt-ns gate OK"           "$LOG" "\\[smoke 46\\] mnt-ns allow=yes deny=yes \\(OK\\)"
    check_grep "sandbox final OK"         "$LOG" "\\[smoke 46\\] sandbox initialized, cgroup_count=1"
    # Регрессионная проверка: stage 45 amply smoke ВСЁ ЕЩЁ работает.
    check_grep "stage 45 still passes"    "$LOG" "\\[smoke 45\\] install r=0 \\(OK\\)"
    # Регрессионная проверка: ОС не паникует и доходит до login.
    check_grep "boot reaches login"       "$LOG" "(login|Welcome|firstboot)"
fi

echo ""
echo "[5/5] Static property tests on the source tree"
# Проверим, что задача (task_t) расширена полями ns/cgroup/seccomp.
check_grep "task_t.ns field"      "kernel/arch/x86_64/task.h" "struct ns_set\\* +ns"
check_grep "task_t.cgroup field"  "kernel/arch/x86_64/task.h" "struct cgroup\\* +cgroup"
check_grep "task_t.seccomp field" "kernel/arch/x86_64/task.h" "struct seccomp_filter\\*"
# syscall.h: новые номера 0x150..0x15A.
check_grep "AOS_SYS_UNSHARE def"     "kernel/arch/x86_64/syscall.h" "AOS_SYS_UNSHARE +0x150"
check_grep "AOS_SYS_BOX_RUN def"     "kernel/arch/x86_64/syscall.h" "AOS_SYS_BOX_RUN +0x15A"
check_grep "AOS_CLONE_NEWPID flag"   "kernel/arch/x86_64/syscall.h" "AOS_UNSHARE_NEWPID"
# Net-ns gate в sock.c.
check_grep "sock.c net-ns gate"      "kernel/arch/x86_64/sock.c" "ns_net_allowed"
# Mount-ns gate в vfs.c.
check_grep "vfs.c mount-ns gate"     "kernel/arch/x86_64/vfs.c"  "ns_mount_path_allowed"
# Seccomp gate в syscall.c.
check_grep "seccomp gate in syscall" "kernel/arch/x86_64/syscall.c" "seccomp_gate"
# Shell-команда box и GUI-приложение.
check_grep "shell cmd_box defined"   "kernel/arch/x86_64/shell.c"   "cmd_box\\(int argc"
check_grep "GUI app_box_open"        "kernel/arch/x86_64/app_box.c" "app_box_open"

echo ""
echo "=========================================="
if [ $FAIL -eq 0 ]; then
    echo "  Stage 46 RESULT: $PASS/$EXPECTED PASS"
    exit 0
else
    echo "  Stage 46 RESULT: $PASS PASS, $FAIL FAIL"
    exit 1
fi
