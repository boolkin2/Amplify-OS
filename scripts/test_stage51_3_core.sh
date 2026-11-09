#!/bin/bash
# ============================================================================
# test_stage51_3_core.sh — подэтап 51.3: программа WebAssembly на своём ядре,
# пределы, правила доступа.
# ----------------------------------------------------------------------------
# Фаза 0 (хост): интерпретатор с паузой и продолжением.
#   Официальный набор тестов в ПОШАГОВОМ режиме — пауза на каждом витке цикла
#   и перед каждой командой, которой нужна «услуга» (функция окружения, рост
#   памяти), — даёт те же итоги, что обычный прогон; два намеренно испорченных
#   интерпретатора (потерянная глубина вызовов, рост памяти дважды) обычный
#   прогон проходят, а пошаговый — проваливают; программы WASI в пошаговом
#   режиме дают тот же вывод, что под gcc; всё это под санитайзером адресов.
# Фаза 1 (QEMU, 4 ядра): [smoke 51.3]; программы исполняются НЕ на главном
#   ядре, вывод как у gcc; пока программа считает без конца — кадры идут,
#   главное ядро свободно, терминал отвечает; остановка (`rune stop`, Ctrl+C);
#   предел времени счёта; предел памяти; ввод с клавиатуры терминала и Ctrl+D;
#   ждущая ввода и спящая программа не занимают ядро; правила доступа
#   (системные каталоги не даются, режим «без файлов»); вторая программа
#   одновременно — отказ; гость phantom рядом с работающей программой.
# Фаза 2 (2 ядра) и фаза 3 (1 ядро: программа идёт на главном, как в 51.2).
# Нужны инструменты узла (scripts/pve_wasm_tools.sh). Без них — SKIP.
# Запуск: scripts/test_stage51_3_core.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-51_3
WORK=/tmp/amplify-stage51_3
MON_PORT=4663
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

# ===========================================================================
echo "== Phase 0: the interpreter that can pause and resume (host) =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the engine (no warnings)"       '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'
ASAN=""
if make -f Makefile.x86_64 wasm-host-asan >"$WORK/asan-cc.log" 2>&1 && [ -x build/wasm_host_asan ]; then ASAN=$PWD/build/wasm_host_asan; fi

if [ ! -s "$PACK" ] && command -v wast2json >/dev/null 2>&1; then bash scripts/wasm_spec_pack.sh >"$WORK/pack.log" 2>&1; fi
if [ -s "$PACK" ]; then
    PLAIN=$($H spec "$PACK" 2>&1 | grep -a '^\[wasm-spec\] exec:' | tail -1)
    T0=$(date +%s%N); $H spec "$PACK" --stepped >"$WORK/stepped.log" 2>&1; RC=$?; MS=$(( ($(date +%s%N) - T0) / 1000000 ))
    STEP=$(grep -a '^\[wasm-spec\] exec:' "$WORK/stepped.log" | tail -1)
    echo "        stepped ${STEP#\[wasm-spec\] }  (${MS} ms)"
    grep -a 'FAIL' "$WORK/stepped.log" | head -5 | cut -c1-200 | sed 's/^/        /'
    check "official tests, a pause at every loop turn and every service: same totals, no failures" \
          '[ $RC = 0 ] && [ -n "$STEP" ] && [ "$STEP" = "$PLAIN" ] && grep -a -q "exec-skipped=0 failures=0" "$WORK/stepped.log"'
    if [ -n "$ASAN" ]; then
        "$ASAN" spec "$PACK" --stepped >"$WORK/stepped-asan.log" 2>&1; RC=$?
        check "the same under AddressSanitizer + UBSan" '[ $RC = 0 ] && [ "$(grep -a "exec:" "$WORK/stepped-asan.log" | tail -1)" = "$PLAIN" ] && ! grep -q -E "ERROR|runtime error" "$WORK/stepped-asan.log"'
    fi
    # Проверка самой проверки: сохранение положения испорчено — обычный прогон
    # этого не видит (пауз в нём нет), пошаговый обязан провалиться.
    mkdir -p "$WORK/mut"
    mutant() {      # sed-выражение -> «обычный пошаговый» (число провалов; пусто — упал)
        cp "$E/wasm_interp.c" "$WORK/mut/wasm_interp.c"
        sed -i "$1" "$WORK/mut/wasm_interp.c"
        cmp -s "$E/wasm_interp.c" "$WORK/mut/wasm_interp.c" && { echo "pattern-not-found x"; return; }
        gcc -O2 -I"$E" -o "$WORK/mut/h" tools/wasm_host.c "$E/wasm_bin.c" "$E/wasm_valid.c" "$E/wasm_rt.c" \
            "$WORK/mut/wasm_interp.c" "$E/wasm_jit.c" "$E/wasm_wasi.c" "$E/wasm_spec.c" 2>/dev/null || { echo "no-build x"; return; }
        local p s
        p=$(WASM_JIT=0 timeout 60 "$WORK/mut/h" spec "$PACK" 2>/dev/null | sed -n 's/.*total:.*failures=\([0-9]*\).*/\1/p' | tail -1)
        s=$(WASM_JIT=0 timeout 120 "$WORK/mut/h" spec "$PACK" --stepped 2>/dev/null | sed -n 's/.*total:.*failures=\([0-9]*\).*/\1/p' | tail -1)
        echo "${p:-crash} ${s:-crash}"
    }
    read -r P1 S1 <<<"$(mutant 's/s->r_depth = depth; s->r_ticks = ticks; s->r_pend = pend;/s->r_depth = 0; s->r_ticks = ticks; s->r_pend = pend;/')"
    read -r P2 S2 <<<"$(mutant 's/if (far \&\& !s->svc_once) { ip--; goto pause_service; }/if (far \&\& !s->svc_once) { goto pause_service; }/')"
    check "the tests can fail: broken saving of the position passes the plain run ($P1, $P2 failures) and fails the stepped one ($S1, $S2)" \
          '[ "$P1" = 0 ] && [ "$P2" = 0 ] && [ "$S1" != 0 ] && [ "$S2" != 0 ] && [ "$S1" != x ] && [ "$S2" != x ]'
else
    skip "official test pack: no $PACK and no wast2json to build it"
fi

HAVE_APPS=0
if make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1 && [ -s "$APPS/files.wasm" ]; then
    HAVE_APPS=1
    ref_of() { local out=$1 n=$2; shift 2; rm -rf "$WORK/r"; mkdir -p "$WORK/r"; ( cd "$WORK/r" && "$A/$n.host" "$@" ) > "$out" 2>&1; }
    ref_of "$WORK/hello.all" hello; grep -v '^arg0=' "$WORK/hello.all" > "$REF/hello.txt"
    ref_of "$REF/calc.txt" calc
    ref_of "$REF/files.txt" files
    # Те же программы, но исполнение отрезками в один виток, а всё окружение —
    # через «услугу», как в ядре.
    stepped_same() {    # исполнитель, программа, вариант, эталон
        rm -rf "$WORK/w"; mkdir -p "$WORK/w"
        WASM_STEPPED=1 WASM_STATS=1 "$1" run -d "$WORK/w" "$APPS/$2$3.wasm" 2>"$WORK/step-err.txt" | grep -v '^arg0=' > "$WORK/got.txt"
        cmp -s "$WORK/got.txt" "$4" && return 0
        echo "        differs: $2$3 stepped"; diff "$4" "$WORK/got.txt" | head -4 | sed 's/^/          /'; return 1
    }
    G=0
    for v in "" -O0 -feat; do
        stepped_same "$H" hello "$v" "$REF/hello.txt" && stepped_same "$H" calc "$v" "$REF/calc.txt" && stepped_same "$H" files "$v" "$REF/files.txt" && G=$((G+1))
    done
    STATS=$(grep -a 'stepped:' "$WORK/step-err.txt" | tail -1)
    echo "        files-feat, ${STATS#wasm_host: }"
    check "WASI programs run in slices with services: same output as built by gcc ($G/3 builds of hello, calc, files)" '[ $G = 3 ]'
    check "the stepped run really pauses and asks for services" 'echo "$STATS" | grep -q -E "stepped: [0-9]{4,} pauses, [0-9]{3,} services"'
    if [ -n "$ASAN" ]; then
        GA=0
        stepped_same "$ASAN" calc -feat "$REF/calc.txt" && GA=$((GA+1))
        stepped_same "$ASAN" files -O0 "$REF/files.txt" && GA=$((GA+1))
        check "the same under the sanitizers ($GA/2)"    '[ $GA = 2 ] && ! grep -q -E "ERROR|runtime error" "$WORK/step-err.txt"'
        JAIL="$WORK/fz/a/b/c/jail"; mkdir -p "$JAIL"
        SEEDS=$(ls build/wasm/spec-json/*.wasm 2>/dev/null | head -400 | tr '\n' ' ')
        WASM_STEPPED=1 WASM_FUZZ_DIR="$JAIL" WASM_FUZZ_FUEL=120 "$ASAN" fuzz-exec "$APPS/files-feat.wasm" 60 717171 \
            "$APPS/env.wasm" "$APPS/calc-feat.wasm" "$APPS/pure-feat.wasm" $SEEDS >"$WORK/fuzz.log" 2>&1; RC=$?
        tail -2 "$WORK/fuzz.log" | cut -c1-230 | sed 's/^/        /'
        check "fuzzing in slices: damaged modules pause and resume without a memory error" \
              '[ $RC = 0 ] && grep -q -E "^fuzz-exec: started=[0-9]{3,} " "$WORK/fuzz.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz.log"'
    else
        skip "sanitizer build is not available here (no libasan)"
    fi
else
    skip "WASI programs: no wasm32-wasi clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
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
    prog_out "$2" | grep -v '^arg0=' > "$WORK/kout.txt"
    cmp -s "$WORK/kout.txt" "$3" && return 0
    echo "        differs from gcc: rune run $1"; diff "$3" "$WORK/kout.txt" | head -6 | cut -c1-150 | sed 's/^/          /'
    return 1
}
last_line() { grep -a -F "$1" "$LOG" | tail -1 | tr -d '\r'; }
num_after() { echo "$1" | sed -n "s/.*$2 \([0-9]*\).*/\1/p"; }   # число после слова

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
    if [ $HAVE_APPS = 1 ]; then
        for pair in hello.wasm:hello.wasm calc-feat.wasm:calc.wasm files-O0.wasm:files.wasm env.wasm:env.wasm; do
            mcopy -D o -i "$IMG" "$APPS/${pair%%:*}" "::/nexus/${pair##*:}" </dev/null
        done
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
    key meta_l-spc 2
}
shutdown_vm() {
    mon "screendump $PWD/$OUT_DIR/$1.ppm"; sleep 0.5
    check "no kernel panic ($1)"                     '! grep -a -q "PANIC" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-$1.log"
    mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; QEMU_PID=""; exec 3>&- 2>/dev/null
    rm -f "$WORK"/*.img
}

# ===========================================================================
echo "== Phase 1: four cores — the program has a core of its own =="
boot 4 4c
check "[smoke 51.3] block is printed at boot"        'wait_log "$LOG" "[smoke 51.3] a program on its own core" 120'
wait_log "$LOG" "[smoke 51.3] done in" 20
grep -a '^\[smoke 51\.3\]' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "[smoke 51.3] a WASI program runs on another core, served by the main one" 'grep -a -q "\[smoke 51.3\] a WASI program runs on another core; .*: PASS (core [1-9]" "$LOG"'
check "[smoke 51.3] memory is returned"              'grep -a -q "\[smoke 51.3\] everything is returned to the kernel afterwards: PASS" "$LOG"'
MS=$(grep -a '\[smoke 51\.3\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
check "[smoke 51.3] PASS and cheap at boot (${MS:-?} ms < 100)" 'grep -a -q "\[smoke 51.3\] done in .* ms: PASS" "$LOG" && [ "${MS:-999}" -lt 100 ]'
login "4 cores"
os_cmd "rune" 1
check "rune: programs get their own core"            'wait_log "$LOG" "[rune] status engine=1 stage=51.3 exec=1 wasi=1 core=1" 10'
check "rune selftest: 16 passed, 0 failed"           'cmd_expect "rune selftest" "[rune] selftest: 16 passed, 0 failed" 20'

if [ $HAVE_APPS = 1 ]; then
    check "hello on another core: output as built by gcc, exit code 7" 'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    HDR=$(last_line "[rune] run $N/hello.wasm |")
    CORE=$(num_after "$HDR" "core")
    check "the program did not run on the main core (core ${CORE:-?})" '[ -n "${CORE:-}" ] && [ "$CORE" -ge 1 ]'
    check "calc on another core: output as built by gcc" 'run_same "$N/calc.wasm" "$N/calc.wasm" "$REF/calc.txt" "[rune] exit 0 |"'
    check "files on another core: $(wc -l < "$REF/files.txt") lines of file work as built by gcc" 'run_same "$N/files.wasm" "$N/files.wasm" "$REF/files.txt" "[rune] exit 0 |" 120'
    grep -a '^\[rune\] exit' "$LOG" | tr -d '\r' | tail -3 | cut -c1-130 | sed 's/^/        /'

    # --- программа считает без конца: система должна оставаться живой ---
    F0=$(grep -a -c '\[fstat\] fps=' "$LOG")
    os_cmd "rune run -b $N/env.wasm spin" 1
    check "an endless program starts in the background" 'wait_log "$LOG" "[rune] run $N/env.wasm | dir $N, core" 10'
    sleep 4
    check "the terminal answers while the program computes" 'cmd_expect "rune status" "[rune] status: running $N/env.wasm on core" 10'
    ST=$(last_line "[rune] status: running")
    echo "        ${ST#\[rune\] }"
    CPU=$(num_after "$ST" "cpu")
    check "the program really computes all this time (cpu ${CPU:-?} ms of ~5000)" '[ -n "${CPU:-}" ] && [ "$CPU" -ge 2500 ]'
    FPS=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+2)) | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p' | sort -n | head -1)
    BUSY=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+2)) | sed -n 's/.*busy=\([0-9]*\)\..*/\1/p' | sort -n | tail -1)
    check "frames keep coming meanwhile (>= 5 a second, worst second: ${FPS:-?})" '[ -n "${FPS:-}" ] && [ "$FPS" -ge 5 ]'
    check "the main core is not busy with the program (worst second: ${BUSY:-?} % < 60)" '[ -n "${BUSY:-}" ] && [ "$BUSY" -lt 60 ]'
    check "a second program at the same time is refused" 'cmd_expect "rune run $N/hello.wasm" "another program is still running" 10'
    # гость phantom рядом с работающей программой (у каждого своё ядро)
    if [ -n "$KVM" ] && grep -a -q "\[smoke 50\] verdict: READY" "$LOG"; then
        os_cmd "phantom selftest" 1
        check "phantom selftest next to the running program: 9 passed" 'wait_log "$LOG" "[phantom] selftest: 9 passed, 0 failed" 90'
        check "the program is still running after it"    'cmd_expect "rune status" "[rune] status: running" 10'
    else
        skip "phantom next to the program (no usable VT-x here)"
    fi
    T0=$(date +%s%N)
    check "rune stop: the program is stopped"            'cmd_expect "rune stop" "[rune] stopped: by user |" 10'
    check "after the stop nothing is running"            'cmd_expect "rune status" "[rune] status: idle" 10'

    # --- пределы ---
    check "time limit: -t 300 stops an endless program"  'cmd_expect "rune run -b -t 300 $N/env.wasm spin" "[rune] stopped: time limit 300 ms |" 20'
    LIM=$(num_after "$(last_line "[rune] stopped: time limit 300 ms")" "cpu")
    check "the limit is on computing time (cpu ${LIM:-?} ms, 290..400)" '[ -n "${LIM:-}" ] && [ "$LIM" -ge 290 ] && [ "$LIM" -le 400 ]'
    cmd_expect "rune run -m 8 $N/env.wasm oom" "[rune] exit 0 |" 60
    OOM=$(prog_out "$N/env.wasm" | sed -n 's/^oom: malloc refused after \([0-9]*\) megabytes, all intact$/\1/p')
    check "memory limit: with -m 8 malloc is refused within 8 MB (${OOM:-?} MB), the program survives" '[ -n "${OOM:-}" ] && [ "$OOM" -ge 3 ] && [ "$OOM" -le 8 ]'

    # --- ввод: терминал — консоль программы ---
    E0=$(count "echo: ready")
    os_cmd "rune run $N/env.wasm echo" 0.5
    check "a dialogue program starts and waits for input" 'wait_more "echo: ready" "$E0" 15'
    os_cmd "hello rune" 0.5
    check "a line typed in the terminal reaches the program" 'wait_log "$LOG" "echo 1: [hello rune]" 10'
    os_cmd "second line" 0.5
    check "and the next one"                             'wait_log "$LOG" "echo 2: [second line]" 10'
    X0=$(count "[rune] exit 0 |")
    key ctrl-d 0.5
    check "Ctrl+D ends the input, the program finishes"  'wait_more "[rune] exit 0 |" "$X0" 10 && prog_out "$N/env.wasm" | grep -q "echo: end of input after 2 lines"'
    S0=$(count "[rune] stopped: by user |")
    os_cmd "rune run $N/env.wasm spin" 1.5
    key ctrl-c 0.5
    check "Ctrl+C stops the program of this terminal"    'wait_more "[rune] stopped: by user |" "$S0" 10'

    # --- ожидание не занимает ядро ---
    os_cmd "rune run -b -i $N/env.wasm echo" 1
    sleep 2
    cmd_expect "rune status" "[rune] status: running" 10
    ST=$(last_line "[rune] status: running")
    echo "        ${ST#\[rune\] }"
    CPU=$(num_after "$ST" "cpu")
    check "a program waiting for input does not compute (cpu ${CPU:-?} ms in 3 s)" 'echo "$ST" | grep -q "waiting for input" && [ -n "${CPU:-}" ] && [ "$CPU" -lt 100 ]'
    os_cmd "rune type from afar" 0.5
    check "rune type: input for a background program"    'wait_log "$LOG" "echo 1: [from afar]" 10'
    check "rune eof ends it"                             'cmd_expect "rune eof" "[rune] exit 0 |" 10'
    cmd_expect "rune run -b $N/env.wasm sleep 1500" "[rune] exit 0 |" 20
    FOOT=$(grep -a '^\[rune\] exit 0 |' "$LOG" | tail -1 | tr -d '\r')
    echo "        ${FOOT#\[rune\] }"
    WALL=$(echo "$FOOT" | sed -n 's/.*| \([0-9]*\) ms, cpu.*/\1/p'); CPU=$(num_after "$FOOT" "cpu")
    check "a sleeping program: 1500 ms of sleep cost ${CPU:-?} ms of computing (wall ${WALL:-?} ms)" \
          '[ -n "${WALL:-}" ] && [ "$WALL" -ge 1500 ] && [ "$WALL" -lt 2500 ] && [ "${CPU:-999}" -lt 100 ]'

    # --- что программе можно видеть ---
    check "the whole disk is not given to a program"     'cmd_expect "rune run -d /disk $N/hello.wasm" "a program cannot be given the whole disk" 10'
    check "the system settings directory is not given (any letter case)" \
          'cmd_expect "rune run -d /disk/Sanctum $N/hello.wasm" "a program cannot be given the system settings directory" 10 && cmd_expect "rune run -d /disk/sanctum/system $N/hello.wasm" "a program cannot be given the system settings directory" 10'
    cmd_expect "rune run -n $N/files.wasm" "[rune] exit 1 |" 30
    check "-n: a program without a directory cannot create a file" 'prog_out "$N/files.wasm" | grep -q "cannot create work"'
    check "rune selftest again: 14 passed"               'cmd_expect "rune selftest" "[rune] selftest: 16 passed, 0 failed" 20 && [ "$(count "[rune] selftest: 16 passed, 0 failed")" -ge 2 ]'
else
    skip "programs on their own core (no clang on this machine)"
fi
shutdown_vm 4c

# ===========================================================================
echo "== Phase 2: two cores — the only helper core is shared =="
boot 2 2c
login "2 cores"
if [ $HAVE_APPS = 1 ]; then
    check "calc on the single helper core: output as built by gcc" 'run_same "$N/calc.wasm" "$N/calc.wasm" "$REF/calc.txt" "[rune] exit 0 |"'
    check "it ran on core 1"                             'last_line "[rune] run $N/calc.wasm |" | grep -q "core 1"'
    os_cmd "rune run -b $N/env.wasm spin" 2
    check "an endless program: the terminal still answers, rune stop works" \
          'cmd_expect "rune status" "[rune] status: running" 10 && cmd_expect "rune stop" "[rune] stopped: by user |" 10'
fi
check "rune selftest on two cores"                   'cmd_expect "rune selftest" "[rune] selftest: 16 passed, 0 failed" 20'
shutdown_vm 2c

# ===========================================================================
echo "== Phase 3: one core — the program runs on the main core, as before =="
boot 1 1c
check "[smoke 51.3] says there is no second core"    'wait_log "$LOG" "[smoke 51.3] no second core: programs run on the main core: PASS" 120'
login "1 core"
if [ $HAVE_APPS = 1 ]; then
    check "hello on the main core: output as built by gcc, exit code 7" 'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    check "the header says so"                           'last_line "[rune] run $N/hello.wasm |" | grep -q "main core, limit"'
    check "an endless program is stopped by the time limit (-t 400)" 'cmd_expect "rune run -t 400 $N/env.wasm spin" "[rune] stopped: time limit 400 ms |" 30'
fi
check "rune selftest on one core"                    'cmd_expect "rune selftest" "[rune] selftest: 15 passed, 0 failed" 20'
shutdown_vm 1c

echo "== stage 51.3 own core: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
