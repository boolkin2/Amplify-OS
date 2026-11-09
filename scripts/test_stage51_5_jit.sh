#!/bin/bash
# ============================================================================
# test_stage51_5_jit.sh — подэтап 51.5: JIT (внутренний код WebAssembly ->
# машинный код x86-64).
# ----------------------------------------------------------------------------
# От JIT требуется одно: исполнять ТО ЖЕ, что интерпретатор, только быстрее.
# Фаза 0 (хост, тот же wasm_jit.c под gcc):
#   официальный набор тестов с JIT = итогам интерпретатора — в обычном и в
#   пошаговом режиме (пауза на каждом витке, услуги через wasm_service), и под
#   санитайзером; восемь намеренно испорченных эмиттеров набор обязан
#   провалить (а с выключенным JIT — пройти: порча именно в машинном коде);
#   программы clang в трёх сборках и 18 функций pure.c = сборке gcc;
#   фаззинг с исполнением испорченных модулей — без ошибок памяти;
#   скорость: fib(32), решето и calc — не меньше чем втрое быстрее интерпретатора.
# Фаза 1 (QEMU, 4 ядра): [smoke 51.5]; набор в ядре с JIT и без — те же строки,
#   что на хосте; программы на своём ядре в обоих режимах дают вывод gcc;
#   пределы и остановка в машинном коде (время счёта, память, rune stop, ввод,
#   сон); скорость в ядре; память ядра после всего возвращена.
# Фаза 2 (1 ядро): программа машинным кодом на главном ядре, предел времени.
# Нужны инструменты узла (scripts/pve_wasm_tools.sh). Без них — SKIP.
# Запуск: scripts/test_stage51_5_jit.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-51_5
WORK=/tmp/amplify-stage51_5
MON_PORT=4666
PACK=build/wasm/spec.pack
APPS=build/wasm/apps
A=$PWD/$APPS
H=$PWD/build/wasm_host
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
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT_DIR"
REF="$WORK/ref"; mkdir -p "$REF"
ms_of() { local s; s=$(date +%s%N); "$@" >/dev/null 2>&1; echo $(( ($(date +%s%N) - s) / 1000000 )); }

# ===========================================================================
echo "== Phase 0: the JIT against the interpreter (host) =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the engine with the JIT (no warnings)" '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'
ASAN=""
if make -f Makefile.x86_64 wasm-host-asan >"$WORK/asan-cc.log" 2>&1 && [ -x build/wasm_host_asan ]; then ASAN=$PWD/build/wasm_host_asan; fi
if [ ! -s "$PACK" ] && command -v wast2json >/dev/null 2>&1; then bash scripts/wasm_spec_pack.sh >"$WORK/pack.log" 2>&1; fi

HAVE_PACK=0
if [ -s "$PACK" ]; then
    HAVE_PACK=1
    WASM_JIT=0 $H spec "$PACK" >"$WORK/spec-interp.log" 2>&1
    $H spec "$PACK" >"$WORK/spec-jit.log" 2>&1; RC=$?
    HOST_TOTAL=$(grep -a '^\[wasm-spec\] total:' "$WORK/spec-jit.log" | tail -1)
    HOST_EXEC=$(grep -a '^\[wasm-spec\] exec:' "$WORK/spec-jit.log" | tail -1)
    echo "        ${HOST_EXEC#\[wasm-spec\] }"
    grep -a 'FAIL' "$WORK/spec-jit.log" | head -5 | cut -c1-200 | sed 's/^/        /'
    check "official tests as machine code: no failures, nothing skipped" '[ $RC = 0 ] && echo "$HOST_TOTAL" | grep -q "exec-skipped=0 failures=0"'
    check "the totals are those of the interpreter, line for line" \
          '[ -n "$HOST_EXEC" ] && [ "$HOST_TOTAL" = "$(grep -a "^\[wasm-spec\] total:" "$WORK/spec-interp.log" | tail -1)" ] && [ "$HOST_EXEC" = "$(grep -a "^\[wasm-spec\] exec:" "$WORK/spec-interp.log" | tail -1)" ]'
    $H spec "$PACK" --stepped >"$WORK/spec-step.log" 2>&1; RC=$?
    check "the same with a pause at every loop turn and every service (machine code resumes)" \
          '[ $RC = 0 ] && [ "$(grep -a "exec:" "$WORK/spec-step.log" | tail -1)" = "$HOST_EXEC" ] && grep -a -q "failures=0" "$WORK/spec-step.log"'
    if [ -n "$ASAN" ]; then
        "$ASAN" spec "$PACK" >"$WORK/spec-asan.log" 2>&1; RC1=$?
        "$ASAN" spec "$PACK" --stepped >"$WORK/spec-asan-step.log" 2>&1; RC2=$?
        check "the same under AddressSanitizer + UBSan (the driver and the compiler)" \
              '[ $RC1 = 0 ] && [ $RC2 = 0 ] && [ "$(grep -a "exec:" "$WORK/spec-asan.log" | tail -1)" = "$HOST_EXEC" ] && ! grep -q -E "ERROR|runtime error" "$WORK/spec-asan.log" "$WORK/spec-asan-step.log"'
    else
        skip "sanitizer build is not available here (no libasan)"
    fi

    # Проверка самой проверки (CLAUDE.md §5.44): набор сошёлся с первого же
    # прогона. Портим эмиттер — набор обязан это заметить; с выключенным JIT
    # тот же двоичный файл обязан пройти (значит, порча — в машинном коде).
    mkdir -p "$WORK/mut"
    # Чего официальный набор НЕ проверяет (нашлось порчей эмиттера: мутант
    # stale-local проходил его целиком): значение локальной, взятое на стек ДО
    # записи в неё, обязано остаться прежним. JIT такие значения не копирует, а
    # помнит «оно в локальной», — и перед записью должен успеть их спасти.
    cat > "$WORK/old.wat" <<'WATEOF'
(module
  (func (export "old") (param i32) (result i32)
    local.get 0  i32.const 7  local.set 0  local.get 0  i32.add)
  (func (export "old2") (param i32 i32) (result i32)
    local.get 0  local.get 1  local.get 0  i32.const 100  i32.add  local.set 1  local.set 0
    local.get 0  i32.const 1000  i32.mul  local.get 1  i32.add  i32.add)
  (func (export "tee") (param i64) (result i64)
    local.get 0  local.get 0  i64.const 3  i64.mul  local.tee 0  i64.add  local.get 0  i64.add))
WATEOF
    HAVE_WAT=0
    if command -v wat2wasm >/dev/null 2>&1 && wat2wasm "$WORK/old.wat" -o "$WORK/old.wasm" 2>/dev/null; then HAVE_WAT=1; fi
    # old(5) = 5 + 7; old2(3, 4) = 3 + (4*1000 + 103); tee(10) = 10 + 30 + 30
    old_ok() { [ "$("$1" call "$WORK/old.wasm" old 5 2>&1 | sed 's/ |.*//')" = "call old(5) = 12" ] &&
               [ "$("$1" call "$WORK/old.wasm" old2 3 4 2>&1 | sed 's/ |.*//')" = "call old2(3, 4) = 4106" ] &&
               [ "$("$1" call "$WORK/old.wasm" tee 10 2>&1 | sed 's/ |.*//')" = "call tee(10) = 70" ]; }
    if [ $HAVE_WAT = 1 ]; then
        check "a local read before it is overwritten keeps its old value (machine code and interpreter)" 'old_ok "$H" && WASM_JIT=0 old_ok "$H"'
    else
        skip "the stale-local corner (no wat2wasm here)"
    fi
    mutant() {      # sed-выражение -> «провалов с JIT» «провалов без JIT»
        cp "$E/wasm_jit.c" "$WORK/mut/wasm_jit.c"
        sed -i "$1" "$WORK/mut/wasm_jit.c"
        cmp -s "$E/wasm_jit.c" "$WORK/mut/wasm_jit.c" && { echo "pattern-not-found x"; return; }
        gcc -O2 -I"$E" -o "$WORK/mut/h" tools/wasm_host.c "$E/wasm_bin.c" "$E/wasm_valid.c" "$E/wasm_rt.c" \
            "$E/wasm_interp.c" "$WORK/mut/wasm_jit.c" "$E/wasm_wasi.c" "$E/wasm_spec.c" 2>/dev/null || { echo "no-build x"; return; }
        local a b
        a=$(timeout 120 "$WORK/mut/h" spec "$PACK" 2>/dev/null | sed -n 's/.*total:.*failures=\([0-9]*\).*/\1/p' | tail -1)
        b=$(WASM_JIT=0 timeout 120 "$WORK/mut/h" spec "$PACK" 2>/dev/null | sed -n 's/.*total:.*failures=\([0-9]*\).*/\1/p' | tail -1)
        # Свой модуль — тоже «тест»: его провал считается наравне с провалами набора.
        if [ $HAVE_WAT = 1 ]; then
            [ -n "$a" ] && ! old_ok "$WORK/mut/h" && a=$((a + 1))
            [ -n "$b" ] && ! WASM_JIT=0 old_ok "$WORK/mut/h" && b=$((b + 1))
        fi
        echo "${a:-crash} ${b:-crash}"
    }
    MUT_OK=0; MUT_N=0; MUT_TXT=""
    while IFS='|' read -r name expr; do
        [ -n "$name" ] || continue
        read -r J I <<<"$(mutant "$expr")"
        MUT_N=$((MUT_N+1)); MUT_TXT="$MUT_TXT $name=$J/$I"
        [ "$J" != 0 ] && [ "$J" != x ] && [ "$J" != pattern-not-found ] && [ "$J" != no-build ] && [ "$I" = 0 ] && MUT_OK=$((MUT_OK+1))
    done <<'MUTEOF'
add-is-sub|s/vbin(j, op == 0x7C, 0x03, 0, true); vpush/vbin(j, op == 0x7C, 0x2B, 5, true); vpush/
no-bounds-check|s/jcc_trap(j, CC_A, WASM_TRAP_MEM_OOB);/(void)0;/
no-type-check|s/jcc_trap(j, CC_NE, WASM_TRAP_SIG_MISMATCH);/(void)0;/
lt-is-le|s/vcmp(j, op >= 0x50, CC_L, fuse); break;/vcmp(j, op >= 0x50, CC_LE, fuse); break;/
swapped-compare|s/int t = a; a = b; b = t; cc = cc_swap(cc); }/int t = a; a = b; b = t; }/
stale-local|s/if (j->vs\[q\].kind == V_LOC \&\& j->vs\[q\].local == b) mat(j, q);/(void)0;/
keep-not-moved|s/st64(j, RAX, R13, 8 \* ((int32_t)i - (int32_t)keep - (int32_t)drop));/(void)0;/
nan-equals|s/e8(j, 0x20); e8(j, 0xC8);  /e8(j, 0x90); e8(j, 0x90);  /
MUTEOF
    echo "        mutants (failures with the JIT / without):$MUT_TXT"
    check "the tests can fail: $MUT_OK of $MUT_N broken code generators are caught, and only with the JIT on" '[ $MUT_N = 8 ] && [ $MUT_OK = 8 ]'
else
    skip "official test pack: no $PACK and no wast2json to build it"
fi

HAVE_APPS=0
if make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1 && [ -s "$APPS/files.wasm" ]; then
    HAVE_APPS=1
    ref_of() { local out=$1 n=$2; shift 2; rm -rf "$WORK/r"; mkdir -p "$WORK/r"; ( cd "$WORK/r" && "$A/$n.host" "$@" ) 2>&1 | grep -v '^время: ' > "$out"; }
    ref_of "$WORK/hello.all" hello; grep -v '^arg0=' "$WORK/hello.all" > "$REF/hello.txt"
    ref_of "$REF/calc.txt" calc
    ref_of "$REF/files.txt" files
    ref_of "$REF/sieve.txt" sieve 300000
    same() {        # режим («» или stepped), программа, вариант, эталон, аргументы…
        local mode=$1 n=$2 v=$3 want=$4; shift 4
        rm -rf "$WORK/w"; mkdir -p "$WORK/w"
        if [ "$mode" = stepped ]; then WASM_STEPPED=1 "$H" run -d "$WORK/w" "$A/$n$v.wasm" "$@" 2>>"$WORK/run-err.txt" | grep -v '^время: \|^arg0=' > "$WORK/got.txt"
        else "$H" run -d "$WORK/w" "$A/$n$v.wasm" "$@" 2>>"$WORK/run-err.txt" | grep -v '^время: \|^arg0=' > "$WORK/got.txt"; fi
        cmp -s "$WORK/got.txt" "$want" && return 0
        echo "        differs ($mode): $n$v $*"; diff "$want" "$WORK/got.txt" | head -4 | cut -c1-150 | sed 's/^/          /'
        return 1
    }
    G=0; GS=0
    for v in "" -O0 -feat; do
        same "" hello "$v" "$REF/hello.txt" && same "" calc "$v" "$REF/calc.txt" && same "" files "$v" "$REF/files.txt" && same "" sieve "$v" "$REF/sieve.txt" 300000 && G=$((G+1))
        same stepped hello "$v" "$REF/hello.txt" && same stepped calc "$v" "$REF/calc.txt" && same stepped files "$v" "$REF/files.txt" && GS=$((GS+1))
    done
    check "programs as machine code: hello, calc, files, sieve in three builds = gcc ($G/3)" '[ $G = 3 ]'
    check "the same programs in slices, paused and resumed in machine code ($GS/3)" '[ $GS = 3 ]'
    "$APPS/pure.host" > "$REF/pure.txt"
    fn_of()   { echo "${1%%(*}"; }
    args_of() { echo "$1" | sed 's/^[^(]*(\(.*\))$/\1/' | tr ',' ' '; }
    G=0; N=0
    while IFS= read -r line; do
        want="${line%% = *}"
        for v in "" -O0 -feat; do
            N=$((N+1))
            got=$("$H" call "$APPS/pure$v.wasm" $(fn_of "$want") $(args_of "$want") 2>&1 | sed 's/^call //; s/ |.*//')
            [ "$got" = "$line" ] && G=$((G+1)) || echo "        pure$v: want '$line', got '$got'"
        done
    done < "$REF/pure.txt"
    check "pure code as machine code: $G of $N results = gcc, floats bit for bit" '[ "$N" -ge 54 ] && [ "$G" = "$N" ]'
    check "traps come from machine code as from the interpreter" \
          '[ "$("$H" call "$APPS/pure.wasm" crash 1 2>&1)" = "$(WASM_JIT=0 "$H" call "$APPS/pure.wasm" crash 1 2>&1)" ] && "$H" call "$APPS/pure.wasm" poke 2147483632 2>&1 | grep -q "trapped: out of bounds memory access" && "$H" call "$APPS/pure.wasm" divzero 1 0 2>&1 | grep -q "trapped: integer divide by zero" && [ "$(WASM_FUEL=20000 "$H" call "$APPS/pure.wasm" forever 1 2>&1)" = "call forever(1) trapped: out of fuel" ]'

    # --- скорость ---
    FJ=$(ms_of "$H" call "$APPS/pure.wasm" fib 32); FI=$(WASM_JIT=0 ms_of "$H" call "$APPS/pure.wasm" fib 32)
    SJ=$(ms_of "$H" run -d "$WORK/w" "$APPS/sieve.wasm" 30000000); SI=$(WASM_JIT=0 ms_of "$H" run -d "$WORK/w" "$APPS/sieve.wasm" 30000000)
    SN=$(ms_of "$APPS/sieve.host" 30000000)
    CJ=$(ms_of "$H" run -d "$WORK/w" "$APPS/calc.wasm"); CI=$(WASM_JIT=0 ms_of "$H" run -d "$WORK/w" "$APPS/calc.wasm"); CN=$(ms_of "$APPS/calc.host")
    echo "        fib(32): $FJ ms, interpreter $FI ms | sieve 30M: $SJ ms, interpreter $SI ms, gcc $SN ms | calc: $CJ ms, interpreter $CI ms, gcc $CN ms"
    check "speed: machine code is at least 3 times faster than the interpreter (fib, sieve, calc)" \
          '[ $((FJ * 3)) -le "$FI" ] && [ $((SJ * 3)) -le "$SI" ] && [ $((CJ * 3)) -le "$CI" ]'

    if [ -n "$ASAN" ]; then
        JAIL="$WORK/fz/a/b/c/jail"; mkdir -p "$JAIL"
        SEEDS=$(ls build/wasm/spec-json/*.wasm 2>/dev/null | tr '\n' ' ')
        WASM_FUZZ_DIR="$JAIL" WASM_FUZZ_FUEL=120 "$ASAN" fuzz-exec "$APPS/files-feat.wasm" 300 515151 \
            "$APPS/env.wasm" "$APPS/calc-feat.wasm" "$APPS/pure-feat.wasm" "$APPS/pure-O0.wasm" $SEEDS >"$WORK/fuzz.log" 2>&1; RC=$?
        tail -2 "$WORK/fuzz.log" | cut -c1-230 | sed 's/^/        /'
        check "fuzzing with execution: damaged modules compiled and run as machine code, no memory error" \
              '[ $RC = 0 ] && grep -q -E "^fuzz-exec: started=[0-9]{3,} " "$WORK/fuzz.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz.log"'
        WASM_STEPPED=1 WASM_FUZZ_DIR="$JAIL" WASM_FUZZ_FUEL=120 "$ASAN" fuzz-exec "$APPS/files-feat.wasm" 40 525252 \
            "$APPS/env.wasm" "$APPS/pure-feat.wasm" $(echo $SEEDS | tr ' ' '\n' | head -400 | tr '\n' ' ') >"$WORK/fuzz-step.log" 2>&1; RC=$?
        check "the same in slices" '[ $RC = 0 ] && grep -q -E "^fuzz-exec: started=[0-9]{3,} " "$WORK/fuzz-step.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz-step.log"'
    else
        skip "fuzzing under the sanitizer (no libasan here)"
    fi
else
    skip "programs: no wasm32-wasi clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
fi

# ===========================================================================
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; true
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
wait_more() {       # текст, сколько было, секунд — дождаться НОВОЙ строки с текстом
    local end=$(( $(date +%s) + ${3:-30} ))
    while [ "$(date +%s)" -lt "$end" ]; do
        [ "$(count "$1")" -gt "${2:-0}" ] && return 0
        sleep 0.3
    done
    return 1
}
cmd_expect() {      # команда, ожидаемый текст, секунд
    local before; before=$(count "$2")
    os_cmd "$1" 0.3
    wait_more "$2" "$before" "${3:-30}"
}
prog_out() {        # вывод последнего запуска программы по пути модуля
    tr -d '\r' < "$LOG" | awk -v h="[rune] run $1 " '
        index($0, h) == 1 { buf = ""; on = 1; next }
        on && /^\[rune\] (exit|stopped|run|warning)/ { on = 0; last = buf; next }
        on && /^\[(fat32|fstat|rune|phantom|anim)\] / { next }
        on { buf = buf $0 "\n" }
        END { printf "%s", last }'
}
run_same() {        # команда после «rune run », путь модуля, эталон, ожидаемый итог, секунд
    cmd_expect "rune run $1" "$4" "${5:-60}" || { echo "        no line '$4' after: rune run $1"; return 1; }
    prog_out "$2" | grep -v '^arg0=\|^время: ' > "$WORK/kout.txt"
    cmp -s "$WORK/kout.txt" "$3" && return 0
    echo "        differs from gcc: rune run $1"; diff "$3" "$WORK/kout.txt" | head -6 | cut -c1-150 | sed 's/^/          /'
    return 1
}
last_line() { grep -a -F "$1" "$LOG" | tail -1 | tr -d '\r'; }
num_after() { echo "$1" | sed -n "s/.*$2 \([0-9]*\).*/\1/p"; }

KVM=""
[ -r /dev/kvm ] && [ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
export MTOOLS_SKIP_CHECK=1
N=/disk/nexus
boot() {            # число ядер, имя журнала
    LOG="$WORK/serial-$2.log"
    IMG="$WORK/dev-$2.img"
    cp --sparse=always "$DEVDISK" "$IMG"
    cp "$ESP" "$WORK/esp.img"
    mmd -D s -i "$IMG" ::/nexus ::/Sanctum </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$IMG" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    [ $HAVE_PACK = 1 ] && mcopy -D o -i "$IMG" "$PACK" ::/spec.pack </dev/null
    if [ $HAVE_APPS = 1 ]; then
        for pair in hello.wasm:hello.wasm calc-feat.wasm:calc.wasm files-O0.wasm:files.wasm env.wasm:env.wasm sieve.wasm:sieve.wasm; do
            mcopy -D o -i "$IMG" "$APPS/${pair%%:*}" "::/nexus/${pair##*:}" </dev/null
        done
        mcopy -D o -i "$IMG" "$APPS/pure.wasm" ::/p.wasm </dev/null
    fi
    qemu-system-x86_64 -bios "$OVMF" ${KVM:--cpu max} -machine pc -m 2G \
        -smp cpus=$1,cores=$1,threads=1 \
        -vga none -device virtio-vga,xres=1280,yres=720 \
        -display none -serial "file:$LOG" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -drive file="$IMG",format=raw,if=none,id=sys \
        -drive file="$WORK/esp.img",format=raw,if=none,id=esp \
        -device virtio-blk-pci,drive=sys -device virtio-blk-pci,drive=esp,bootindex=0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
login() {
    wait_log "$LOG" "[smoke 37.6c-1] RTC" 120 || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
    check "desktop reached ($1)"                     'wait_log "$LOG" "[wm] main loop start" 60'
    if ! grep -a -q "\[wm\] main loop start" "$LOG"; then
        tail -4 "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
        cp "$LOG" "$OUT_DIR/serial-noboot.log"
        echo "== stage 51.5 JIT: $PASS passed, $FAIL failed, $SKIP skipped (the system did not boot) =="
        exit 1
    fi
    key meta_l-spc 2
}
shutdown_vm() {
    mon "screendump $PWD/$OUT_DIR/$1.ppm"; sleep 0.5
    check "no kernel panic ($1)"                     '! grep -a -q "PANIC" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-$1.log"
    mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; QEMU_PID=""; exec 3>&- 2>/dev/null
    rm -f "$WORK"/*.img
}
kernel_spec() {     # имя режима — строки итога набора в ядре равны строкам хоста
    local n0; n0=$(count "[rune] spec: ")
    os_cmd "rune spec /disk/spec.pack" 3
    wait_more "[rune] spec: " "$n0" 240 || { echo "        ($1: rune spec did not finish)"; return 1; }
    local kt ke
    kt=$(grep -a '^\[wasm-spec\] total:' "$LOG" | tail -1 | tr -d '\r'); ke=$(grep -a '^\[wasm-spec\] exec:' "$LOG" | tail -1 | tr -d '\r')
    echo "        $1: $(last_line "[rune] spec: " | cut -c1-110)"
    [ "$kt" = "$HOST_TOTAL" ] && [ "$ke" = "$HOST_EXEC" ] && last_line "[rune] spec: " | grep -q "PASS in .* kernel heap unchanged"
}

# ===========================================================================
echo "== Phase 1: machine code in the kernel (4 cores) =="
boot 4 4c
check "[smoke 51.5] block is printed at boot"        'wait_log "$LOG" "[smoke 51.5] JIT" 120'
wait_log "$LOG" "[smoke 51.5] done in" 20
grep -a '^\[smoke 51\.5\]' "$LOG" | tr -d '\r' | cut -c1-170 | sed 's/^/        /'
check "[smoke 51.5] machine code runs in the kernel and agrees with the interpreter" 'grep -a -q "\[smoke 51.5\] machine code runs in the kernel and agrees with the interpreter: PASS" "$LOG"'
check "[smoke 51.5] it is stopped the same way"      'grep -a -q "\[smoke 51.5\] machine code is stopped the same way: .*: PASS" "$LOG"'
MS=$(grep -a '\[smoke 51\.5\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
check "[smoke 51.5] PASS and cheap at boot (${MS:-?} ms < 100)" 'grep -a -q "\[smoke 51.5\] done in .* ms: PASS" "$LOG" && [ "${MS:-999}" -lt 100 ]'
check "the earlier smoke blocks pass as machine code too" 'grep -a -q "\[smoke 51.1\] done in .* ms: PASS" "$LOG" && grep -a -q "\[smoke 51.2\] done in .* ms: PASS" "$LOG" && grep -a -q "\[smoke 51.3\] done in .* ms: PASS" "$LOG"'
login "4 cores"
os_cmd "rune" 1
check "rune: the JIT is on by default"               'wait_log "$LOG" "[rune] status engine=1 stage=51.3 exec=1 wasi=1 core=1 jit=1" 10'
check "rune selftest: 16 passed, 0 failed"           'cmd_expect "rune selftest" "[rune] selftest: 16 passed, 0 failed" 30'

if [ $HAVE_PACK = 1 ]; then
    check "official tests INSIDE the kernel as machine code: the same lines as on the host" 'kernel_spec jit'
    check "rune jit off: new programs go to the interpreter" 'cmd_expect "rune jit off" "[rune] jit off" 10'
    check "official tests inside the kernel in the interpreter: the same lines" 'kernel_spec interpreter'
    check "rune jit on"                                  'cmd_expect "rune jit on" "[rune] jit on" 10'
else
    skip "official tests in the kernel (no test pack)"
fi

if [ $HAVE_APPS = 1 ]; then
    # --- программы на своём ядре: машинный код и услуги главного ядра ---
    check "hello as machine code on another core: output = gcc, exit code 7" 'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    HDR=$(last_line "[rune] run $N/hello.wasm |"); echo "        ${HDR#\[rune\] }"
    check "the header says: its own core, jit"           'echo "$HDR" | grep -q "core [1-9][0-9]*.*, jit$"'
    check "calc as machine code: output = gcc"           'run_same "$N/calc.wasm" "$N/calc.wasm" "$REF/calc.txt" "[rune] exit 0 |"'
    CALC_J=$(num_after "$(last_line "[rune] exit 0 |")" "cpu")
    check "files as machine code: $(wc -l < "$REF/files.txt") lines of file work = gcc" 'run_same "$N/files.wasm" "$N/files.wasm" "$REF/files.txt" "[rune] exit 0 |" 120'
    cmd_expect "rune run $N/sieve.wasm 30000000" "[rune] exit 0 |" 120
    SV_J=$(num_after "$(last_line "[rune] exit 0 |")" "cpu")
    check "sieve of 30 million as machine code counts as gcc" 'prog_out "$N/sieve.wasm" | grep -q "^простых чисел: 1857859$" || prog_out "$N/sieve.wasm" | iconv -f cp1251 -t utf-8 | grep -q "1857859"'
    cmd_expect "rune call /disk/p.wasm fib 32" "[rune] call fib(32) = 2178309 |" 120
    FIB_J=$(grep -a 'call fib(32) = 2178309' "$LOG" | tail -1 | sed -n 's/.*| \([0-9]*\) ms.*/\1/p')
    # пределы и остановка в машинном коде
    os_cmd "rune run -b $N/env.wasm spin" 1
    sleep 3
    check "an endless program in machine code: the terminal answers" 'cmd_expect "rune status" "[rune] status: running $N/env.wasm on core" 10'
    ST=$(last_line "[rune] status: running"); echo "        ${ST#\[rune\] }"
    CPU=$(num_after "$ST" "cpu")
    check "it really computes (cpu ${CPU:-?} ms of ~4000)" '[ -n "${CPU:-}" ] && [ "$CPU" -ge 2000 ]'
    check "rune stop stops machine code"                 'cmd_expect "rune stop" "[rune] stopped: by user |" 10'
    check "time limit: -t 300 stops machine code"        'cmd_expect "rune run -b -t 300 $N/env.wasm spin" "[rune] stopped: time limit 300 ms |" 20'
    LIM=$(num_after "$(last_line "[rune] stopped: time limit 300 ms")" "cpu")
    check "on the 300th millisecond of computing (cpu ${LIM:-?} ms, 290..400)" '[ -n "${LIM:-}" ] && [ "$LIM" -ge 290 ] && [ "$LIM" -le 400 ]'
    cmd_expect "rune run -m 8 $N/env.wasm oom" "[rune] exit 0 |" 60
    OOM=$(prog_out "$N/env.wasm" | sed -n 's/^oom: malloc refused after \([0-9]*\) megabytes, all intact$/\1/p')
    check "memory limit: with -m 8 malloc is refused within 8 MB (${OOM:-?} MB), all blocks intact" '[ -n "${OOM:-}" ] && [ "$OOM" -ge 3 ] && [ "$OOM" -le 8 ]'
    E0=$(count "echo: ready")
    os_cmd "rune run $N/env.wasm echo" 0.5
    check "a dialogue program waits for input"           'wait_more "echo: ready" "$E0" 15'
    os_cmd "hello jit" 0.5
    check "a typed line reaches the program in machine code" 'wait_log "$LOG" "echo 1: [hello jit]" 10'
    X0=$(count "[rune] exit 0 |")
    key ctrl-d 0.5
    check "Ctrl+D ends the input, the program finishes"  'wait_more "[rune] exit 0 |" "$X0" 10'
    cmd_expect "rune run -b $N/env.wasm sleep 1500" "[rune] exit 0 |" 20
    FOOT=$(last_line "[rune] exit 0 |")
    WALL=$(echo "$FOOT" | sed -n 's/.*| \([0-9]*\) ms, cpu.*/\1/p'); CPU=$(num_after "$FOOT" "cpu")
    check "a sleeping program does not compute (1500 ms of sleep: wall ${WALL:-?} ms, cpu ${CPU:-?} ms)" \
          '[ -n "${WALL:-}" ] && [ "$WALL" -ge 1500 ] && [ "$WALL" -lt 2500 ] && [ "${CPU:-999}" -lt 100 ]'

    # --- то же интерпретатором: вывод тот же, время — больше ---
    cmd_expect "rune jit off" "[rune] jit off" 10
    check "hello in the interpreter: the same output"    'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    check "the header says: interpreter"                 'last_line "[rune] run $N/hello.wasm |" | grep -q ", interpreter$"'
    check "calc in the interpreter: the same output"     'run_same "$N/calc.wasm" "$N/calc.wasm" "$REF/calc.txt" "[rune] exit 0 |"'
    CALC_I=$(num_after "$(last_line "[rune] exit 0 |")" "cpu")
    cmd_expect "rune run $N/sieve.wasm 30000000" "[rune] exit 0 |" 180
    SV_I=$(num_after "$(last_line "[rune] exit 0 |")" "cpu")
    cmd_expect "rune call /disk/p.wasm fib 32" "[rune] call fib(32) = 2178309 |" 120
    FIB_I=$(grep -a 'call fib(32) = 2178309' "$LOG" | tail -1 | sed -n 's/.*| \([0-9]*\) ms.*/\1/p')
    cmd_expect "rune jit on" "[rune] jit on" 10
    echo "        in the kernel: fib(32) $FIB_J ms / interpreter $FIB_I ms | sieve 30M cpu $SV_J / $SV_I ms | calc cpu $CALC_J / $CALC_I ms"
    check "speed in the kernel: machine code is at least 3 times faster (fib, sieve, calc)" \
          '[ -n "${FIB_J:-}" ] && [ -n "${FIB_I:-}" ] && [ $((FIB_J * 3)) -le "$FIB_I" ] && [ $((SV_J * 3)) -le "$SV_I" ] && [ $((CALC_J * 3)) -le "$CALC_I" ]'
    check "rune selftest again: 16 passed (the memory of all programs is back)" 'cmd_expect "rune selftest" "[rune] selftest: 16 passed, 0 failed" 30'
else
    skip "programs in the kernel (no clang on this machine)"
fi
check "no heap exhaustion in the kernel log"         '! grep -a -q "\[heap\] OOM" "$LOG"'
shutdown_vm 4c

# ===========================================================================
echo "== Phase 2: one core — machine code on the main core =="
boot 1 1c
check "[smoke 51.5] passes on one core"              'wait_log "$LOG" "[smoke 51.5] done in" 120 && grep -a -q "\[smoke 51.5\] done in .* ms: PASS" "$LOG"'
login "1 core"
if [ $HAVE_APPS = 1 ]; then
    check "hello as machine code on the main core: output = gcc" 'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    check "the header says: main core, jit"              'last_line "[rune] run $N/hello.wasm |" | grep -q "main core, limit .*, jit$"'
    check "an endless program is stopped by the time limit (-t 400)" 'cmd_expect "rune run -t 400 $N/env.wasm spin" "[rune] stopped: time limit 400 ms |" 30'
fi
check "rune selftest on one core: 15 passed"         'cmd_expect "rune selftest" "[rune] selftest: 15 passed, 0 failed" 30'
shutdown_vm 1c

echo "== stage 51.5 JIT: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
