#!/bin/bash
# ============================================================================
# test_stage51_4_app.sh — подэтап 51.4: приложение Rune (окно программ
# WebAssembly: библиотека и консоль программы).
# ----------------------------------------------------------------------------
# Всё — как человек: щелчки по координатам из строк `[rune-ui] layout …`
# (QMP, планшет), клавиши — sendkey. Что окно ПОКАЗЫВАЕТ, тест узнаёт командами
# `rune log` (весь журнал консоли окна) и `rune screen` (видимые строки): они
# печатают в serial ровно те экранные строки, которые рисует окно. Эталон
# вывода каждой программы — тот же исходник под gcc.
# Фаза 0 (хост): встроенные программы в дереве (userspace/wasm/dist) = свежей
#   сборке из исходников, побайтно; каждая = сборке gcc; в каждой есть строка
#   описания; все четыре лежат в initrd.
# Фаза 1 (QEMU, 4 ядра, 1920x1080): окно, список (встроенные + с диска),
#   описания; диалог (abacus, oracle), длинная строка с переносом, прокрутка
#   (клавиши, колесо), конец ввода, «Запустить снова»; настройки запуска:
#   аргументы, каталог, доступ (запись / чтение / без файлов), предел памяти;
#   отказы (системный каталог, испорченный модуль, неверный предел);
#   считающая без конца программа — состояние, кадры, занятость главного ядра,
#   закрытие окна без остановки и возврат; конец программы при закрытом окне;
#   программа, запущенная из терминала.
# Фаза 2 (1 ядро): программа идёт на главном ядре, окно показывает итог.
# Нужны инструменты узла (scripts/pve_wasm_tools.sh). Без них — SKIP.
# Запуск: scripts/test_stage51_4_app.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-51_4
WORK=/tmp/amplify-stage51_4
MON_PORT=4664
QMP_PORT=4665
APPS=build/wasm/apps
DIST=userspace/wasm/dist
A=$PWD/$APPS
H=$PWD/build/wasm_host
BUILTIN="oracle abacus sieve scribe"

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

# Один экземпляр: общий рабочий каталог и порты (CLAUDE.md §5.36).
if pgrep -f "[q]emu-system.*$WORK/" >/dev/null 2>&1; then
    echo "another run of this test is still alive (QEMU with $WORK) — wait for it or kill it"
    exit 1
fi
rm -rf "$WORK"; mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.log
REF="$WORK/ref"; mkdir -p "$REF"
gcc -O2 -o build/ppmdiff tools/ppmdiff.c || { echo "cannot build tools/ppmdiff.c"; exit 1; }

# ===========================================================================
echo "== Phase 0: the built-in programs (host) =="
make -f Makefile.x86_64 wasm-host >"$WORK/host-cc.log" 2>&1
check "host build of the engine (no warnings)"       '[ -x build/wasm_host ] && ! grep -q -E "warning|error" "$WORK/host-cc.log"'
if ! make -f Makefile.x86_64 wasm-apps >"$WORK/apps.log" 2>&1 || [ ! -s "$APPS/abacus.wasm" ]; then
    skip "the Rune window: no wasm32-wasi clang on this machine ($(head -1 "$WORK/apps.log" | cut -c1-80))"
    echo "== stage 51.4 app: $PASS passed, $FAIL failed, $SKIP skipped =="
    exit $FAIL
fi

# Готовые модули в дереве обязаны быть тем, что дают исходники: иначе в ОС
# поедет не та программа, которую читает человек.
make -f Makefile.x86_64 wasm-dist RUNE_DIST_OUT="$WORK/dist" >"$WORK/dist.log" 2>&1
G=0; for n in $BUILTIN; do cmp -s "$WORK/dist/$n.wasm" "$DIST/$n.wasm" && G=$((G+1)); done
check "built-in programs in the tree are what the sources give, byte for byte ($G/4)" '[ $G = 4 ]'
G=0; for n in $BUILTIN; do wasm-objdump -h "$DIST/$n.wasm" 2>/dev/null | grep -q '"rune.about"' && G=$((G+1)); done
check "each carries its description (custom section rune.about) ($G/4)" '[ $G = 4 ]'
G=0; for n in $BUILTIN; do grep -a -q "$n.wasm" build/initrd-x86_64.img && G=$((G+1)); done
check "all four are in the initrd ($G/4)"            '[ $G = 4 ]'
SZ=$(cat $(for n in $BUILTIN; do echo "$DIST/$n.wasm"; done) | wc -c)
echo "        four programs: $SZ bytes"

# Эталоны: тот же исходник под gcc. Каталог программы — $WORK/r (пустой).
# ref <файл> <ввод для printf> <программа> [аргументы] — каталог НЕ чистится:
# так несколько запусков подряд видят файлы друг друга, как в ОС.
ref() { local out=$1 in=$2 n=$3; shift 3; ( cd "$WORK/r" && printf "$in" | "$A/$n.host" "$@" ) 2>&1 | grep -v '^время: ' > "$out"; }
fresh() { rm -rf "$WORK/r"; mkdir -p "$WORK/r"; }
IN_ABACUS='(2 + 3) * 4\n10 / 0\n7 %% 3\nq\n'
LONG=$(printf '1+%.0s' $(seq 1 90))1                 # 181 знак: строка длиннее консоли
fresh
ref "$REF/abacus.txt" "$IN_ABACUS" abacus
ref "$REF/abacus-long.txt" "$LONG\nq\n" abacus
ref "$REF/abacus-arg.txt" '' abacus '6*7'
ref "$REF/oracle-win.txt" '50\n75\nabc\n88\n94\n97\n99\n' oracle 2024
ref "$REF/oracle-eof.txt" '10\n' oracle 7
ref "$REF/oracle-far.txt" '40\n' oracle 7
ref "$REF/sieve.txt" '' sieve
ref "$REF/sieve-big.txt" '' sieve 30000000
ref "$REF/files.txt" '' files
fresh
ref "$REF/scribe-note.txt" '' scribe note first note
ref "$REF/scribe-list.txt" '' scribe
ref "$REF/scribe-count.txt" '' scribe notes.txt
# Те же сценарии — модулями из дерева на хост-сборке движка: расхождение с
# gcc здесь значит, что дело не в окне.
host_same() { local want=$1 in=$2 n=$3; shift 3; printf "$in" | "$H" run -d "$WORK/w" "$DIST/$n.wasm" "$@" 2>&1 | grep -v '^время: ' | cmp -s - "$want"; }
rm -rf "$WORK/w"; mkdir -p "$WORK/w"; G=0
host_same "$REF/abacus.txt" "$IN_ABACUS" abacus && G=$((G+1))
host_same "$REF/oracle-win.txt" '50\n75\nabc\n88\n94\n97\n99\n' oracle 2024 && G=$((G+1))
host_same "$REF/oracle-eof.txt" '10\n' oracle 7 && G=$((G+1))
host_same "$REF/sieve.txt" '' sieve && G=$((G+1))
host_same "$REF/scribe-note.txt" '' scribe note first note && G=$((G+1))
host_same "$REF/scribe-list.txt" '' scribe && G=$((G+1))
host_same "$REF/scribe-count.txt" '' scribe notes.txt && G=$((G+1))
check "built-in programs behave as built by gcc on the host engine ($G/7)" '[ $G = 7 ]'

# Как выглядит диалог В ОКНЕ: набранное встаёт в журнал сразу за приглашением
# программы (эхо, как в терминале), а в выводе под gcc его нет. Строим
# ожидаемый текст из эталона: после каждого приглашения в начале строки —
# очередная введённая строка.
cat > "$WORK/echo.py" <<'PYEOF'
import sys
ref = open(sys.argv[1], encoding='utf-8').read()
prompt, inputs = sys.argv[2], sys.argv[3:]
out, pos, k, bol = [], 0, 0, True
while pos < len(ref):
    if bol and k < len(inputs) and ref.startswith(prompt, pos):
        out.append(prompt + inputs[k] + chr(10))
        pos += len(prompt)
        k += 1
        continue
    out.append(ref[pos])
    bol = ref[pos] == chr(10)
    pos += 1
sys.stdout.write(''.join(out))
PYEOF
python3 "$WORK/echo.py" "$REF/abacus.txt" ': ' '(2 + 3) * 4' '10 / 0' '7 % 3' 'q' > "$REF/abacus.win"
python3 "$WORK/echo.py" "$REF/oracle-win.txt" '> ' 50 75 abc 88 94 97 99 > "$REF/oracle-win.win"
python3 "$WORK/echo.py" "$REF/oracle-far.txt" '> ' 40 > "$REF/oracle-far.win"
check "expected window text of a dialogue is built (echo after every prompt)" \
      'grep -q "^: (2 + 3) \* 4$" "$REF/abacus.win" && grep -q "^> 99$" "$REF/oracle-win.win" && [ "$(wc -l < "$REF/abacus.win")" -gt "$(wc -l < "$REF/abacus.txt")" ]'

# ===========================================================================
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null; rm -f "$WORK"/*.img; true
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
        '(') echo shift-9;; ')') echo shift-0;; '*') echo shift-8;; '+') echo shift-equal;; '%') echo shift-5;;
        '_') echo shift-minus;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" "${TYPE_DELAY:-0.08}"; done; }
cmd()   { type_text "$1"; key ret "${2:-1}"; }
qmp()   { echo "$1" >&4; sleep 0.1; while read -t 0.05 -r -u 4 _; do :; done; }
qmp_connect() {
    for _ in $(seq 1 50); do
        if exec 4<>/dev/tcp/127.0.0.1/$QMP_PORT 2>/dev/null; then
            sleep 0.3; while read -t 0.2 -r -u 4 _; do :; done
            qmp '{"execute":"qmp_capabilities"}'
            return 0
        fi
        sleep 0.1
    done
    return 1
}
# Планшет: координаты 0..32767 по каждой оси; у нас — пиксели экрана 1920x1080.
abs_px() {
    local ax=$(( $1 * 32767 / 1919 )) ay=$(( $2 * 32767 / 1079 ))
    qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}}]}}"
}
btn() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$1,\"button\":\"left\"}}]}}"; }
wheel() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"$1\"}}]}}"; }
click_px() { abs_px "$1" "$2"; sleep 0.3; btn true; sleep 0.15; btn false; sleep 0.3; }
# Координаты элемента из строки раскладки ядра: `... name=X,Y ...`.
coord() { echo "$1" | tr ' ' '\n' | tr -d '\r' | sed -n "s/^$2=\([0-9]*\),\([0-9]*\)$/\1 \2/p" | head -1; }
click_at() {
    local c; c=$(coord "$1" "$2")
    [ -n "$c" ] || { echo "        (no coordinates for '$2')"; return 1; }
    click_px $c
}
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}
# Отметка в журнале и ожидание строки, появившейся ПОСЛЕ отметки.
MARK=0
mark()     { MARK=$(wc -l < "$LOG"); }
since()    { tail -n +"$((MARK+1))" "$LOG" | tr -d '\r'; }
wait_new() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        since | grep -a -q -F "$1" && return 0
        sleep 0.3
    done
    return 1
}
wait_new_i() {      # без учёта регистра: имя 8.3 FAT отдаёт заглавными (CLAUDE.md §5.41)
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        since | grep -a -q -i -F "$1" && return 0
        sleep 0.3
    done
    return 1
}
last_new() { since | grep -a -F "$1" | tail -1; }
last_any() { grep -a -F "$1" "$LOG" | tail -1 | tr -d '\r'; }
num_after() { echo "$1" | sed -n "s/.*$2 \([0-9]*\).*/\1/p"; }
shot()  { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 1; }
ndiff() { build/ppmdiff "$SHOT_DIR/$1.ppm" "$SHOT_DIR/$2.ppm" 2>/dev/null | sed -n 's/^diff=\(-\?[0-9]*\).*/\1/p'; }

# Терминал поверх окна и обратно. Super+Space каждый раз открывает НОВЫЙ
# терминал (буфер окна — 7.6 МБ кучи ядра), поэтому, сделав дело, терминал
# закрываем (Esc): фокус возвращается окну под ним. Первый вариант теста
# открывал терминал перед каждой проверкой и за десять минут исчерпал кучу.
term()   { key meta_l-spc 1.5; }
to_win() { key esc 1; }
# Журнал консоли окна: строки программы (без служебных строк окна), UTF-8.
# Оставляет окно в фокусе.
win_log() {         # файл
    mark; key ctrl-o 0.5                # Ctrl+O в окне = `rune log`
    wait_new "[rune-ui] log: rows=" 10
    since | grep -a '^\[rune-log\] [0-9]*|' | sed 's/^\[rune-log\] [0-9]*|//' | iconv -f cp1251 -t utf-8 > "$1"
    since | grep -a '^\[rune-log\] [0-9]*#' | sed 's/^\[rune-log\] [0-9]*#//' | iconv -f cp1251 -t utf-8 > "$1.sys"
}
win_screen() {      # файл; top и total — в SCR_TOP, SCR_TOTAL, SCR_ROWS
    mark; key ctrl-p 0.5                # Ctrl+P в окне = `rune screen`
    wait_new "[rune-ui] screen: rows=" 10
    since | grep -a '^\[rune-scr\] [0-9]*[|#]' | sed 's/^\[rune-scr\] [0-9]*[|#]//' | iconv -f cp1251 -t utf-8 > "$1"
    local l; l=$(last_new "[rune-ui] screen: rows=")
    SCR_ROWS=$(echo "$l" | sed -n 's/.*rows=\([0-9]*\).*/\1/p'); SCR_TOP=$(echo "$l" | sed -n 's/.*top=\([0-9]*\).*/\1/p')
    SCR_TOTAL=$(echo "$l" | sed -n 's/.*total=\([0-9]*\).*/\1/p')
}
same_log() {        # эталон — сравнить с только что снятым журналом окна
    win_log "$WORK/win.txt"
    grep -v '^время: ' "$WORK/win.txt" > "$WORK/win.cmp"
    cmp -s "$WORK/win.cmp" "$1" && return 0
    echo "        window text differs from $1:"; diff "$1" "$WORK/win.cmp" | head -6 | cut -c1-150 | sed 's/^/          /'
    return 1
}
# Раскладка библиотеки и номер строки программы в списке (0..). Не нашли —
# пусто: проверка падает на месте, первая попавшаяся не выбирается.
lib_layout() { sleep 0.7; LL=$(last_any "[rune-ui] layout view=library"); }
con_layout() { sleep 0.7; CL=$(last_any "[rune-ui] layout view=console"); }
row_of() { echo "$PROGS" | sed 's/.*programs: [0-9]* - //' | tr ';' '\n' | grep -n -i -F " $1 (" | head -1 | cut -d: -f1; }
# Выбрать программу: строка списка видна, только если она в первых ROWS.
pick() {
    local r; r=$(echo "$PROGS" | sed 's/.*programs: [0-9]* - /; /' | tr ';' '\n' | sed 1d | grep -n -i -F " $1 (" | head -1 | cut -d: -f1)
    [ -n "$r" ] || { echo "        ($1 is not in the list)"; return 1; }
    lib_layout
    local first vis i cur
    first=$(echo "$LL" | sed -n 's/.* first=\([0-9]*\).*/\1/p'); first=${first:-0}
    vis=$(echo "$LL" | tr ' ' '\n' | grep -c '^prog')
    i=$((r - 1 - first))
    mark
    if [ $i -ge 0 ] && [ $i -lt "$vis" ]; then
        click_at "$LL" "prog$i" || return 1
    else
        # Программа за краем списка: щелчок по первой видимой и стрелками до неё
        # (список прокручивается за выбранной строкой).
        click_at "$LL" prog0 || return 1
        cur=$first
        while [ $cur -lt $((r - 1)) ]; do key down 0.25; cur=$((cur + 1)); done
        while [ $cur -gt $((r - 1)) ]; do key up 0.25; cur=$((cur - 1)); done
    fi
    wait_new_i "[rune-ui] selected $1" 5
}
# Поле ввода: щёлкнуть, стереть, набрать.
set_field() {       # имя поля в раскладке, сколько стереть, текст
    lib_layout; click_at "$LL" "$1" || return 1
    for _ in $(seq 1 "$2"); do key backspace 0.04; done
    [ -n "$3" ] && type_text "$3"
    return 0
}
start_prog() {      # дождаться, что программа пошла
    lib_layout; mark; click_at "$LL" start || return 1
    wait_new "[rune-ui] state running" 10 || wait_new "[rune-ui] state ended" 3
}
wait_end() { wait_new "[rune-ui] program ended: $1" "${2:-30}"; }
say_line() { mark; type_text "$1"; key ret 0.3; wait_new "[rune-ui] input: " 5; sleep 0.4; }
back() { con_layout; mark; click_at "$CL" back; wait_new "[rune-ui] state library" 5; }

export MTOOLS_SKIP_CHECK=1
make_disk() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    mmd -D s -i "$WORK/dev.img" ::/Sanctum ::/nexus ::/nexus/desk ::/vault ::/vault/runes </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    # «Свои» программы пользователя — на диске: проверочные из прежних подэтапов
    # и обрезанный файл, который модулем не является.
    mcopy -D o -i "$WORK/dev.img" "$APPS/env.wasm" ::/vault/runes/env.wasm </dev/null
    mcopy -D o -i "$WORK/dev.img" "$APPS/files-O0.wasm" ::/vault/runes/files.wasm </dev/null
    head -c 700 "$APPS/hello.wasm" > "$WORK/broken.wasm"
    mcopy -D o -i "$WORK/dev.img" "$WORK/broken.wasm" ::/vault/runes/broken.wasm </dev/null
    printf 'not a program\n' > "$WORK/readme.txt"
    mcopy -D o -i "$WORK/dev.img" "$WORK/readme.txt" ::/vault/runes/readme.txt </dev/null
}
start_qemu() {      # журнал, число ядер
    qemu-system-x86_64 -bios "$OVMF" -enable-kvm -cpu host -machine pc -m 2G \
        -smp cpus=$2,cores=$2,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$1" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -qmp "tcp:127.0.0.1:$QMP_PORT,server,nowait" \
        -device virtio-keyboard-pci -device virtio-tablet-pci \
        -drive file="$WORK/dev.img",format=raw,if=none,id=sys \
        -drive file="$WORK/esp.img",format=raw,if=none,id=esp \
        -device virtio-blk-pci,drive=sys -device virtio-blk-pci,drive=esp,bootindex=0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
    qmp_connect || echo "  (QMP port never opened)"
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null
}
login() {
    wait_log "$1" "[smoke 37.6c-1] RTC" 120 || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    skip "the window in QEMU: no KVM here (keys and clicks under TCG take too long)"
    echo "== stage 51.4 app: $PASS passed, $FAIL failed, $SKIP skipped =="
    exit $FAIL
fi

# ===========================================================================
echo "== Phase 1: the Rune window (4 cores) =="
LOG="$WORK/serial-app.log"
make_disk
start_qemu "$LOG" 4
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
# Без рабочего стола дальше проверять нечего: десятки проверок упали бы
# цепочкой, выглядя как сбой окна (первый прогон: ядро не загрузилось вовсе).
if ! grep -a -q "\[wm\] main loop start" "$LOG"; then
    tail -4 "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
    cp "$LOG" "$SHOT_DIR/serial-noboot.log"
    echo "== stage 51.4 app: $PASS passed, $FAIL failed, $SKIP skipped (the system did not boot) =="
    exit 1
fi
sleep 2
term
mark; cmd "rune list" 1.5
wait_new "[rune] programs:" 10
since | grep -a '^\[rune\] program ' | iconv -f cp1251 -t utf-8 > "$WORK/list.txt"
cut -c1-110 "$WORK/list.txt" | sed 's/^/        /'
G=0; for n in $BUILTIN; do grep -q "^\[rune\] program $n.wasm built-in $(stat -c %s "$DIST/$n.wasm") | ..*" "$WORK/list.txt" && G=$((G+1)); done
check "rune list: four built-in programs with sizes and descriptions ($G/4)" '[ $G = 4 ]'
check "the description is the Russian line from the source" 'grep -q "oracle.wasm built-in .* | Оракул: угадайте число от 1 до 100$" "$WORK/list.txt"'
check "programs of the user come from /disk/vault/runes; a non-wasm file is not listed" \
      'grep -q -i "program /disk/vault/runes/env.wasm disk" "$WORK/list.txt" && grep -q -i "runes/files.wasm disk" "$WORK/list.txt" && ! grep -q -i "readme" "$WORK/list.txt"'

mark; cmd "summon Rune" 2
check "the Rune window opens (summon Rune)"          'wait_new "[rune-ui] open, console" 10'
OPEN=$(last_new "[rune-ui] open, console"); echo "        ${OPEN#\[rune-ui\] }"
PROGS=$(last_new "[rune-ui] programs:")
check "the library lists seven programs"             'echo "$PROGS" | grep -q "programs: 7 "'
lib_layout
check "the library reported its layout"              '[ -n "$LL" ] && [ -n "$(coord "$LL" start)" ] && [ -n "$(coord "$LL" mem)" ]'
shot 01-library

# ---- диалог: abacus ---------------------------------------------------------
check "a click selects abacus in the list"           'pick abacus.wasm'
check "«Запустить» starts it; the window turns into its console" 'start_prog'
HDR=$(last_new "[rune] run abacus.wasm |"); echo "        ${HDR#\[rune\] }"
CORE=$(num_after "$HDR" "core")
check "the program got a core of its own (core ${CORE:-?}), directory /disk/nexus" '[ -n "${CORE:-}" ] && [ "$CORE" -ge 1 ] && echo "$HDR" | grep -q "dir /disk/nexus,"'
con_layout
check "the console reported its layout"              '[ -n "$(coord "$CL" stop)" ] && [ -n "$(coord "$CL" input)" ] && echo "$CL" | grep -q "cols=[0-9]* rows=[0-9]*"'
COLS=$(echo "$CL" | sed -n 's/.* cols=\([0-9]*\).*/\1/p'); VIS=$(echo "$CL" | sed -n 's/.* rows=\([0-9]*\).*/\1/p')
shot 02-console-empty
say_line "(2 + 3) * 4"
say_line "10 / 0"
say_line "7 % 3"
shot 03-console-dialogue
D=$(ndiff 02-console-empty 03-console-dialogue)
check "the frame changes as the dialogue goes (diff=$D px)" '[ "${D:-0}" -gt 500 ]'
mark; type_text "q"; key ret 0.3
check "the program ends by itself: exit 0"           'wait_end "exit 0" 15'
check "the window shows the end and keeps the text"  'wait_new "[rune-ui] state ended" 5'
check "the dialogue in the window = gcc output with the typed lines echoed" 'same_log "$REF/abacus.win"'
check "the comparison can fail (another expected text is not accepted)" '! cmp -s "$WORK/win.cmp" "$REF/oracle-win.win" && ! cmp -s "$WORK/win.cmp" "$REF/abacus.txt"'
check "the end is written into the console as a line of the window" 'iconv -f utf-8 -t utf-8 "$WORK/win.txt.sys" | grep -q "Программа завершилась, код 0"'
shot 04-ended

# ---- длинная строка: перенос ------------------------------------------------
con_layout
mark; click_at "$CL" again
check "«Запустить снова» starts the same program"    'wait_new "[rune] run abacus.wasm |" 10 && wait_new "[rune-ui] state running" 5'
TYPE_DELAY=0.035 say_line "$LONG"
mark; type_text "q"; key ret 0.3
wait_end "exit 0" 15
win_log "$WORK/long.txt"
ROW1=$(grep -n '^: 1+1' "$WORK/long.txt" | head -1 | cut -d: -f1)
JOIN="$(sed -n "${ROW1:-1}p" "$WORK/long.txt")$(sed -n "$(( ${ROW1:-1} + 1 ))p" "$WORK/long.txt")"
check "a line longer than the console ($(( ${#LONG} + 2 )) > $COLS) is wrapped, nothing is lost" \
      '[ -n "$ROW1" ] && [ "$(sed -n "${ROW1}p" "$WORK/long.txt" | wc -m)" = "$((COLS + 1))" ] && [ "$JOIN" = ": $LONG" ]'
check "the answer after it is right"                 'grep -q "^  = 91$" "$WORK/long.txt"'
check "back to the library"                          'back'

# ---- оракул: аргументы, конец ввода ------------------------------------------
check "oracle is selected"                           'pick oracle.wasm'
set_field args 0 "2024"
check "it starts with the argument from the field"   'start_prog && since | grep -a -q "\[rune-ui\] start oracle.wasm args: 2024"'
for g in 50 75 abc 88 94 97; do say_line "$g"; done
mark; type_text "99"; key ret 0.3
check "the game is won: exit 0"                      'wait_end "exit 0" 15'
check "the game in the window = gcc output with the echo" 'same_log "$REF/oracle-win.win"'
back
set_field args 8 "7"
start_prog
say_line "40"
con_layout
mark; click_at "$CL" eof
check "«Конец ввода» closes the input; the program gives up: exit 1" 'wait_new "[rune-ui] input closed" 5 && wait_end "exit 1" 15'
win_log "$WORK/eof.txt"
check "what was said before the end of input is in the window" 'head -4 "$WORK/eof.txt" | cmp -s - <(head -4 "$REF/oracle-far.win") && grep -q "Оракул устал ждать. Это было число 78." "$WORK/eof.txt"'
back

# ---- каталог и доступ: scribe -------------------------------------------------
check "scribe is selected"                           'pick scribe.wasm'
set_field args 8 "note first note"
set_field dir 30 "/disk/nexus/desk"
check "scribe writes a note into the directory from the field" 'start_prog && wait_end "exit 0" 20 && since | grep -a -q "\[rune\] run scribe.wasm | dir /disk/nexus/desk,"'
check "its output = gcc"                             'same_log "$REF/scribe-note.txt"'
back
set_field args 30 ""
start_prog; wait_end "exit 0" 20
check "scribe lists the directory: the same as under gcc" 'same_log "$REF/scribe-list.txt"'
back
# только чтение
set_field args 0 "note again"
lib_layout; mark; click_at "$LL" ro
check "access «Чтение» is selected"                  'wait_new "[rune-ui] access read-only" 5'
start_prog
check "with read-only access the note is refused: exit 1" 'wait_end "exit 1" 20 && since | grep -a -q "dir /disk/nexus/desk (read-only)"'
win_log "$WORK/ro.txt"
check "the program says so itself"                   'grep -q "notes.txt: записать нельзя" "$WORK/ro.txt"'
back
# без файлов
set_field args 30 ""
lib_layout; mark; click_at "$LL" nofiles
check "access «Без файлов» is selected"              'wait_new "[rune-ui] access no files" 5'
start_prog
check "without a directory the program sees no files: exit 1" 'wait_end "exit 1" 20 && since | grep -a -q "\[rune\] run scribe.wasm | no files"'
win_log "$WORK/nf.txt"
check "and says so"                                  'grep -q "Каталог программе не дан" "$WORK/nf.txt"'
back
# системный каталог не даётся
lib_layout; mark; click_at "$LL" rw
wait_new "[rune-ui] access read-write" 5
set_field dir 30 "/disk/Sanctum"
lib_layout; mark; click_at "$LL" start
check "the system settings directory is refused, the window stays in the library" \
      'wait_new "[rune-ui] start refused: a program cannot be given the system settings directory" 5 && ! since | grep -a -q "state running"'
sleep 0.7; shot 05-refused
set_field dir 30 "/disk/nexus"

# ---- предел памяти: sieve ------------------------------------------------------
check "sieve is selected"                            'pick sieve.wasm'
set_field args 0 ""
start_prog
check "sieve with defaults: exit 0"                  'wait_end "exit 0" 60'
check "its numbers = gcc (the time line aside)"      'same_log "$REF/sieve.txt"'
check "the window shows the time line of the program" 'grep -q "^время: [0-9]* мс$" "$WORK/win.txt"'
back
set_field args 0 "30000000"
set_field mem 2 "99"
lib_layout; mark; click_at "$LL" start
check "a memory limit of 99 MB is refused (1..64)"   'wait_new "[rune-ui] start refused: the memory limit must be 1..64 MB" 5 && ! since | grep -a -q "state running"'
set_field mem 2 "8"
start_prog
check "with 8 MB the sieve of 30 million gets no memory: exit 1" 'wait_end "exit 1" 30 && since | grep -a -q "\[rune\] run sieve.wasm | dir /disk/nexus, core [0-9]*, memory 8 MB"'
win_log "$WORK/mem.txt"
check "the program reports it itself and survives"   'grep -q "Не хватило памяти: нужно 29297 КБ." "$WORK/mem.txt"'
back
set_field mem 2 ""
start_prog
check "without the limit the same sieve finishes: exit 0" 'wait_end "exit 0" 120'
FOOT=$(last_any "[rune] exit 0 |"); echo "        ${FOOT#\[rune\] }"
check "and counts as under gcc (1857859 primes)"     'same_log "$REF/sieve-big.txt"'
back

# ---- отказ: файл, который не модуль ---------------------------------------------
check "the damaged file is selected"                 'pick broken.wasm'
set_field args 10 ""
lib_layout; mark; click_at "$LL" start
check "a damaged module is refused with the reason"  'wait_new "[rune-ui] start refused: rejected - " 5 && ! since | grep -a -q "state running"'

# ---- длинный вывод: прокрутка -----------------------------------------------------
check "files (a program of the user, from the disk) is selected" 'pick files.wasm'
start_prog
check "files: exit 0"                                'wait_end "exit 0" 120'
NREF=$(wc -l < "$REF/files.txt")
check "$NREF lines of file work in the window = gcc" 'same_log "$REF/files.txt"'
win_screen "$WORK/scr-end.txt"
echo "        screen: rows=$SCR_ROWS top=$SCR_TOP total=$SCR_TOTAL (console $COLS x $VIS)"
check "the output is longer than the console and the window stays at its end" \
      '[ "$SCR_TOTAL" -gt "$VIS" ] && [ "$SCR_ROWS" = "$VIS" ] && [ "$SCR_TOP" = "$((SCR_TOTAL - VIS))" ]'
check "the visible rows are the last lines of the output" 'head -$((VIS - 1)) "$WORK/scr-end.txt" | cmp -s - <(tail -$((VIS - 1)) "$REF/files.txt")'
shot 06-scroll-end
key pgup 0.6
win_screen "$WORK/scr-up.txt"; TOP_UP=$SCR_TOP
EXP=$((SCR_TOTAL - VIS - (VIS - 2))); [ $EXP -lt 0 ] && EXP=0
check "PgUp scrolls back by a page (top $TOP_UP, expected $EXP)" '[ "$TOP_UP" = "$EXP" ]'
shot 07-scroll-up
D=$(ndiff 06-scroll-end 07-scroll-up)
check "the frame shows other lines (diff=$D px)"     '[ "${D:-0}" -gt 2000 ]'
key home 0.6
win_screen "$WORK/scr-home.txt"
check "Home shows the beginning of the output"       '[ "$SCR_TOP" = 0 ] && head -$VIS "$REF/files.txt" | cmp -s - "$WORK/scr-home.txt"'
con_layout; C=$(coord "$CL" panel); abs_px $C; sleep 0.3
wheel wheel-down; wheel wheel-down; sleep 0.6
win_screen "$WORK/scr-wheel.txt"
check "the mouse wheel scrolls the console (top $SCR_TOP)" '[ "$SCR_TOP" = 6 ]'
key end 0.6
win_screen "$WORK/scr-end2.txt"
check "End returns to the end of the output"         'cmp -s "$WORK/scr-end2.txt" "$WORK/scr-end.txt"'
back

# ---- программа считает без конца ----------------------------------------------------
check "env is selected"                              'pick env.wasm'
set_field args 0 "spin"
F0=$(grep -a -c '\[fstat\] fps=' "$LOG")
check "an endless program starts"                    'start_prog'
sleep 5
shot 08-computing
term; mark; cmd "rune status" 1
check "the terminal answers while the program of the window computes" 'wait_new "[rune] status: running /disk/vault/runes/" 10'
ST=$(last_new "[rune] status: running"); echo "        ${ST#\[rune\] }"
CPU=$(num_after "$ST" "cpu")
check "the program really computes (cpu ${CPU:-?} ms of ~6000)" '[ -n "${CPU:-}" ] && [ "$CPU" -ge 3000 ]'
FPS=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+2)) | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p' | sort -n | head -1)
BUSY=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+2)) | sed -n 's/.*busy=\([0-9]*\)\..*/\1/p' | sort -n | tail -1)
check "frames keep coming meanwhile (>= 5 a second, worst second: ${FPS:-?})" '[ -n "${FPS:-}" ] && [ "$FPS" -ge 5 ]'
check "the main core is not busy with the program (worst second: ${BUSY:-?} % < 60)" '[ -n "${BUSY:-}" ] && [ "$BUSY" -lt 60 ]'
mark; cmd "rune screen" 1
check "rune screen from the terminal dumps the console of the window" 'wait_new "[rune-ui] screen: rows=" 10'
mark; cmd "rune run /disk/nexus/none.wasm" 1
check "a second program at the same time is refused" 'wait_new "another program is still running" 10'
# закрыть окно — программа остаётся
to_win
mark; key esc 1
check "Esc closes the window"                        'wait_new "[rune-ui] closed" 5'
term; mark; cmd "rune status" 1
check "the program keeps running without the window" 'wait_new "[rune] status: running /disk/vault/runes/" 10'
mark; cmd "summon Rune" 1.5
check "the window opened again shows the same program" 'wait_new "[rune-ui] open, console" 5 && wait_new "[rune-ui] state running" 5 && wait_new "[rune-ui] layout view=console" 5'
con_layout
mark; click_at "$CL" stop
check "«Остановить» stops it"                        'wait_new "[rune] stopped: by user |" 10 && wait_end "stopped by user" 10'
check "the window shows the end"                     'wait_new "[rune-ui] state ended" 5'
sleep 0.7; shot 09-stopped
back

# ---- программа кончается при закрытом окне -------------------------------------------
check "abacus is selected again"                     'pick abacus.wasm'
set_field args 10 ""
start_prog
say_line "6 * 7"
mark; key esc 1
wait_new "[rune-ui] closed" 5
term; mark; cmd "rune type 100 - 1" 1
cmd "rune type q" 1
check "the program ends while its window is closed"  'wait_new "[rune-ui] program ended: exit 0" 15'
mark; cmd "summon Rune" 1.5
check "the window opened afterwards shows the finished program" 'wait_new "[rune-ui] state ended" 5'
win_log "$WORK/closed.txt"
check "with everything it printed, before and after the closing" \
      'grep -q "^: 6 \* 7$" "$WORK/closed.txt" && grep -q "^  = 42$" "$WORK/closed.txt" && grep -q "^:   = 99$" "$WORK/closed.txt" && grep -q "^: Абак убран.$" "$WORK/closed.txt"'
back

# ---- программа, запущенная из терминала ------------------------------------------------
term; mark; cmd "rune run -b /disk/vault/runes/env.wasm spin" 1.5
check "a program started from the terminal: the window turns to it" 'wait_new "[rune-ui] state running" 10'
to_win; sleep 0.5; shot 10-foreign
con_layout
mark; click_at "$CL" stop
check "the window can stop it"                       'wait_new "[rune] stopped: by user |" 10'
check "and returns to the library (the result belongs to the terminal)" 'wait_new "[rune-ui] state library" 5 && ! since | grep -a -q "state ended"'
term; mark; cmd "rune selftest" 1
check "rune selftest after all this: 14 passed"      'wait_new "[rune] selftest: 16 passed, 0 failed" 30'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-app.log"
stop_qemu

# Том глазами хоста: записка одна (вторая запись была отклонена), рядом ничего.
mcopy -n -i "$WORK/dev.img" ::/nexus/desk/notes.txt "$WORK/notes.txt" </dev/null 2>/dev/null
check "on the disk: desk/notes.txt holds the one note" '[ "$(cat "$WORK/notes.txt" 2>/dev/null)" = "first note" ]'
check "the directory of the programs was not touched by them" '[ "$(mdir -b -i "$WORK/dev.img" ::/vault/runes 2>/dev/null | wc -l)" = 4 ]'
fsck.fat -n "$WORK/dev.img" >"$WORK/fsck.log" 2>&1
check "the volume is consistent (fsck: no damaged chains or directories)" '! grep -q -i -E "cluster chain|bad |orphan|truncat|loop" "$WORK/fsck.log"'

# ===========================================================================
echo "== Phase 2: one core — the program runs on the main core =="
LOG="$WORK/serial-1c.log"
make_disk
start_qemu "$LOG" 1
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
# Без рабочего стола дальше проверять нечего: десятки проверок упали бы
# цепочкой, выглядя как сбой окна (первый прогон: ядро не загрузилось вовсе).
if ! grep -a -q "\[wm\] main loop start" "$LOG"; then
    tail -4 "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
    cp "$LOG" "$SHOT_DIR/serial-noboot.log"
    echo "== stage 51.4 app: $PASS passed, $FAIL failed, $SKIP skipped (the system did not boot) =="
    exit 1
fi
sleep 2
term; mark; cmd "summon Rune" 2
check "the window opens on one core"                 'wait_new "[rune-ui] open, console" 10'
PROGS=$(last_new "[rune-ui] programs:")
check "abacus is selected"                           'pick abacus.wasm'
set_field args 0 "6*7"
lib_layout; mark; click_at "$LL" start
check "the program runs on the main core and ends"   'wait_new "[rune] run abacus.wasm | dir /disk/nexus, main core, limit 10000 ms" 10 && wait_end "exit 0" 15'
check "the window shows its output = gcc"            'same_log "$REF/abacus-arg.txt"'
sleep 0.5; shot 11-one-core
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-1c.log"
stop_qemu

echo "== stage 51.4 app: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
