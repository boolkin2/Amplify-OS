#!/bin/bash
# ============================================================================
# wasm_spec_pack.sh — собрать пакет официальных тестов WebAssembly (этап 51).
# ----------------------------------------------------------------------------
# Берёт *.wast из каталога набора тестов (на узле — ~/local/share/wasm-spec,
# тег wg-2.0; см. scripts/pve_wasm_tools.sh), прогоняет каждый через
# `wast2json` (wabt) и склеивает результат в build/wasm/spec.pack — его читают
# build/wasm_host и ядро (`rune spec`). Файлы simd_* пропускаются: SIMD у
# движка нет, и это оговорено (CLAUDE.md §8).
# Нужны wast2json и python3 — то есть узел; на ПК их нет (пакет оттуда можно
# просто скопировать).
#   bash scripts/wasm_spec_pack.sh [каталог с .wast]
# ============================================================================
set -u
cd "$(dirname "$0")/.."
SPEC="${1:-${WASM_SPEC_DIR:-$HOME/local/share/wasm-spec}}"
OUT=build/wasm
W=$OUT/spec-json

command -v wast2json >/dev/null 2>&1 || { echo "wast2json not found (run scripts/pve_wasm_tools.sh on the node)"; exit 2; }
ls "$SPEC"/*.wast >/dev/null 2>&1 || { echo "no .wast files in $SPEC"; exit 2; }
rm -rf "$W"; mkdir -p "$W"

ok=0; failed=0; simd=0
for f in "$SPEC"/*.wast; do
    n=$(basename "$f" .wast)
    case "$n" in simd_*) simd=$((simd+1)); continue;; esac
    if wast2json "$f" -o "$W/$n.json" >/dev/null 2>"$W/$n.err"; then
        ok=$((ok+1))
    else
        failed=$((failed+1))
        echo "wast2json could not convert $n.wast: $(head -1 "$W/$n.err" | cut -c1-120)"
        rm -f "$W/$n.json"
    fi
done
echo "wast files: converted=$ok failed=$failed simd-skipped=$simd (tests: $(cat "$SPEC/ref.txt" 2>/dev/null))"
python3 tools/wasm_pack_spec.py "$OUT/spec.pack" "$W" || exit 1
