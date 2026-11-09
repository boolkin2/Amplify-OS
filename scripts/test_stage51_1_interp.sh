#!/bin/bash
# ============================================================================
# test_stage51_1_interp.sh — подэтап 51.1: интерпретатор WebAssembly («rune»).
# ----------------------------------------------------------------------------
# Фаза 0 (хост, тот же движок, собранный gcc):
#   официальный набор тестов WebAssembly 2.0 (без SIMD) — теперь с исполнением:
#   каждый вызов возвращает то, что требует тест, каждая ошибка счёта
#   останавливает программу названной причиной, модули связываются друг с
#   другом как положено; проверка самой проверки — три намеренно испорченных
#   интерпретатора обязаны набор провалить;
#   настоящий код: userspace/wasm/pure/pure.c, собранный clang 19 в трёх
#   вариантах, против того же файла, собранного gcc под хост, — 18 функций,
#   результаты совпадают (дробные — по битам);
#   стойкость: модули набора портятся случайным образом, и те, что прошли
#   проверку, ИСПОЛНЯЮТСЯ под санитайзером адресов.
# Фаза 1 (QEMU, 2 ГБ): [smoke 51.1]; `rune selftest`; `rune spec` — весь набор
#   с исполнением ВНУТРИ ядра, итог цифра в цифру как на хосте; `rune call` —
#   те же 18 функций; ошибки счёта; предел времени останавливает вечный цикл;
#   программа с импортами WASI честно не запускается (окружение — 51.2).
# Нужны инструменты узла (scripts/pve_wasm_tools.sh). Без них — SKIP.
# Запуск: scripts/test_stage51_1_interp.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-51_1
WORK=/tmp/amplify-stage51_1
MON_PORT=4661
PACK=build/wasm/spec.pack
APPS=build/wasm/apps
E=kernel/arch/x86_64

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
rm -rf "$WORK"/*.log "$WORK"/*.img "$WORK/mut"

# ===========================================================================
echo "== Phase 0: the interpreter on the host — official tests, real code, fuzzing =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the kernel engine (no warnings)" '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'

HAVE_PACK=0
if [ ! -s "$PACK" ] && command -v wast2json >/dev/null 2>&1; then
    bash scripts/wasm_spec_pack.sh >"$WORK/pack.log" 2>&1
    tail -2 "$WORK/pack.log" | sed 's/^/        /' | cut -c1-200
fi
HOST_TOTAL=""; HOST_EXEC=""
if [ -s "$PACK" ]; then
    HAVE_PACK=1
    T0=$(date +%s%N)
    build/wasm_host spec "$PACK" >"$WORK/spec-host.log" 2>&1; RC=$?
    SPEC_MS=$(( ($(date +%s%N) - T0) / 1000000 ))
    HOST_TOTAL=$(grep -a '^\[wasm-spec\] total:' "$WORK/spec-host.log" | tail -1)
    HOST_EXEC=$(grep -a '^\[wasm-spec\] exec:' "$WORK/spec-host.log" | tail -1)
    echo "        ${HOST_TOTAL#\[wasm-spec\] }"
    echo "        ${HOST_EXEC#\[wasm-spec\] }  (${SPEC_MS} ms)"
    grep -a 'FAIL' "$WORK/spec-host.log" | head -8 | cut -c1-220 | sed 's/^/        /'
    num() { echo "$HOST_EXEC" | sed -n "s/.* $1=\([0-9]*\)\/\([0-9]*\).*/\1 \2/p"; }
    read -r R_OK R_ALL <<<"$(num returns)"; read -r T_OK T_ALL <<<"$(num traps)"; read -r X_OK X_ALL <<<"$(num exhaustion)"
    read -r L_OK L_ALL <<<"$(num unlinkable)"; read -r U_OK U_ALL <<<"$(num uninstantiable)"; read -r A_OK A_ALL <<<"$(num actions)"
    check "official tests: every call returns the required values (${R_OK:-?}/${R_ALL:-?})"   '[ -n "${R_ALL:-}" ] && [ "$R_ALL" -ge 20000 ] && [ "$R_OK" = "$R_ALL" ]'
    check "official tests: every trap happens, with the required reason (${T_OK:-?}/${T_ALL:-?})" '[ -n "${T_ALL:-}" ] && [ "$T_ALL" -ge 2000 ] && [ "$T_OK" = "$T_ALL" ]'
    check "official tests: runaway recursion ends as 'call stack exhausted' (${X_OK:-?}/${X_ALL:-?})" '[ -n "${X_ALL:-}" ] && [ "$X_ALL" -ge 10 ] && [ "$X_OK" = "$X_ALL" ]'
    check "official tests: linking — bad imports refused (${L_OK:-?}/${L_ALL:-?}), failing start reported (${U_OK:-?}/${U_ALL:-?})" \
          '[ -n "${L_ALL:-}" ] && [ "$L_ALL" -ge 50 ] && [ "$L_OK" = "$L_ALL" ] && [ "${U_ALL:-0}" -ge 20 ] && [ "$U_OK" = "$U_ALL" ] && [ "$A_OK" = "$A_ALL" ]'
    check "official tests: nothing skipped, no failures, exit code 0" '[ $RC = 0 ] && echo "$HOST_TOTAL" | grep -q "exec-skipped=0 failures=0"'

    # Проверка самой проверки: если интерпретатор намеренно сломать, набор
    # обязан это заметить. Исходники не трогаются — порченая копия в $WORK.
    mkdir -p "$WORK/mut"
    mutant() {
        cp "$E/wasm_interp.c" "$WORK/mut/wasm_interp.c"
        sed -i "$1" "$WORK/mut/wasm_interp.c"
        cmp -s "$E/wasm_interp.c" "$WORK/mut/wasm_interp.c" && { echo "pattern-not-found"; return; }
        # Список файлов движка — тот же, что WASM_ENGINE в Makefile, только
        # wasm_interp.c свой: добавил файл в движок — добавь и сюда.
        gcc -O2 -I"$E" -o "$WORK/mut/h" tools/wasm_host.c "$E/wasm_bin.c" "$E/wasm_valid.c" "$E/wasm_rt.c" \
            "$WORK/mut/wasm_interp.c" "$E/wasm_jit.c" "$E/wasm_wasi.c" "$E/wasm_spec.c" 2>/dev/null || { echo "no-build"; return; }
        WASM_JIT=0 timeout 120 "$WORK/mut/h" spec "$PACK" 2>/dev/null | sed -n 's/.*total:.*failures=\([0-9]*\).*/\1/p' | tail -1
    }
    M1=$(mutant 's/BIN((uint32_t)(A32 + B32))/BIN((uint32_t)(A32 - B32))/')
    M2=$(mutant 's/if (ea + (n) > mem_bytes) TRAP(WASM_TRAP_MEM_OOB)/if (ea >= mem_bytes) TRAP(WASM_TRAP_MEM_OOB)/')
    M3=$(mutant 's/if (callee->canon != inst->canon\[IR_B(w)\]) TRAP(WASM_TRAP_SIG_MISMATCH);//')
    check "the tests can fail: 3 deliberately broken interpreters are caught (failures: add ${M1:-?}, memory bound ${M2:-?}, call type ${M3:-?})" \
          '[ "${M1:-0}" -gt 0 ] 2>/dev/null && [ "${M2:-0}" -gt 0 ] 2>/dev/null && [ "${M3:-0}" -gt 0 ] 2>/dev/null'
else
    skip "official test pack: no $PACK and no wast2json to build it (scripts/pve_wasm_tools.sh)"
fi

HAVE_APPS=0
REF="$WORK/pure.ref"
: > "$REF"
if make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1 && [ -s "$APPS/pure.wasm" ]; then
    HAVE_APPS=1
    "$APPS/pure.host" > "$REF"
    N_REF=$(wc -l < "$REF")
    sed 's/^/        /' "$REF" | head -4; echo "        ... ($N_REF reference lines from the native build)"
    # «f(a, b) = v» -> имя и аргументы через пробел
    fn_of()   { local l="$1"; echo "${l%%(*}"; }
    args_of() { local l="$1"; l="${l#*(}"; l="${l%%)*}"; echo "${l//,/}"; }
    for v in pure pure-O0 pure-feat; do
        GOOD=0
        while IFS= read -r line; do
            want="${line%% = *}"
            got=$(build/wasm_host call "$APPS/$v.wasm" $(fn_of "$want") $(args_of "$want") 2>&1 | sed 's/ | fuel.*//')
            if [ "$got" = "call $line" ]; then GOOD=$((GOOD+1)); else echo "        differs ($v): want '$line', got '$got'"; fi
        done < "$REF"
        check "$v.wasm: $GOOD/$N_REF functions return exactly what the native build returns" '[ "$N_REF" -ge 18 ] && [ "$GOOD" = "$N_REF" ]'
    done
    t() { build/wasm_host call "$APPS/pure.wasm" "$@" 2>&1; }
    check "traps in real code: unreachable, out of bounds, divide by zero, INT_MIN / -1" \
          '[ "$(t crash 1)" = "call crash(1) trapped: unreachable" ] && [ "$(t poke 2147483632)" = "call poke(2147483632) trapped: out of bounds memory access" ] &&
           [ "$(t divzero 1 0)" = "call divzero(1, 0) trapped: integer divide by zero" ] && [ "$(t divzero -2147483648 -1)" = "call divzero(-2147483648, -1) trapped: integer overflow" ]'
    check "an endless loop ends when the fuel runs out"    '[ "$(WASM_FUEL=20000 build/wasm_host call "$APPS/pure.wasm" forever 1 2>&1)" = "call forever(1) trapped: out of fuel" ]'
    check "pure-feat.wasm really uses the 2.0 extensions (memory.copy/fill, saturating conversions)" \
          'wasm-objdump -d "$APPS/pure-feat.wasm" 2>/dev/null | grep -q -E "memory\.(copy|fill)" && wasm-objdump -d "$APPS/pure-feat.wasm" 2>/dev/null | grep -q "trunc_sat"'
    check "a WASI program does not start yet: its imports are named" 'build/wasm_host call "$APPS/hello.wasm" _start 2>&1 | grep -q "unknown import: wasi_snapshot_preview1\."'
    T0=$(date +%s%N); FIB=$(WASM_FUEL=100000000 build/wasm_host call "$APPS/pure.wasm" fib 32 2>&1); FIB_MS=$(( ($(date +%s%N) - T0) / 1000000 ))
    T0=$(date +%s%N); "$APPS/pure.host" >/dev/null; NAT_MS=$(( ($(date +%s%N) - T0) / 1000000 ))
    echo "        speed on the host: ${FIB% |*} in ${FIB_MS} ms (7 million calls); all 18 native functions together: ${NAT_MS} ms"
else
    skip "real code: no wasm32 clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
fi

# Стойкость: портим модули и ИСПОЛНЯЕМ те, что прошли проверку.
FUZZ_BIN=build/wasm_host; FUZZ_KIND="plain build"
if make -f Makefile.x86_64 wasm-host-asan >"$WORK/asan-cc.log" 2>&1 && [ -x build/wasm_host_asan ]; then
    FUZZ_BIN=build/wasm_host_asan; FUZZ_KIND="AddressSanitizer + UBSan"
fi
if [ $HAVE_PACK = 1 ]; then
    "$FUZZ_BIN" spec "$PACK" >"$WORK/spec-asan.log" 2>&1; RC=$?
    check "official tests under the sanitizers: same totals, no memory error ($FUZZ_KIND)" \
          '[ $RC = 0 ] && [ "$(grep -a "exec:" "$WORK/spec-asan.log" | tail -1)" = "$HOST_EXEC" ] && ! grep -q -E "ERROR|runtime error" "$WORK/spec-asan.log"'
fi
SEEDS=$(ls build/wasm/spec-json/*.wasm 2>/dev/null | tr '\n' ' ')
[ $HAVE_APPS = 1 ] && SEEDS="$SEEDS $APPS/pure-feat.wasm $APPS/pure-O0.wasm $APPS/calc-feat.wasm"
N_SEEDS=$(echo $SEEDS | wc -w)
if [ "$N_SEEDS" -ge 2 ]; then
    set -- $SEEDS; FIRST=$1; shift
    "$FUZZ_BIN" fuzz-exec "$FIRST" 300 424242 "$@" >"$WORK/fuzz.log" 2>&1; RC=$?
    tail -2 "$WORK/fuzz.log" | cut -c1-260 | sed 's/^/        /'
    check "fuzzing with execution: mutants of $N_SEEDS modules, no crash, no out-of-bounds access ($FUZZ_KIND)" \
          '[ $RC = 0 ] && grep -q "^fuzz: [0-9]* mutants of $N_SEEDS files" "$WORK/fuzz.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz.log"'
    check "fuzzing with execution: thousands of damaged-but-valid modules really ran" \
          'grep -q -E "^fuzz-exec: started=[0-9]{4,} .* calls=[0-9]{4,} traps=[1-9]" "$WORK/fuzz.log"'
else
    skip "fuzzing with execution: no seed modules (build/wasm/spec-json, $APPS)"
fi
[ "$FUZZ_KIND" = "plain build" ] && skip "sanitizer build is not available here (no libasan): fuzzing ran without it"

# ===========================================================================
echo "== Phase 1: the same interpreter inside the kernel =="
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
        '_') echo shift-minus;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.07; done; }
os_cmd() { type_text "$1"; key ret "${2:-1}"; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}
count() { grep -a -c -F "$1" "$LOG" 2>/dev/null; }
# Набрать команду и дождаться НОВОЙ строки с таким текстом (такая же могла
# быть в журнале от прошлой команды).
cmd_expect() {      # команда, ожидаемый текст, секунд
    local before; before=$(count "$2")
    os_cmd "$1" 0.3
    local end=$(( $(date +%s) + ${3:-30} ))
    while [ "$(date +%s)" -lt "$end" ]; do
        [ "$(count "$2")" -gt "${before:-0}" ] && return 0
        sleep 0.3
    done
    return 1
}

KVM=""
[ -r /dev/kvm ] && [ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
LOG="$WORK/serial.log"
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
[ $HAVE_PACK = 1 ] && mcopy -D o -i "$WORK/dev.img" "$PACK" ::/spec.pack </dev/null
if [ $HAVE_APPS = 1 ]; then
    mcopy -D o -i "$WORK/dev.img" "$APPS/pure.wasm" ::/p.wasm </dev/null
    mcopy -D o -i "$WORK/dev.img" "$APPS/pure-O0.wasm" ::/p0.wasm </dev/null
    mcopy -D o -i "$WORK/dev.img" "$APPS/pure-feat.wasm" ::/pf.wasm </dev/null
    mcopy -D o -i "$WORK/dev.img" "$APPS/hello.wasm" ::/hello.wasm </dev/null
fi
# 2 ГБ: один из тестов набора растит память модуля до 52 МБ, и на время роста
# нужны оба куска — старый и новый.
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

check "[smoke 51.1] block is printed at boot"        'wait_log "$LOG" "[smoke 51.1] interpreter" 120'
wait_log "$LOG" "[smoke 51.1] done in" 20
grep -a '^\[smoke 51\.1\]' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "[smoke 51.1] calls and a loop compute"        'grep -a -q "\[smoke 51.1\] add(2, 3) = 5, sum(1000) = 500500: PASS" "$LOG"'
check "[smoke 51.1] traps stop the program, not the kernel" 'grep -a -q "\[smoke 51.1\] traps: .*: PASS" "$LOG" && grep -a -q "\[smoke 51.1\] endless recursion .*: PASS" "$LOG"'
check "[smoke 51.1] an endless loop is stopped three ways (fuel, flag, kernel timer)" 'grep -a -q "\[smoke 51.1\] endless loop is stopped: .*: PASS" "$LOG"'
check "[smoke 51.1] imports and program memory"      'grep -a -q "\[smoke 51.1\] import: .*: PASS" "$LOG" && grep -a -q "\[smoke 51.1\] program memory: .*: PASS" "$LOG"'
MS=$(grep -a '\[smoke 51\.1\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
check "[smoke 51.1] PASS and cheap at boot (${MS:-?} ms < 100)" 'grep -a -q "\[smoke 51.1\] done in .* ms: PASS" "$LOG" && [ "${MS:-999}" -lt 100 ]'

wait_log "$LOG" "[smoke 37.6c-1] RTC" 120 || true
sleep 2
key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "rune" 1
check "rune: the engine reports that it executes"    'wait_log "$LOG" "[rune] status engine=1 stage=51." 10 && grep -a -q "\[rune\] status engine=1 stage=51\.[0-9]* exec=1" "$LOG"'
os_cmd "rune selftest" 1
check "rune selftest: all passed"                    'wait_log "$LOG" "[rune] selftest: " 15 && grep -a -q "\[rune\] selftest: [0-9]* passed, 0 failed" "$LOG"'

if [ $HAVE_PACK = 1 ]; then
    os_cmd "rune spec /disk/spec.pack" 3
    check "rune spec: the official tests ran inside the kernel" 'wait_log "$LOG" "[rune] spec: " 180'
    KTOTAL=$(grep -a '^\[wasm-spec\] total:' "$LOG" | tail -1 | tr -d '\r')
    KEXEC=$(grep -a '^\[wasm-spec\] exec:' "$LOG" | tail -1 | tr -d '\r')
    grep -a '^\[wasm-spec\] FAIL' "$LOG" | tr -d '\r' | head -8 | cut -c1-220 | sed 's/^/        /'
    grep -a '^\[rune\] spec: ' "$LOG" | tr -d '\r' | cut -c1-120 | sed 's/^/        /'
    check "rune spec: load/validation totals are identical to the host run" '[ -n "$KTOTAL" ] && [ "$KTOTAL" = "$HOST_TOTAL" ]'
    check "rune spec: execution totals are identical to the host run (${KEXEC#\[wasm-spec\] exec: })" '[ -n "$KEXEC" ] && [ "$KEXEC" = "$HOST_EXEC" ]'
    check "rune spec: PASS, kernel heap unchanged"   'grep -a -q "\[rune\] spec: PASS in .* kernel heap unchanged" "$LOG"'
else
    skip "rune spec in the kernel (no test pack)"
fi

if [ $HAVE_APPS = 1 ]; then
    # Все 18 функций — из сборки -O2; из двух других сборок — по четыре.
    GOOD=0; N=0
    while IFS= read -r line; do
        want="${line%% = *}"; N=$((N+1))
        if cmd_expect "rune call /disk/p.wasm $(fn_of "$want") $(args_of "$want")" "[rune] call $line |" 40; then GOOD=$((GOOD+1))
        else echo "        differs: want '$line', kernel said: $(grep -a '^\[rune\] call' "$LOG" | tail -1 | tr -d '\r' | cut -c1-120)"; fi
    done < "$REF"
    check "rune call: $GOOD/$N functions of pure.wasm return what the native build returns" '[ "$N" -ge 18 ] && [ "$GOOD" = "$N" ]'
    for pair in "p0.wasm:-O0" "pf.wasm:-O2 with all 2.0 features"; do
        f="${pair%%:*}"; GOOD=0; N=0
        for fn in fib memops conv divmix; do
            line=$(grep "^$fn(" "$REF" | head -1); want="${line%% = *}"; N=$((N+1))
            cmd_expect "rune call /disk/$f $(fn_of "$want") $(args_of "$want")" "[rune] call $line |" 60 && GOOD=$((GOOD+1))
        done
        check "rune call: the ${pair#*:} build gives the same results ($GOOD/$N)" '[ "$GOOD" = "$N" ]'
    done
    grep -a '^\[rune\] call' "$LOG" | tr -d '\r' | head -6 | cut -c1-110 | sed 's/^/        /'

    check "rune call: unreachable is a trap"            'cmd_expect "rune call /disk/p.wasm crash 1" "[rune] call crash(1) trapped: unreachable" 20'
    check "rune call: a wild pointer is a trap, not a kernel fault" 'cmd_expect "rune call /disk/p.wasm poke 2147483632" "[rune] call poke(2147483632) trapped: out of bounds memory access" 20'
    check "rune call: division by zero is a trap"       'cmd_expect "rune call /disk/p.wasm divzero 1 0" "[rune] call divzero(1, 0) trapped: integer divide by zero" 20'
    check "rune call: the next call works after traps"  'cmd_expect "rune call /disk/p.wasm crash 0" "[rune] call crash(0) = 7 |" 20'
    # Вечный цикл: ядро само останавливает программу, когда выходит время.
    check "rune call -t 400: an endless loop is stopped by the time limit" 'cmd_expect "rune call -t 400 /disk/p.wasm forever 1" "[rune] call forever(1) trapped: stopped (time limit 400 ms)" 30'
    LIM=$(grep -a 'call forever(1) trapped' "$LOG" | tail -1 | sed -n 's/.*| \([0-9]*\) ms.*/\1/p')
    check "the stop came on time (${LIM:-?} ms, limit 400)" '[ -n "${LIM:-}" ] && [ "$LIM" -ge 390 ] && [ "$LIM" -le 480 ]'
    check "the system is alive after the stop"          'cmd_expect "rune call /disk/p.wasm fib 20" "[rune] call fib(20) = 6765 |" 20'
    check "rune call: wrong arguments are refused"      'cmd_expect "rune call /disk/p.wasm fib" "[rune] call /disk/p.wasm: wrong number of arguments" 15 && cmd_expect "rune call /disk/p.wasm nosuch 1" "[rune] call /disk/p.wasm: no exported function with this name" 15'
    check "rune call: a WASI program is refused with the missing import named" 'cmd_expect "rune call /disk/hello.wasm _start" "[rune] call /disk/hello.wasm: unknown import: wasi_snapshot_preview1." 20'   # `call` окружения не даёт; для этого — `rune run`
    cmd_expect "rune call /disk/p.wasm fib 32" "[rune] call fib(32) = 2178309 |" 120
    FIBK=$(grep -a 'call fib(32) = 2178309' "$LOG" | tail -1 | sed -n 's/.*| \([0-9]*\) ms.*/\1/p')
    check "speed in the kernel: fib(32), 7 million calls, in ${FIBK:-?} ms (< 20000)" '[ -n "${FIBK:-}" ] && [ "$FIBK" -lt 20000 ]'
else
    skip "rune call on real code (no clang on this machine)"
fi
os_cmd "rune call /disk/nothing.wasm f" 1.5
check "rune call: a missing file is reported"        'wait_log "$LOG" "[rune] call /disk/nothing.wasm: file not found" 10'
mon "screendump $PWD/$OUT_DIR/terminal.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial.log"
mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; QEMU_PID=""

echo "== stage 51.1 interpreter: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
