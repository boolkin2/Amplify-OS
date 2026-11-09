#!/bin/bash
# ============================================================================
# test_stage51_0_format.sh — подэтап 51.0: двоичный формат и проверка модулей
# WebAssembly (движок «rune»).
# ----------------------------------------------------------------------------
# Фаза 0 (хост, тот же движок, собранный gcc):
#   официальный набор тестов WebAssembly 2.0 (тег wg-2.0, без SIMD) — все
#   верные модули приняты, все неверные и нераспознаваемые отклонены;
#   настоящие программы на C, собранные clang 19 в wasm32-wasi (три варианта
#   сборки), — приняты, счётчики секций совпадают с wasm-objdump;
#   стойкость: сотни тысяч испорченных модулей под санитайзером адресов — ни
#   одного чтения за границей буфера.
# Фаза 1 (QEMU): [smoke 51]; в терминале — `rune`, `rune selftest`,
#   `rune info` для настоящей программы и для обрезанной, `rune spec` — тот же
#   набор тестов ВНУТРИ ядра, итог обязан совпасть с хостом цифра в цифру.
# Нужны инструменты узла (scripts/pve_wasm_tools.sh): wast2json, clang для
# wasm32-wasi. Без них соответствующие проверки пишут SKIP.
# Запуск: scripts/test_stage51_0_format.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-51_0
WORK=/tmp/amplify-stage51_0
MON_PORT=4660
PACK=build/wasm/spec.pack
APPS=build/wasm/apps

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

if pgrep -f "[q]emu-system.*$WORK/" >/dev/null 2>&1; then
    echo "another run of this test is still alive (QEMU with $WORK) — wait for it or kill it"
    exit 1
fi
mkdir -p "$WORK" "$OUT_DIR" build/wasm
rm -f "$WORK"/*.log "$WORK"/*.img

# ===========================================================================
echo "== Phase 0: the engine on the host — official tests, real programs, fuzzing =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the kernel engine (no warnings)" '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'

HAVE_PACK=0
if command -v wast2json >/dev/null 2>&1 && ls "${WASM_SPEC_DIR:-$HOME/local/share/wasm-spec}"/*.wast >/dev/null 2>&1; then
    bash scripts/wasm_spec_pack.sh >"$WORK/pack.log" 2>&1
    sed 's/^/        /' "$WORK/pack.log" | cut -c1-200
    check "all 90 test files converted and packed"   'grep -q "converted=90 failed=0" "$WORK/pack.log" && [ -s "$PACK" ]'
fi
if [ -s "$PACK" ]; then
    HAVE_PACK=1
    build/wasm_host spec "$PACK" >"$WORK/spec-host.log" 2>&1; RC=$?
    HOST_TOTAL=$(grep -a '^\[wasm-spec\] total:' "$WORK/spec-host.log" | tail -1)
    echo "        ${HOST_TOTAL#\[wasm-spec\] }"
    grep -a 'FAIL' "$WORK/spec-host.log" | head -8 | cut -c1-200 | sed 's/^/        /'
    num() { echo "$HOST_TOTAL" | sed -n "s/.* $1=\([0-9]*\)\/\([0-9]*\).*/\1 \2/p"; }
    read -r M_OK M_ALL <<<"$(num modules)"; read -r I_OK I_ALL <<<"$(num invalid)"; read -r B_OK B_ALL <<<"$(num malformed)"
    check "official tests: every valid module is accepted (${M_OK:-?}/${M_ALL:-?})"        '[ -n "${M_ALL:-}" ] && [ "$M_ALL" -ge 1000 ] && [ "$M_OK" = "$M_ALL" ]'
    check "official tests: every invalid module is rejected (${I_OK:-?}/${I_ALL:-?})"      '[ -n "${I_ALL:-}" ] && [ "$I_ALL" -ge 1000 ] && [ "$I_OK" = "$I_ALL" ]'
    check "official tests: every malformed binary is rejected (${B_OK:-?}/${B_ALL:-?})"    '[ -n "${B_ALL:-}" ] && [ "$B_ALL" -ge 500 ] && [ "$B_OK" = "$B_ALL" ]'
    check "official tests: no failures, exit code 0"         '[ $RC = 0 ] && echo "$HOST_TOTAL" | grep -q "failures=0"'
else
    skip "official test pack: no $PACK and no wast2json to build it (scripts/pve_wasm_tools.sh)"
fi

HAVE_APPS=0
if make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1; then
    HAVE_APPS=1
    tail -1 "$WORK/apps.log" | sed 's/\x1b\[[0-9;]*m//g; s/^/        /'
    N_APPS=$(ls "$APPS"/*.wasm | wc -l)
    GOOD=0
    for w in "$APPS"/*.wasm; do build/wasm_host info "$w" 1 >/dev/null 2>&1 && GOOD=$((GOOD+1)); done
    check "real C programs built by clang 19 are accepted ($GOOD/$N_APPS: -O0, -O2, -O2 with all 2.0 features)" '[ "$N_APPS" -ge 6 ] && [ "$GOOD" = "$N_APPS" ]'
    # Наши счётчики секций против wasm-objdump — независимого разборщика.
    SAME=0
    for w in "$APPS"/*.wasm; do
        OURS=$(build/wasm_host info "$w" 0 | head -1)
        OBJ=$(wasm-objdump -h "$w" 2>/dev/null)
        # Секции, которой в модуле нет, у wasm-objdump нет и в списке: это ноль.
        cnt() { local v; v=$(echo "$OBJ" | sed -n "s/^ *$1 .*count: \([0-9]*\).*/\1/p" | head -1); echo "${v:-0}"; }
        ours() { echo "$OURS" | sed -n "s/.* $1=\([0-9]*\).*/\1/p"; }
        if [ "$(ours types)" = "$(cnt Type)" ] && [ "$(ours funcs)" = "$(cnt Function)" ] &&
           [ "$(ours imports)" = "$(cnt Import)" ] && [ "$(ours exports)" = "$(cnt Export)" ] &&
           [ "$(ours datas)" = "$(cnt Data)" ] && [ "$(ours globals)" = "$(cnt Global)" ]; then SAME=$((SAME+1))
        else echo "        differs: $(basename "$w"): $OURS"; fi
    done
    check "section counts equal wasm-objdump's for every program ($SAME/$N_APPS)" '[ "$SAME" = "$N_APPS" ]'
    build/wasm_host info "$APPS/hello.wasm" 8 | sed 's/^/        /' | cut -c1-150 | head -14
    check "hello.wasm: WASI imports and _start are seen" 'build/wasm_host info "$APPS/hello.wasm" 64 | grep -q "import func wasi_snapshot_preview1.fd_write (i32, i32, i32, i32) -> (i32)" && build/wasm_host info "$APPS/hello.wasm" 64 | grep -q "export func _start () -> ()"'
    check "calc-feat.wasm really uses the 2.0 extensions (bulk memory)" 'wasm-objdump -d "$APPS/calc-feat.wasm" 2>/dev/null | grep -q -E "memory\.(copy|fill)"'
else
    skip "real programs: no wasm32-wasi clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
fi

# Стойкость к испорченным модулям. Под санитайзером любое чтение за границей
# буфера или неопределённое поведение останавливает утилиту.
FUZZ_BIN=build/wasm_host; FUZZ_KIND="plain build"
if make -f Makefile.x86_64 wasm-host-asan >"$WORK/asan-cc.log" 2>&1 && [ -x build/wasm_host_asan ]; then
    FUZZ_BIN=build/wasm_host_asan; FUZZ_KIND="AddressSanitizer + UBSan"
fi
SEED_MOD="$WORK/seed.wasm"
if [ $HAVE_APPS = 1 ]; then cp "$APPS/calc-feat.wasm" "$SEED_MOD"; N_FUZZ=40000
else printf '\0asm\1\0\0\0\1\7\1\140\2\177\177\1\177\3\2\1\0\5\3\1\0\1\7\15\2\3add\0\0\3mem\2\0\12\11\1\7\0\40\0\40\1\152\13' > "$SEED_MOD"; N_FUZZ=400000; fi
"$FUZZ_BIN" fuzz "$SEED_MOD" $N_FUZZ 12345 >"$WORK/fuzz.log" 2>&1; RC=$?
tail -1 "$WORK/fuzz.log" | cut -c1-200 | sed 's/^/        /'
check "fuzzing: $N_FUZZ damaged modules, no crash, no out-of-bounds read ($FUZZ_KIND)" '[ $RC = 0 ] && grep -q "^fuzz: $N_FUZZ mutants" "$WORK/fuzz.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz.log"'
check "fuzzing: damaged modules are mostly rejected, some stay valid" 'grep -q -E "accepted=[1-9][0-9]* malformed=[1-9][0-9]* invalid=[1-9]" "$WORK/fuzz.log"'
[ "$FUZZ_KIND" = "plain build" ] && skip "sanitizer build is not available here (no libasan): fuzzing ran without it"

# ===========================================================================
echo "== Phase 1: the same engine inside the kernel =="
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; true
}
trap cleanup EXIT
mon_connect() {
    for _ in $(seq 1 100); do
        if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
            sleep 0.3; read -t 0.3 -r -u 3 _ || true; read -t 0.3 -r -u 3 _ || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}
mon()   { echo "$1" >&3; sleep 0.05; while read -t 0.02 -r -u 3 _; do :; done; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '.') echo dot;; '-') echo minus;; '=') echo equal;; ',') echo comma;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.09; done; }
os_cmd() { type_text "$1"; key ret "${2:-1}"; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}

KVM=""
[ -r /dev/kvm ] && [ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
LOG="$WORK/serial.log"
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
mmd -D s -i "$WORK/dev.img" ::/nexus </dev/null 2>/dev/null
[ $HAVE_PACK = 1 ] && mcopy -D o -i "$WORK/dev.img" "$PACK" ::/nexus/spec.pack </dev/null
if [ $HAVE_APPS = 1 ]; then
    mcopy -D o -i "$WORK/dev.img" "$APPS/hello.wasm" ::/nexus/hello.wasm </dev/null
    mcopy -D o -i "$WORK/dev.img" "$APPS/calc-feat.wasm" ::/nexus/calc.wasm </dev/null
    # Тот же модуль без последних 100 байт — нераспознаваемый.
    head -c $(( $(stat -c %s "$APPS/hello.wasm") - 100 )) "$APPS/hello.wasm" > "$WORK/cut.wasm"
    mcopy -D o -i "$WORK/dev.img" "$WORK/cut.wasm" ::/nexus/cut.wasm </dev/null
fi
# 2 ГБ: с 51.1 `rune spec` ещё и ИСПОЛНЯЕТ тесты, один из них растит память
# модуля до 52 МБ (на время роста нужны оба куска — старый и новый).
qemu-system-x86_64 -bios "$OVMF" ${KVM:--cpu max} -machine pc -m 2G \
    -smp cpus=4,cores=4,threads=1 \
    -vga none -device virtio-vga,xres=1280,yres=720 \
    -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }

check "[smoke 51] block is printed at boot"          'wait_log "$LOG" "[smoke 51] WebAssembly engine" 120'
wait_log "$LOG" "[smoke 51] done in" 20
grep -a '^\[smoke 51\]' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "[smoke 51] builtin modules decode and validate" 'grep -a -q "\[smoke 51\] module add.*: PASS" "$LOG" && grep -a -q "\[smoke 51\] module sum.*: PASS" "$LOG"'
check "[smoke 51] broken modules rejected with the right reason (5 of 5)" 'grep -a -q "\[smoke 51\] broken modules are rejected with the right reason: PASS (5 of 5" "$LOG"'
check "[smoke 51] module memory returns to the heap"  'grep -a -q "\[smoke 51\] 200 load/free cycles leave the kernel heap unchanged: PASS" "$LOG"'
MS=$(grep -a '\[smoke 51\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
check "[smoke 51] PASS and cheap at boot (${MS:-?} ms < 100)" 'grep -a -q "\[smoke 51\] done in .* ms: PASS" "$LOG" && [ "${MS:-999}" -lt 100 ]'

wait_log "$LOG" "[smoke 37.6c-1] RTC" 120 || true
sleep 2
key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "rune" 1
check "rune: status in the terminal"                 'wait_log "$LOG" "[rune] status engine=1 stage=51." 10'
os_cmd "wasm selftest" 1
check "wasm is an alias of rune; selftest passes"    'wait_log "$LOG" "[rune] selftest: " 10 && grep -a -q "\[rune\] selftest: [0-9]* passed, 0 failed" "$LOG"'
if [ $HAVE_APPS = 1 ]; then
    os_cmd "rune info /disk/nexus/hello.wasm" 2
    check "rune info: a real C program from the disk is valid" 'wait_log "$LOG" "[rune] info /disk/nexus/hello.wasm: valid module" 15'
    grep -a '^\[rune\]   ' "$LOG" | tr -d '\r' | cut -c1-140 | head -5 | sed 's/^/        /'
    HOSTLINE=$(build/wasm_host info "$APPS/hello.wasm" 0 | head -1)
    KLINE=$(grep -a '^\[rune\]   size=' "$LOG" | head -1 | tr -d '\r' | sed 's/^\[rune\]   //')
    check "rune info: the kernel reports the same counters as the host build" '[ -n "$KLINE" ] && [ "$KLINE" = "$HOSTLINE" ]'
    check "rune info: WASI imports are listed"       'grep -a -q "\[rune\]   import func wasi_snapshot_preview1.fd_write (i32, i32, i32, i32) -> (i32)" "$LOG"'
    os_cmd "rune info /disk/nexus/cut.wasm" 2
    check "rune info: a truncated module is rejected as malformed" 'wait_log "$LOG" "[rune] info /disk/nexus/cut.wasm: rejected - malformed" 15'
    os_cmd "rune info /disk/nexus/calc.wasm" 2
    check "rune info: a program using the 2.0 extensions is valid" 'wait_log "$LOG" "[rune] info /disk/nexus/calc.wasm: valid module" 15'
else
    skip "rune info on real programs (no clang on this machine)"
fi
os_cmd "rune info /disk/nexus/nothing.wasm" 1.5
check "rune info: a missing file is reported"        'wait_log "$LOG" "[rune] info /disk/nexus/nothing.wasm: file not found" 10'
if [ $HAVE_PACK = 1 ]; then
    os_cmd "rune spec /disk/nexus/spec.pack" 3
    check "rune spec: the official tests ran inside the kernel" 'wait_log "$LOG" "[rune] spec: " 120'
    KTOTAL=$(grep -a '^\[wasm-spec\] total:' "$LOG" | tail -1 | tr -d '\r')
    grep -a '^\[rune\] spec: ' "$LOG" | tr -d '\r' | cut -c1-120 | sed 's/^/        /'
    check "rune spec: totals are identical to the host run" '[ -n "$KTOTAL" ] && [ "$KTOTAL" = "$HOST_TOTAL" ]'
    check "rune spec: PASS, kernel heap unchanged"   'grep -a -q "\[rune\] spec: PASS in .* kernel heap unchanged" "$LOG"'
else
    skip "rune spec in the kernel (no test pack)"
fi
mon "screendump $PWD/$OUT_DIR/terminal.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial.log"
mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; QEMU_PID=""

echo "== stage 51.0 format: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
