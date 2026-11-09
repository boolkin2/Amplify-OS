#!/usr/bin/env bash
# ============================================================================
# pve_full38.sh — «полная регрессия» подэтапа 38.1: всё из pve_regression.sh и
# следом тесты вне её, которые задевает перенос ядра (pve_extra38.sh — там же
# объяснено, какие и почему). Запускается на узле через pve_test.sh.
# Итог — build/reg/full38.txt (обе сводки подряд).
#   AOS_FULL38_TAG — пометка в первой строке (какая сборка проверялась).
# ============================================================================
[ -f ~/local/env.sh ] && . ~/local/env.sh
cd "$(dirname "$0")/.."
mkdir -p build/reg
OUT=build/reg/full38.txt
echo "# full38 ${AOS_FULL38_TAG:-} $(date '+%F %T') image_end=0x$(nm build/kernel-x86_64.bin | awk '$3 == "_kernel_end" { print $1 }')" > "$OUT"
bash scripts/pve_regression.sh > build/reg/full38-regression.out 2>&1
cat build/reg/summary.txt >> "$OUT"
bash scripts/pve_extra38.sh > build/reg/full38-extra.out 2>&1
cat build/reg/extra-summary.txt >> "$OUT"
echo "=== full38 done ===" >> "$OUT"
cat "$OUT"
