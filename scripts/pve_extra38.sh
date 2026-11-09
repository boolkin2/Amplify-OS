#!/usr/bin/env bash
# ============================================================================
# pve_extra38.sh — тесты ВНЕ pve_regression.sh, которые задевает подэтап 38.1
# (ядро в верхней половине адресов). Запускается на узле (через pve_test.sh).
# ----------------------------------------------------------------------------
# Почему они: правка 38.1 меняет то, как ядро ходит в память пользовательских
# процессов и что отдаёт устройствам как адрес DMA, а регрессия этого почти
# не касается:
#   40_libc      — пользовательские ELF, libc, динамическая загрузка, syscall;
#                  грузится с ISO через GRUB (ветка BIOS);
#   46_sandbox   — песочницы: процессы, shm, пространства имён;
#   47_crypto    — самотесты криптографии (контексты TLS уезжают из .bss);
#   47_send_recv — TLS поверх rtl8139 (DMA из кучи) с сервером на хосте;
#   uefi_boot    — голая загрузка через наш BOOTX64.EFI;
#   e1000_qemu   — запасная карта e1000 (кольца DMA из кучи);
#   51_4, 51_5   — окно Rune и JIT (в pve_regression их нет);
#   38_1_reloc   — сам тест 38.1.1 (разные физические адреса, BIOS, откат).
# Не входят (ядро не запускают или дублируются): test_stage_*_static,
# test_mouse_*, test_c5_splash_bash, 47_internet/e1000_https (нужен интернет,
# то же проверяют 41_2_net и 48_2_net), 47_live/modern/ecdsa (тот же путь,
# что send_recv, с другим набором шифров).
#
#   AOS_EXTRA="a b c" — свой список; итог — build/reg/extra-summary.txt
# ============================================================================
[ -f ~/local/env.sh ] && . ~/local/env.sh
cd "$(dirname "$0")/.."
mkdir -p build/reg
SUM=build/reg/extra-summary.txt
: > "$SUM"
LIST="${AOS_EXTRA:-test_stage40_libc test_stage46_sandbox test_stage47_crypto test_stage47_send_recv test_uefi_boot test_e1000_qemu test_stage51_4_app test_stage51_5_jit test_stage38_1_reloc}"
for t in $LIST; do
    start=$(date +%s)
    # Старые тесты не всегда ограничивают себя сами — предел снаружи.
    timeout 1500 bash "scripts/$t.sh" > "build/reg/x-$t.log" 2>&1
    rc=$?
    {
        echo "$t rc=$rc $(( $(date +%s) - start ))s PASS=$(grep -acE '\[PASS\]|^  PASS |: OK$|^\[OK\]|\[ok\]' "build/reg/x-$t.log") FAIL=$(grep -acE '\[FAIL\]|^  FAIL |\[fail\]' "build/reg/x-$t.log"): $(grep -aE '^== |passed|failed|PASS: |ФИНАЛ|RESULT' "build/reg/x-$t.log" | tail -1)"
        grep -aE '\[FAIL\]|^  FAIL |\[fail\]' "build/reg/x-$t.log" | head -12 | sed 's/^/    /'
    } >> "$SUM"
    # Не оставлять чужой QEMU следующему тесту (общие порты монитора).
    pkill -9 -u "$(id -u)" -f '[q]emu-system-x86_64' 2>/dev/null
    sleep 1
done
echo "=== all done ===" >> "$SUM"
cat "$SUM"
