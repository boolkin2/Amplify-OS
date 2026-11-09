#!/bin/bash
# ============================================================================
# test_stage51_2_wasi.sh — подэтап 51.2: окружение WASI preview1 («rune run»).
# ----------------------------------------------------------------------------
# Проверочные программы — обычный C (printf, fopen, opendir, clock_gettime),
# собранный clang 19 под wasm32-wasi в трёх вариантах. Эталон — те же исходники
# под gcc на хосте: вывод обязан совпасть побайтно.
# Фаза 0 (хост, тот же движок и тот же слой WASI, собранные gcc):
#   hello, calc, files (файлы и каталоги: 93 строки), env (аргументы, часы,
#   случайные байты, код возврата) — как у gcc; переименование без помощи
#   хозяина (так устроено в ядре); ввод, переменные окружения, аварийный
#   конец, вечный цикл, нехватка памяти; ГРАНИЦА каталога — программа jail
#   пытается выйти наружу 14 способами, и снаружи ничего не меняется; каталог
#   только для чтения; всё то же под санитайзером адресов; испорченные
#   программы с настоящим окружением — ни ошибки памяти, ни файла вне каталога.
# Фаза 1 (QEMU): [smoke 51.2]; `rune run` тех же программ в ядре — вывод
#   построчно как у gcc; файлы действительно на томе FAT (читаются снаружи
#   mtools, fsck без ошибок); граница каталога; только чтение; предел времени;
#   русский текст программы в окне (UTF-8 -> CP1251).
# Нужен clang узла (scripts/pve_wasm_tools.sh). Без него — SKIP.
# Запуск: scripts/test_stage51_2_wasi.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-51_2
WORK=/tmp/amplify-stage51_2
MON_PORT=4662
APPS=build/wasm/apps
A=$PWD/$APPS
H=$PWD/build/wasm_host

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
echo "== Phase 0: WASI on the host — the same layer, the same programs =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the engine with the WASI layer (no warnings)" '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'

HAVE_APPS=0
if make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1 && [ -s "$APPS/files.wasm" ] && [ -s "$APPS/jail.wasm" ]; then
    HAVE_APPS=1
else
    skip "WASI programs: no wasm32-wasi clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
fi

# Запуск «как у gcc»: программа под хост в пустом каталоге — эталон; модуль —
# в другом пустом каталоге, который он видит как «/». Сравниваются вывод и код.
same() {        # имя программы, вариант, аргументы...
    local n=$1 v=$2; shift 2
    rm -rf "$WORK/n" "$WORK/w"; mkdir -p "$WORK/n" "$WORK/w"
    ( cd "$WORK/n" && "$A/$n.host" "$@" ) > "$WORK/ref.txt" 2>&1; local rn=$?
    "$RUNNER" run -d "$WORK/w" "$APPS/$n$v.wasm" "$@" > "$WORK/got.txt" 2>&1; local rw=$?
    # Единственное законное отличие — как программа названа в argv[0].
    grep -v '^arg0=' "$WORK/ref.txt" > "$WORK/ref.f"; grep -v '^arg0=' "$WORK/got.txt" > "$WORK/got.f"
    if cmp -s "$WORK/ref.f" "$WORK/got.f" && [ $rn = $rw ]; then return 0; fi
    echo "        differs: $n$v $* (exit: native $rn, wasm $rw)"; diff "$WORK/ref.f" "$WORK/got.f" | head -6 | sed 's/^/          /'
    return 1
}

if [ $HAVE_APPS = 1 ]; then
    # Эталоны (вывод сборок gcc; каждая — в своём пустом каталоге).
    ref_of() { local out=$1 n=$2; shift 2; rm -rf "$WORK/r"; mkdir -p "$WORK/r"; ( cd "$WORK/r" && "$A/$n.host" "$@" ) > "$out" 2>&1; }
    ref_of "$WORK/hello.all" hello; grep -v '^arg0=' "$WORK/hello.all" > "$REF/hello.txt"
    ref_of "$REF/calc.txt" calc
    ref_of "$REF/files.txt" files
    ref_of "$REF/args.txt" env args one two 3
    ref_of "$REF/clock.txt" env clock
    N_FILES=$(wc -l < "$REF/files.txt")
    RUNNER=$H
    G1=0; G2=0; G3=0; G4=0
    for v in "" -O0 -feat; do
        same hello "$v" && G1=$((G1+1))
        same calc "$v" && G2=$((G2+1))
        same files "$v" && G3=$((G3+1))
        same env "$v" args one two 3 && same env "$v" clock && same env "$v" random && same env "$v" exit 7 && G4=$((G4+1))
    done
    check "hello: output and exit code as built by gcc ($G1/3 builds: -O2, -O0, -O2 with all 2.0 features)" '[ $G1 = 3 ]'
    check "calc: output as built by gcc ($G2/3 builds)"                 '[ $G2 = 3 ]'
    check "files: $N_FILES lines of file and directory work as built by gcc ($G3/3 builds)" '[ $G3 = 3 ] && [ "$N_FILES" -ge 80 ]'
    check "env: arguments, clocks and sleep, random bytes, exit code ($G4/3 builds)" '[ $G4 = 3 ]'
    same files "" >/dev/null
    check "files: what the program left is really on the disk (work/kept.txt, nothing else)" \
          '[ "$(cat "$WORK/w/work/kept.txt" 2>/dev/null)" = "kept by files.c" ] && [ "$(ls "$WORK/w/work" | wc -l)" = 1 ] && [ "$(ls "$WORK/w" | wc -l)" = 1 ]'
    export WASI_NO_RENAME=1
    check "files: rename done by copying, as in the kernel (FAT cannot rename) — same output" 'same files ""'
    unset WASI_NO_RENAME

    check "environment variable is passed"                '[ "$($H run -e OS=AMPLIFY-OS "$APPS/env.wasm" getenv OS)" = "OS=AMPLIFY-OS" ] && [ "$($H run "$APPS/env.wasm" getenv NOPE)" = "NOPE=(unset)" ]'
    check "standard input reaches the program"            '[ "$(printf 12345 | $H run "$APPS/env.wasm" stdin)" = "stdin: 5 bytes" ]'
    $H run "$APPS/env.wasm" abort >"$WORK/abort.txt" 2>&1; RC=$?
    check "abort() is a trap, reported with its reason"   '[ $RC = 101 ] && grep -q "stopped: unreachable" "$WORK/abort.txt"'
    WASM_FUEL=20000 $H run "$APPS/env.wasm" spin >"$WORK/spin.txt" 2>&1; RC=$?
    check "an endless loop is stopped by the limit"       '[ $RC = 111 ] && grep -q "stopped: out of fuel" "$WORK/spin.txt"'
    OOM=$($H run "$APPS/env.wasm" oom 2>&1)
    echo "        $OOM"
    OOM_MB=$(echo "$OOM" | sed -n 's/^oom: malloc refused after \([0-9]*\) megabytes, all intact$/\1/p')
    check "out of memory: malloc returns NULL near the 64 MB limit (${OOM_MB:-?} MB), the program survives" '[ -n "$OOM_MB" ] && [ "$OOM_MB" -ge 50 ] && [ "$OOM_MB" -le 64 ]'
    check "a module without _start is refused as not a program" '$H run "$APPS/pure.wasm" 2>&1 | grep -q "no _start function"'

    # Граница каталога. Рядом с каталогом программы лежит файл outside.txt:
    # она не должна ни прочесть, ни изменить его, ни создать что-то рядом.
    rm -rf "$WORK/j"; mkdir -p "$WORK/j/in"; echo outside > "$WORK/j/outside.txt"
    $H run -d "$WORK/j/in" "$APPS/jail.wasm" >"$WORK/jail.txt" 2>&1; RC=$?
    grep -E "ESCAPED|BROKEN|^jail" "$WORK/jail.txt" | sed 's/^/        /'
    check "jail: every attempt to leave the directory is blocked (14 of 14)" '[ $RC = 0 ] && grep -q "^jail: 14 of 14 blocked" "$WORK/jail.txt" && ! grep -q -E "ESCAPED|BROKEN" "$WORK/jail.txt"'
    check "jail: nothing changed outside the directory"   '[ "$(ls "$WORK/j" | tr "\n" " ")" = "in outside.txt " ] && [ "$(cat "$WORK/j/outside.txt")" = "outside" ] && [ -z "$(ls "$WORK/j/in")" ]'
    echo seed > "$WORK/j/in/seed.txt"
    $H run -r -d "$WORK/j/in" "$APPS/jail.wasm" ro >"$WORK/jail-ro.txt" 2>&1; RC=$?
    grep -E "ESCAPED|BROKEN|^jail" "$WORK/jail-ro.txt" | sed 's/^/        /'
    check "read-only directory: reading works, every change is refused (6 of 6)" \
          '[ $RC = 0 ] && grep -q "^jail ro: 6 of 6 blocked" "$WORK/jail-ro.txt" && ! grep -q -E "ESCAPED|BROKEN" "$WORK/jail-ro.txt" && [ "$(ls "$WORK/j/in")" = "seed.txt" ] && [ "$(cat "$WORK/j/in/seed.txt")" = "seed" ]'

fi

# То же под санитайзерами и фаззинг с настоящим окружением.
FUZZ_BIN="";
if make -f Makefile.x86_64 wasm-host-asan >"$WORK/asan-cc.log" 2>&1 && [ -x build/wasm_host_asan ]; then FUZZ_BIN=$PWD/build/wasm_host_asan; fi
if [ $HAVE_APPS = 1 ] && [ -n "$FUZZ_BIN" ]; then
    RUNNER=$FUZZ_BIN
    GA=0
    same calc -feat 2>"$WORK/asan1.txt" && GA=$((GA+1))
    same files "" 2>>"$WORK/asan1.txt" && GA=$((GA+1))
    same files -O0 2>>"$WORK/asan1.txt" && GA=$((GA+1))
    rm -rf "$WORK/ja"; mkdir -p "$WORK/ja/in"
    $FUZZ_BIN run -d "$WORK/ja/in" "$APPS/jail.wasm" >"$WORK/jail-asan.txt" 2>&1 && grep -q "^jail: 14 of 14 blocked" "$WORK/jail-asan.txt" && GA=$((GA+1))
    check "the same programs under AddressSanitizer + UBSan: same results, no memory error ($GA/4)" \
          '[ $GA = 4 ] && ! grep -q -E "ERROR|runtime error" "$WORK/got.txt" "$WORK/jail-asan.txt"'

    # Испорченные программы получают НАСТОЯЩЕЕ окружение: каталог-песочница
    # лежит глубоко, чтобы даже ошибочный выход на уровень-другой был виден.
    JAIL="$WORK/fz/a/b/c/jail"; mkdir -p "$JAIL"
    # «Топлива» — на запуск libc и первые десятки обращений к файлам, не на всю
    # программу: зациклившийся мутант сжигает запас целиком, а их тысячи (§5.44).
    WASM_FUZZ_DIR="$JAIL" WASM_FUZZ_FUEL=120 "$FUZZ_BIN" fuzz-exec "$APPS/files-feat.wasm" 2000 515151 \
        "$APPS/files-O0.wasm" "$APPS/jail.wasm" "$APPS/env.wasm" "$APPS/calc-feat.wasm" "$APPS/hello.wasm" >"$WORK/fuzz.log" 2>&1; RC=$?
    tail -2 "$WORK/fuzz.log" | cut -c1-250 | sed 's/^/        /'
    check "fuzzing with the real environment: 12000 damaged programs, no crash, no out-of-bounds access" \
          '[ $RC = 0 ] && grep -q "^fuzz: 12000 mutants of 6 files" "$WORK/fuzz.log" && ! grep -q -E "ERROR|runtime error" "$WORK/fuzz.log"'
    check "fuzzing: thousands of damaged programs really ran" 'grep -q -E "^fuzz-exec: started=[0-9]{4,} " "$WORK/fuzz.log"'
    OUTSIDE=$(find "$WORK/fz" -mindepth 1 -not -path "$JAIL" -not -path "$JAIL/*" -not -path "$WORK/fz/a" -not -path "$WORK/fz/a/b" -not -path "$WORK/fz/a/b/c" | wc -l)
    INSIDE=$(find "$JAIL" -mindepth 1 | wc -l)
    check "fuzzing: not one file appeared outside the directory ($INSIDE entries inside, $OUTSIDE outside)" '[ "$OUTSIDE" = 0 ]'
elif [ $HAVE_APPS = 1 ]; then
    skip "sanitizer build is not available here (no libasan)"
fi

# ===========================================================================
echo "== Phase 1: the same programs inside the kernel =="
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
# Набрать команду и дождаться НОВОЙ строки с таким текстом.
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
# Вывод последнего запуска программы: строки между «[rune] run <путь> …» и
# строкой итога. $1 — путь модуля, как в команде.
prog_out() {
    tr -d '\r' < "$LOG" | awk -v h="[rune] run $1 " '
        index($0, h) == 1 { buf = ""; on = 1; next }
        on && /^\[rune\] (exit|stopped|run|warning)/ { on = 0; last = buf; next }
        on && /^\[fat32\] / { next }                # журнал драйвера диска — не вывод программы
        on { buf = buf $0 "\n" }
        END { printf "%s", last }'
}
# Запустить программу, дождаться её конца (новой строки итога) и проверить,
# что в её выводе есть текст. [51.3] Ждать именно итога, а не самого текста:
# пока программа жива, терминал — её консоль, и следующая команда ушла бы ей.
run_has() {         # команда после «rune run », путь модуля, текст в выводе [, итог [, секунд]]
    cmd_expect "rune run $1" "${4:-[rune] exit 0 |}" "${5:-60}" || { echo "        no line '${4:-[rune] exit 0 |}' after: rune run $1"; return 1; }
    prog_out "$2" | grep -q -F "$3"
}
# Запустить программу в ядре и сравнить её вывод с эталоном.
run_same() {        # команда после «rune run », путь модуля, эталон, ожидаемый итог, секунд
    cmd_expect "rune run $1" "$4" "${5:-60}" || { echo "        no line '$4' after: rune run $1"; return 1; }
    prog_out "$2" | grep -v '^arg0=' > "$WORK/kout.txt"
    cmp -s "$WORK/kout.txt" "$3" && return 0
    echo "        differs from gcc: rune run $1"; diff "$3" "$WORK/kout.txt" | head -6 | cut -c1-150 | sed 's/^/          /'
    return 1
}

KVM=""
[ -r /dev/kvm ] && [ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
LOG="$WORK/serial.log"
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
IMG="$WORK/dev.img"
mmd -D s -i "$IMG" ::/nexus </dev/null 2>/dev/null
if [ $HAVE_APPS = 1 ]; then
    for pair in hello.wasm:hello.wasm calc-feat.wasm:calc.wasm files-O0.wasm:files.wasm env.wasm:env.wasm jail.wasm:jail.wasm pure.wasm:pure.wasm; do
        mcopy -D o -i "$IMG" "$APPS/${pair%%:*}" "::/nexus/${pair##*:}" </dev/null
    done
    # Каталоги для проверки границы: cell — пустой, ro — с одним файлом;
    # рядом с ними, в /nexus, — файл, до которого программа добраться не должна.
    mmd -D s -i "$IMG" ::/nexus/cell ::/nexus/ro </dev/null
    echo outside > "$WORK/outside.txt"; echo seed > "$WORK/seed.txt"
    mcopy -D o -i "$IMG" "$WORK/outside.txt" ::/nexus/outside.txt </dev/null
    mcopy -D o -i "$IMG" "$WORK/seed.txt" ::/nexus/ro/seed.txt </dev/null
fi
# Диски — virtio-blk (системный первым: ядро монтирует первый найденный том
# FAT). На IDE под KVM запись идёт ~0.2 МиБ/с (PIO, CLAUDE.md §8), и программа
# files с её шестью десятками записей файла тратит ~10 с — на диск, не на счёт.
qemu-system-x86_64 -bios "$OVMF" ${KVM:--cpu max} -machine pc -m 2G \
    -smp cpus=4,cores=4,threads=1 \
    -vga none -device virtio-vga,xres=1280,yres=720 \
    -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive file="$IMG",format=raw,if=none,id=sys \
    -drive file="$WORK/esp.img",format=raw,if=none,id=esp \
    -device virtio-blk-pci,drive=sys -device virtio-blk-pci,drive=esp,bootindex=0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }

check "[smoke 51.2] block is printed at boot"        'wait_log "$LOG" "[smoke 51.2] WASI preview1" 120'
wait_log "$LOG" "[smoke 51.2] done in" 20
grep -a '^\[smoke 51\.2\]' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "[smoke 51.2] a WASI program prints and exits with its code" 'grep -a -q "\[smoke 51.2\] a WASI program prints .*: PASS" "$LOG"'
check "[smoke 51.2] memory is returned"              'grep -a -q "\[smoke 51.2\] the environment returns all memory to the kernel: PASS" "$LOG"'
MS=$(grep -a '\[smoke 51\.2\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
check "[smoke 51.2] PASS and cheap at boot (${MS:-?} ms < 100)" 'grep -a -q "\[smoke 51.2\] done in .* ms: PASS" "$LOG" && [ "${MS:-999}" -lt 100 ]'

wait_log "$LOG" "[smoke 37.6c-1] RTC" 120 || true
sleep 2
key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "rune" 1
check "rune: the engine reports WASI"                'wait_log "$LOG" "[rune] status engine=1 stage=51." 10 && grep -a -q "\[rune\] status engine=1 stage=51\.[0-9]* exec=1 wasi=1" "$LOG"'
os_cmd "rune selftest" 1
check "rune selftest: all passed"                    'wait_log "$LOG" "[rune] selftest: " 15 && grep -a -q "\[rune\] selftest: [0-9]* passed, 0 failed" "$LOG"'

if [ $HAVE_APPS = 1 ]; then
    N=/disk/nexus
    check "rune run hello: output as built by gcc, exit code 7" 'run_same "$N/hello.wasm" "$N/hello.wasm" "$REF/hello.txt" "[rune] exit 7 |"'
    check "rune run hello: argv[0] is the file name"     'prog_out "$N/hello.wasm" | grep -q "^arg0=hello.wasm$"'
    check "rune run calc: $(wc -l < "$REF/calc.txt") lines as built by gcc" 'run_same "$N/calc.wasm" "$N/calc.wasm" "$REF/calc.txt" "[rune] exit 0 |"'
    check "rune run files: $(wc -l < "$REF/files.txt") lines of file and directory work as built by gcc" \
          'run_same "$N/files.wasm" "$N/files.wasm" "$REF/files.txt" "[rune] exit 0 |" 120'
    grep -a '^\[rune\] \(run\|exit\)' "$LOG" | tr -d '\r' | tail -6 | cut -c1-130 | sed 's/^/        /'
    check "rune run env args: arguments reach the program" 'run_same "$N/env.wasm args one two 3" "$N/env.wasm" "$REF/args.txt" "[rune] exit 0 |"'
    check "rune run env clock: clocks run, sleep sleeps"  'run_same "$N/env.wasm clock" "$N/env.wasm" "$REF/clock.txt" "[rune] exit 0 |"'
    check "rune run env random: random bytes"            'run_has "$N/env.wasm random" "$N/env.wasm" "random: ok"'
    check "rune run env getenv: the system gives OS=AMPLIFY-OS" 'run_has "$N/env.wasm getenv OS" "$N/env.wasm" "OS=AMPLIFY-OS"'
    check "rune run env exit 3: the exit code is reported" 'cmd_expect "rune run $N/env.wasm exit 3" "[rune] exit 3 |" 30'
    check "rune run -b env stdin: a background program has no input — end of input at once" 'cmd_expect "rune run -b $N/env.wasm stdin" "[rune] exit 0 |" 30 && prog_out "$N/env.wasm" | grep -q "stdin: 0 bytes"'
    check "rune run env abort: a trap, not a kernel fault" 'cmd_expect "rune run $N/env.wasm abort" "[rune] stopped: unreachable |" 30'
    cmd_expect "rune run $N/env.wasm oom" "[rune] exit 0 |" 60
    KOOM=$(prog_out "$N/env.wasm" | sed -n 's/^oom: malloc refused after \([0-9]*\) megabytes, all intact$/\1/p')
    check "rune run env oom: malloc returns NULL near the 64 MB limit (${KOOM:-?} MB), the program survives" '[ -n "$KOOM" ] && [ "$KOOM" -ge 50 ] && [ "$KOOM" -le 64 ]'
    check "rune run -t 500 env spin: stopped by the time limit" 'cmd_expect "rune run -t 500 $N/env.wasm spin" "[rune] stopped: time limit 500 ms |" 30'
    # Русский текст программы: в окно (и в журнал) он идёт в кодировке ядра, CP1251.
    cmd_expect "rune run $N/env.wasm utf8" "[rune] exit 0 |" 30
    check "program text in Russian reaches the window (UTF-8 -> CP1251, a letter split between two writes)" \
          'grep -a -q "$(printf "\xcf\xf0\xe8\xe2\xe5\xf2 \xe8\xe7 WebAssembly")" "$LOG" && grep -a -q "$(printf "\xd0\xf3\xed\xe0: \xf1\xf2\xf0\xee\xea\xe0")" "$LOG"'

    cmd_expect "rune run -d $N/cell $N/jail.wasm" "[rune] exit" 60
    prog_out "$N/jail.wasm" > "$WORK/kjail.txt"
    grep -E "ESCAPED|BROKEN|^jail" "$WORK/kjail.txt" | sed 's/^/        /'
    check "jail in the kernel: every attempt to leave the directory is blocked (14 of 14)" \
          'grep -q "^jail: 14 of 14 blocked" "$WORK/kjail.txt" && ! grep -q -E "ESCAPED|BROKEN" "$WORK/kjail.txt"'
    cmd_expect "rune run -r -d $N/ro $N/jail.wasm ro" "[rune] exit" 60
    prog_out "$N/jail.wasm" > "$WORK/kjail-ro.txt"
    grep -E "ESCAPED|BROKEN|^jail" "$WORK/kjail-ro.txt" | sed 's/^/        /'
    check "read-only directory in the kernel: reading works, every change is refused (6 of 6)" \
          'grep -q "^jail ro: 6 of 6 blocked" "$WORK/kjail-ro.txt" && ! grep -q -E "ESCAPED|BROKEN" "$WORK/kjail-ro.txt"'
    check "a module without _start is refused"           'cmd_expect "rune run $N/pure.wasm" "[rune] run $N/pure.wasm: no _start function" 20'
    check "a directory that does not exist is refused"   'cmd_expect "rune run -d /disk/nope $N/hello.wasm" "[rune] run $N/hello.wasm: the program directory does not exist" 20'
    check "the system is alive: selftest again"          'cmd_expect "rune selftest" "[rune] selftest: " 20 && [ "$(grep -a -c "\[rune\] selftest: [0-9]* passed, 0 failed" "$LOG")" -ge 2 ]'
else
    skip "rune run in the kernel (no clang on this machine)"
fi
os_cmd "rune run /disk/nexus/nothing.wasm" 1.5
check "rune run: a missing file is reported"         'wait_log "$LOG" "[rune] run /disk/nexus/nothing.wasm: file not found" 10'
mon "screendump $PWD/$OUT_DIR/terminal.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial.log"
mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; QEMU_PID=""
sleep 0.5

# Том FAT после работы программ — глазами хоста.
if [ $HAVE_APPS = 1 ]; then
    KEPT=$(mtype -i "$IMG" ::/nexus/work/kept.txt 2>/dev/null | tr -d '\r')
    WORKDIR=$(mdir -b -i "$IMG" ::/nexus/work 2>/dev/null | tr -d '\r' | grep -c .)
    check "on the FAT volume: files.c left exactly one file, readable from the host (work/kept.txt)" '[ "$KEPT" = "kept by files.c" ] && [ "$WORKDIR" = 1 ]'
    NEXUS=$(mdir -b -i "$IMG" ::/nexus 2>/dev/null | tr -d '\r' | tr 'A-Z' 'a-z')
    check "on the FAT volume: nothing appeared outside the program directories" \
          '[ "$(mtype -i "$IMG" ::/nexus/outside.txt | tr -d "\r")" = "outside" ] && ! echo "$NEXUS" | grep -q -E "escape|stolen|taken|/dir" && [ -z "$(mdir -b -i "$IMG" ::/nexus/cell 2>/dev/null | grep .)" ]'
    check "on the FAT volume: the read-only directory is untouched" \
          '[ "$(mtype -i "$IMG" ::/nexus/ro/seed.txt | tr -d "\r")" = "seed" ] && [ "$(mdir -b -i "$IMG" ::/nexus/ro | grep -c .)" = 1 ]'
    FSCK=$(command -v fsck.fat || echo /usr/sbin/fsck.fat)
    "$FSCK" -n "$IMG" >"$WORK/fsck.txt" 2>&1; RC=$?
    grep -v -E "^fsck.fat|^$" "$WORK/fsck.txt" | head -6 | cut -c1-130 | sed 's/^/        /'
    check "on the FAT volume: fsck finds no damaged chains or directories after all that (rc $RC)" \
          '[ -s "$WORK/fsck.txt" ] && ! grep -q -i -E "contains a free cluster|cluster chain|shares clusters|orphan|truncat|bad (cluster|entry|short|long)|loop in|start cluster|is not a valid|both FATs|file size is" "$WORK/fsck.txt"'
fi
rm -f "$WORK"/*.img

echo "== stage 51.2 WASI: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
