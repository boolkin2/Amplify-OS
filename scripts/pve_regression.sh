#!/usr/bin/env bash
# ============================================================================
# pve_regression.sh — регрессия на узле Proxmox (запускать НА УЗЛЕ, в фоне):
#   nohup bash scripts/pve_regression.sh > build/reg/run.log 2>&1 &
# Итог — build/reg/summary.txt (по строке на тест + все [FAIL]), маркер
# конца — строка `=== all done ===`. Логи тестов — build/reg/<тест>.log.
#
# Порядок: test_stage45/41/42 первыми и по одному — они сами зовут `make` и
# могут пересобрать образы, которые другие тесты в этот момент копируют. Дальше две
# дорожки параллельно (у тестов разные порты монитора и рабочие каталоги):
#   A: test_stage49_qwen3 (самый долгий);
#   B: 57_installer → 57_ahci → 48_fde → 49_mind (49_mind последним: он, как и
#      qwen3, собирает build/mind_host — к этому времени фаза 0 qwen3 позади).
# Без доступа к /dev/kvm тесты идут под TCG; AOS_SLOW растягивает таймауты
# (узел медленнее ПК в одном потоке).
# ============================================================================
[ -f ~/local/env.sh ] && . ~/local/env.sh
cd "$(dirname "$0")/.."
mkdir -p build/reg
SUM=build/reg/summary.txt
: > "$SUM"
export AOS_SLOW="${AOS_SLOW:-14}"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then echo "accel: KVM" >> "$SUM"; else echo "accel: TCG (ai not in kvm group), AOS_SLOW=$AOS_SLOW" >> "$SUM"; fi

run() {
    local t="$1" start rc
    start=$(date +%s)
    bash "scripts/$t.sh" > "build/reg/$t.log" 2>&1
    rc=$?
    {
        echo "$t rc=$rc $(( $(date +%s) - start ))s PASS=$(grep -cE '\[PASS\]|^  PASS |: OK$|^\[OK\]' "build/reg/$t.log") FAIL=$(grep -cE '\[FAIL\]|^  FAIL ' "build/reg/$t.log"): $(grep -aE '^== |passed|failed' "build/reg/$t.log" | tail -1)"
        grep -aE '\[FAIL\]|^  FAIL ' "build/reg/$t.log" | sed 's/^/    /'
    } >> "$SUM"
}

# Тот же тест с целевым диском на virtio-blk; лог — <тест>-virtio.log.
runv() {
    local t="$1" start rc
    start=$(date +%s)
    AOS_DISK_BUS=virtio bash "scripts/$t.sh" > "build/reg/$t-virtio.log" 2>&1
    rc=$?
    {
        echo "$t (virtio-blk) rc=$rc $(( $(date +%s) - start ))s PASS=$(grep -cE '\[PASS\]' "build/reg/$t-virtio.log") FAIL=$(grep -cE '\[FAIL\]' "build/reg/$t-virtio.log"): $(grep -aE '^== ' "build/reg/$t-virtio.log" | tail -1)"
        grep -aE '\[FAIL\]' "build/reg/$t-virtio.log" | sed 's/^/    /'
    } >> "$SUM"
}

run test_stage45_amply
run test_stage41_gpu
run test_stage42_compositor
# 45/41/42 запускают ОС ПРЯМО на dev-образе и гасят QEMU через kill -9. Если
# гость в этот миг создавал файл (первый запуск пишет темы в /Sanctum), FAT
# остаётся несогласованной: запись каталога есть, цепочки кластеров нет.
# Дальше test_stage49_qwen3 читает образ целиком (`mcopy -s`), mtools падает
# («Fat problem while decoding»), диск с моделью не собирается — и 20 проверок
# MIND проваливаются, хотя ядро ни при чём (так было 2026-10-02: загрузка
# стала на 3 с дольше, и kill попал в запись). Остальным — чистый образ.
rm -f build/disk-x86_64.img
make ARCH=x86_64 build/disk-x86_64.img >/dev/null 2>&1 || true
( run test_stage49_qwen3 ) &
( run test_stage57_installer; run test_stage57_ahci; run test_stage48_fde; run test_stage49_mind; run test_stage41_2_ui; run test_stage41_2_input; run test_stage41_2_net; run test_stage48_2_net;
  run test_stage50_0_probe; run test_stage50_1_vmx; run test_stage50_2_linux; run test_stage50_3_bios; run test_stage50_4_alpine; run test_stage50_5_app; run test_stage51_0_format; run test_stage51_1_interp; run test_stage51_2_wasi; run test_stage51_3_core; runv test_stage57_installer; runv test_stage48_fde ) &
wait
echo "=== all done ===" >> "$SUM"
