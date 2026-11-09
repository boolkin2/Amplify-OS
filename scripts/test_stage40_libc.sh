#!/bin/bash
# ============================================================================
# test_stage40_libc.sh — автотест завершения этапа 40 (libc finalize).
# ----------------------------------------------------------------------------
# Что проверяет:
#   1. Сборка: make ARCH=x86_64 создаёт libc.a, libc.so, crt0.o, все 6
#      миграционных stage40_*.elf + hello_libc.elf через sysroot.
#   2. Артефакты: libc.so — корректный ELF64 shared object с PT_DYNAMIC.
#   3. sysroot layout корректный.
#   4. QEMU: система загружается до firstboot (core init не сломался).
#   5. initrd: содержит все ожидаемые файлы.
#
# Сложные сценарии (шелл-взаимодействие, запуск launch hello_libc.elf через
# клавиатурный ввод) — в отдельных test-case'ах под python3; на 40-м
# используем bash-вариант для базовой верификации.
# ============================================================================
set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

check() {
    local name="$1"
    local cmd="$2"
    printf "  %-55s ... " "$name"
    if eval "$cmd" >/dev/null 2>&1; then
        echo "OK"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================================"
echo "  AMPLIFY-OS Stage 40 (libc finalize) — автотест"
echo "============================================================"

# ---- 1. Сборка ---------------------------------------------------------
echo "[1] Сборка через make ARCH=x86_64"
if make ARCH=x86_64 >/tmp/s40-build.log 2>&1; then
    echo "  build: OK"
    PASS=$((PASS + 1))
else
    echo "  build: FAIL (см. /tmp/s40-build.log)"
    tail -20 /tmp/s40-build.log
    FAIL=$((FAIL + 1))
fi

# ---- 2. Артефакты libc -------------------------------------------------
echo "[2] Артефакты libc и sysroot"
check "libc.a существует" "[ -f build/sysroot/usr/lib/libc.a ]"
check "libc.so существует" "[ -f build/sysroot/usr/lib/libc.so ]"
check "crt0.o существует"  "[ -f build/sysroot/usr/lib/crt0.o ]"
check "libc.so — ELF64 shared" \
    "file build/sysroot/usr/lib/libc.so | grep -q 'ELF 64-bit LSB shared object'"
check "libc.so имеет PT_DYNAMIC" \
    "readelf -d build/sysroot/usr/lib/libc.so | grep -q SONAME"
check "libc.so экспортирует printf" \
    "nm build/sysroot/usr/lib/libc.so 2>/dev/null | grep -q 'T printf'"
check "libc.so экспортирует malloc" \
    "nm build/sysroot/usr/lib/libc.so 2>/dev/null | grep -q 'T malloc'"
check "libc.so экспортирует pthread_create" \
    "nm build/sysroot/usr/lib/libc.so 2>/dev/null | grep -q 'T pthread_create'"
check "libc.so экспортирует socket" \
    "nm build/sysroot/usr/lib/libc.so 2>/dev/null | grep -q 'T socket'"
check "libc.so экспортирует stat" \
    "nm build/sysroot/usr/lib/libc.so 2>/dev/null | grep -q 'T stat'"

# ---- 3. Userspace миграция --------------------------------------------
echo "[3] 6 мигрированных userspace-программ"
for p in stage40_hello stage40_elftest stage40_forktest stage40_exectest stage40_cwdtest stage40_shmtest; do
    check "$p.elf собран" "[ -f build/x86_64/userspace/$p.elf ]"
    check "$p.elf — валидный ELF64" \
        "file build/x86_64/userspace/$p.elf | grep -q 'ELF 64-bit LSB executable'"
done

# ---- 4. initrd содержимое ---------------------------------------------
echo "[4] initrd"
check "initrd-x86_64.img существует" "[ -f build/initrd-x86_64.img ]"
check "initrd содержит libc.so" \
    "strings build/initrd-x86_64.img | grep -q '^libc.so\$'"
check "initrd содержит stage40_hello.elf" \
    "strings build/initrd-x86_64.img | grep -q 'stage40_hello.elf'"
check "initrd содержит hello_libc.elf" \
    "strings build/initrd-x86_64.img | grep -q 'hello_libc.elf'"

# ---- 5. Smoke-тест QEMU (core init) -----------------------------------
echo "[5] Smoke QEMU boot"
rm -f /tmp/s40-serial.log
timeout 12 qemu-system-x86_64 \
    -cdrom build/myos-x86_64.iso -boot d -m 256M \
    -vga std -serial file:/tmp/s40-serial.log \
    -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -display none 2>/dev/null || true

check "QEMU: [syscall] SYSCALL/SYSRET enabled" \
    "grep -q 'SYSCALL/SYSRET enabled' /tmp/s40-serial.log"
check "QEMU: [doublebuf] initialized: 1024x768" \
    "grep -q 'doublebuf.*initialized' /tmp/s40-serial.log"
# [38.1] Тест грузит ISO, а с этапа 57 ISO — установочный носитель: после
# ядра идёт установщик, а не firstboot. Раньше проверка ждала firstboot и
# падала на исправной системе.
check "QEMU: дошли до установщика (live ISO) или firstboot" \
    "grep -qE 'firstboot.*starting|\[installer\] live mode' /tmp/s40-serial.log"
check "QEMU: panic не произошёл" \
    "! grep -q 'AMPLIFY-OS PANIC' /tmp/s40-serial.log"

# ---- 6. Результат -----------------------------------------------------
echo "============================================================"
echo "  Всего: $((PASS + FAIL)) | PASS: $PASS | FAIL: $FAIL"
echo "============================================================"

if [ $FAIL -gt 0 ]; then
    echo "[test] ФИНАЛ: есть провалы — stage 40 не завершён"
    exit 1
fi
echo "[test] ФИНАЛ: все проверки прошли — stage 40 готов"
exit 0
