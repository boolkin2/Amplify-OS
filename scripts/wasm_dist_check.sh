#!/bin/bash
# ============================================================================
# wasm_dist_check.sh — НА УЗЛЕ: собрать встроенные программы Rune и сверить
# каждую с тем же исходником под gcc (хост-сборка движка, тот же wasm_wasi.c,
# что в ядре). Зовётся из scripts/pve_wasm_dist.sh через pve_run.sh; можно
# повторять. Код возврата 0 — все сверки сошлись.
# ============================================================================
. ~/local/env.sh
cd "${PVE_WORK:-/tmp/ai-work}/OS2" || exit 1
make -f Makefile.x86_64 wasm-host 2>&1 | grep -E 'error|warning' | head
make -f Makefile.x86_64 wasm-apps 2>&1 | grep -E 'error|warning' | head -20
make -f Makefile.x86_64 wasm-dist 2>&1 | grep -E 'error|warning|dist' | head -20
A=build/wasm/apps
T=/tmp/ai-work/dist-try; rm -rf $T; mkdir -p $T
H=$PWD/build/wasm_host
bad=0
# имя, вариант сборки, ввод (строка для printf), аргументы…
cmp_run() {
    local n=$1 v=$2 in=$3; shift 3
    rm -rf $T/n $T/w; mkdir -p $T/n $T/w
    printf 'alpha beta\ngamma\n' > $T/n/poem.txt; cp $T/n/poem.txt $T/w/
    ( cd $T/n && printf "$in" | $OLDPWD/$A/$n.host "$@" ) 2>&1 | grep -v '^время: ' > $T/ref.txt; local rn=${PIPESTATUS[0]}
    printf "$in" | $H run -d $T/w $A/$n$v.wasm "$@" 2>&1 | grep -v '^время: ' > $T/got.txt; local rw=${PIPESTATUS[1]}
    if cmp -s $T/ref.txt $T/got.txt; then echo "same: $n$v $* ($(wc -l < $T/got.txt) lines)"
    else echo "DIFF: $n$v $*"; diff $T/ref.txt $T/got.txt | head -12; bad=1; fi
}
for n in oracle abacus sieve scribe; do cp userspace/wasm/dist/$n.wasm $A/$n-dist.wasm; done
for v in "" -O0 -feat -dist; do
    cmp_run oracle "$v" '50\n75\nabc\n88\n94\n97\n99\n' 2024
    cmp_run oracle "$v" '10\n' 7
    cmp_run abacus "$v" '(2 + 3) * 4\n10 / 0\n7 %% 3\n-(5 - 12) * 1000000007\n2 +\nq\n'
    cmp_run abacus "$v" '' '9223372036854775807' '+' '1'
    cmp_run sieve "$v" '' 100000
    cmp_run scribe "$v" ''
    cmp_run scribe "$v" '' poem.txt
    cmp_run scribe "$v" '' note first note
    cmp_run scribe "$v" '' nothing.txt
done
# Строка описания — в пользовательской секции каждого модуля.
for n in oracle abacus sieve scribe; do
    if wasm-objdump -h userspace/wasm/dist/$n.wasm | grep -q 'rune.about'; then echo "about: $n ok"
    else echo "about: $n MISSING"; bad=1; fi
done
ls -l userspace/wasm/dist/*.wasm | awk '{print $5, $9}'
sha256sum userspace/wasm/dist/*.wasm | cut -c1-16,65-
exit $bad
