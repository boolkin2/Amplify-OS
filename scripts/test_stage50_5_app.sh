#!/bin/bash
# ============================================================================
# test_stage50_5_app.sh — подэтап 50.5: приложение Phantom (окно гостя).
# ----------------------------------------------------------------------------
# Всё делается так, как это делает человек: мышью по окну (QMP, координаты —
# из строк `[phantom-ui] layout …`) и клавишами (sendkey -> клавиатура хоста
# -> перехват скан-кодов окном -> контроллер 8042 гостя).
#
# Что пишет гость, тест узнаёт не с экрана, а от самого гостя: команда,
# набранная в его оболочке на tty1, пишет метку в /dev/ttyS0 — это
# последовательный порт гостя, его вывод ядро выносит в serial строками
# `[guest] …`. Так доказывается весь путь клавиши: хост -> окно -> 8042 ->
# ядро Linux -> оболочка.
#
# Фаза 1 (одна загрузка, 4 ядра, VT-x):
#   библиотека образов; TinyCore: чтение образа без заморозки интерфейса,
#   клавиши в загрузчик (BIOS INT 09h/16h), клавиши в Linux (i8042, Shift,
#   стрелки), возврат клавиатуры правым Ctrl, закрытие и повторное открытие
#   окна при работающем госте, Ctrl+Alt+Del, «Закрыть гостя»;
#   Alpine: строка загрузчика из поля, вход root с клавиатуры окна на tty1,
#   AMPLIFY MIND отвечает при работающем госте (ids = эталон), гость жив.
# Фаза 2 (-vmx): окно честно говорит, что VT-x нет.
# Фаза 3: образов на диске нет, но в приводе — установочный ISO AMPLIFY-OS
#   (build/myos-x86_64.iso, на нём /guests/*.iso): окно показывает образы с
#   носителя и запускает гостя прямо с CD. Так устроен стенд (ВМ 402).
# Нужны KVM с вложенной виртуализацией и оба образа в build/guests (§2).
# Запуск: scripts/test_stage50_5_app.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
ISO_TC=build/guests/CorePure64-17.1.iso
ISO_AL=build/guests/alpine-virt-3.24.2-x86_64.iso
GOLD=scripts/stage49_golden.txt
OS_ISO=build/myos-x86_64.iso
SHOT_DIR=build/screens-50_5
WORK=/tmp/amplify-stage50_5
MON_PORT=4656
QMP_PORT=4657

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
gold() { grep "^$1=" "$GOLD" | head -1 | cut -d= -f2-; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK" "$GOLD"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

KVM=0
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then KVM=1; fi
NESTED=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null)
case "$NESTED" in Y|1) NESTED=1;; *) NESTED=0;; esac
if [ $KVM != 1 ] || [ $NESTED != 1 ]; then
    echo "  [SKIP] needs KVM with nested virtualization (kvm=$KVM nested=$NESTED); TCG has no VMX"
    echo "== stage 50.5 app: 0 passed, 0 failed, 1 skipped =="
    exit 0
fi
for f in "$ISO_TC" "$ISO_AL"; do
    if [ ! -f "$f" ]; then
        echo "  [SKIP] $f not found — copy the guest ISO there (scp -O from the PC)"
        echo "== stage 50.5 app: 0 passed, 0 failed, 1 skipped =="
        exit 0
    fi
done

# Один экземпляр: общий рабочий каталог и порты (CLAUDE.md §5.36).
if pgrep -f "[q]emu-system.*$WORK/" >/dev/null 2>&1; then
    echo "another run of this test is still alive (QEMU with $WORK) — wait for it or kill it"
    exit 1
fi
mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.log
gcc -O2 -o build/ppmdiff tools/ppmdiff.c || { echo "cannot build tools/ppmdiff.c"; exit 1; }

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
        '$') echo shift-4;; '(') echo shift-9;; ')') echo shift-0;; '*') echo shift-8;;
        '>') echo shift-dot;; '_') echo shift-minus;; ':') echo shift-semicolon;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.09; done; }
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
# То же без учёта регистра: mcopy кладёт файл с именем 8.3, и FAT отдаёт его
# заглавными буквами (CORE.ISO).
wait_new_i() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        since | grep -a -q -i -F "$1" && return 0
        sleep 0.3
    done
    return 1
}
wait_new_re() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        since | grep -a -q -E "$1" && return 0
        sleep 0.3
    done
    return 1
}
last_new() { since | grep -a -F "$1" | tail -1; }
shot()  { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 1; }
ndiff() { build/ppmdiff "$SHOT_DIR/$1.ppm" "$SHOT_DIR/$2.ppm" 2>/dev/null | sed -n 's/^diff=\(-\?[0-9]*\).*/\1/p'; }
ids_of() { since | grep -a "\[mind\] ids: " | sed -n "${1}p" | sed 's/.*ids: //'; }

# make_disk [noimages] — без образов гостей на диске (фаза 3)
make_disk() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum ::/vault ::/vault/phantom </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    # Модели MIND на диске нет: работает встроенная stories260K, её ответ на
    # «Once upon a time» при greedy известен заранее (эталон этапа 49).
    printf 'greedy=1\nmax_tokens=32\n' > "$WORK/mind.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/mind.conf" ::/Sanctum/mind.conf </dev/null
    [ "${1:-}" = noimages ] && return 0
    # Образы кладутся КАК ЕСТЬ, ни один байт в них не меняется.
    mcopy -D o -i "$WORK/dev.img" "$ISO_TC" ::/vault/phantom/core.iso </dev/null
    mcopy -D o -i "$WORK/dev.img" "$ISO_AL" ::/vault/phantom/alpine.iso </dev/null
}
# start_qemu <log> <модель процессора> [доп. аргументы QEMU]
start_qemu() {
    local log="$1" cpu="$2"; shift 2
    qemu-system-x86_64 -bios "$OVMF" -enable-kvm -cpu "$cpu" -machine pc -m 3G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -qmp "tcp:127.0.0.1:$QMP_PORT,server,nowait" \
        -device virtio-keyboard-pci -device virtio-tablet-pci \
        -drive file="$WORK/dev.img",format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0,unit=0 \
        -drive file="$WORK/esp.img",format=raw,if=none,id=d1 -device ide-hd,drive=d1,bus=ide.0,unit=1,bootindex=0 \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
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
# Какой строкой списка идёт образ (0..): порядок — как в каталоге диска.
row_of() { echo "$IMG" | sed 's/.*images: [0-9]* - //' | tr ';' '\n' | grep -n -i -F "$1" | head -1 | cut -d: -f1; }
guest_health='! grep -a -q -E "\[guest\].*(Oops|Kernel panic|general protection|BUG:)" "$LOG"'
# Наименьшая частота кадров между двумя строками журнала (по строкам [fstat]).
min_fps() { awk -v a="$1" -v b="$2" 'NR > a && NR < b && /\[fstat\] fps=/ { sub(/.*fps=/, ""); sub(/\..*/, ""); if (m == "" || $0 + 0 < m) m = $0 + 0 } END { print m }' "$LOG"; }
line_of() { grep -a -n -F "$1" "$LOG" | tail -1 | cut -d: -f1; }

# ===========================================================================
echo "== Phase 1: the Phantom window: library, TinyCore, Alpine, MIND next to the guest =="
LOG="$WORK/serial-app.log"
make_disk
start_qemu "$LOG" host
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
sleep 2
key meta_l-spc 2
mark; cmd "summon Phantom" 2
check "the Phantom window opens (summon Phantom)"    'wait_new "[phantom-ui] open" 10'
IMG=$(last_new "[phantom-ui] images:")
echo "        ${IMG#\[phantom-ui\] }" | cut -c1-200
check "library lists both images from /vault/phantom" 'echo "$IMG" | grep -q "images: 2 " && echo "$IMG" | grep -q -i "core.iso (26294272 bytes, disk)" && echo "$IMG" | grep -q -i "alpine.iso (69206016 bytes, disk)"'
sleep 1
LL=$(last_new "[phantom-ui] layout view=library")
check "library reported its layout"                  '[ -n "$LL" ]'
shot 01-library

# ---- TinyCore ---------------------------------------------------------------
R=$(row_of core.iso)
# Не нашли строку — проверка падает здесь, а не «выбирается первая»: иначе
# дальше под именем одного гостя молча запустится другой.
check "TinyCore is in the list"                      '[ -n "$R" ]'
R=$(( ${R:-1} - 1 ))
mark; click_at "$LL" "img$R"
check "click selects TinyCore in the list"           'wait_new_i "[phantom-ui] selected core.iso" 5'
mark; click_at "$LL" start
check "«Запустить» starts reading the image"         'wait_new_i "[phantom] loading /disk/vault/phantom/core.iso" 5'
check "window switched to the loading view"          'wait_new "[phantom-ui] state loading" 5'
sleep 1.5; shot 02-loading
check "guest started from the window"                'wait_new "[phantom] bios boot:" 120'
last_new "[phantom] bios boot:" | sed 's/.*; image read/        image read/' | cut -c1-60
A=$(line_of "[phantom] loading"); B=$(line_of "[phantom] bios boot:")
FPS=$(min_fps "${A:-0}" "${B:-0}")
echo "        frames while the image was being read: min ${FPS:-none within a full second} per second"
check "interface keeps drawing while the image is read (>= 5 frames/s)" '[ -z "$FPS" ] || [ "$FPS" -ge 5 ]'
check "window shows the running guest"               'wait_new "[phantom-ui] state running" 10'
check "keyboard goes to the guest right after start" 'wait_new "[phantom-ui] keyboard: guest" 10'
sleep 1
CL=$(last_new "[phantom-ui] layout view=console")
echo "        ${CL#\[phantom-ui\] }" | cut -c1-160
check "console reported its layout and font"         'echo "$CL" | grep -q "cell=16x32 size=1280x800"'
check "boot loader shows its prompt (boot:)"         'wait_new_re "^\[guest\] boot:" 30'
shot 03-bootprompt
mark; cmd "corepure64 console=ttyS0 console=tty0 loglevel=7 rdinit=/bin/sh" 1
check "keys typed in the window reach the boot loader (BIOS INT 09h/16h)" 'wait_new_re "\[guest\].*Command line:.*console=ttyS0 console=tty0 loglevel=7 rdinit=/bin/sh" 60'
check "guest kernel found the keyboard controller (i8042)" 'wait_new_re "\[guest\].*serio: i8042 KBD port at 0x60,0x64 irq 1" 60'
check "guest kernel found the keyboard"              'wait_new_re "\[guest\].*input: AT Translated Set 2 keyboard" 30'
check "guest reached its shell on tty1"              'wait_new_re "\[guest\].*Run /bin/sh as init process" 60'
since | grep -a -E '\[guest\].*(i8042|AT Translated|PS/2)' | cut -c1-120 | head -5 | sed 's/^/        /'
sleep 2
cmd "mount -t devtmpfs d /dev" 1
cmd "mount -t proc p /proc" 1
mark; cmd 'echo K5-$((6*7)) >/dev/ttyS0' 1
check "typing in the window reaches the guest's shell (i8042 -> Linux, with Shift)" 'wait_new "[guest] K5-42" 15'
key up 0.4; key ret 1
check "arrow keys work in the guest (Up recalls the command)" '[ "$(since | grep -a -c -F "[guest] K5-42")" -ge 2 ] || { sleep 3; [ "$(since | grep -a -c -F "[guest] K5-42")" -ge 2 ]; }'
mark; cmd "cat /proc/interrupts >/dev/ttyS0" 2
since | grep -a -E '^\[guest\] +[0-9]+: ' | cut -c1-110 | sed 's/^/        /'
check "keyboard interrupts arrive on IRQ1 through the 8259" 'since | grep -a -q -E "^\[guest\] +1: +[1-9][0-9]* .*i8042"'
check "guest kernel is healthy (no oops, no panic)"  "$guest_health"
shot 04-tinycore-shell

mark; key ctrl_r 0.6
check "right Ctrl returns the keyboard to the system" 'wait_new "[phantom-ui] keyboard: host" 5'
sleep 0.5; shot 05-before-typing
mark; click_at "$CL" screen
check "click on the guest screen gives it the keyboard" 'wait_new "[phantom-ui] keyboard: guest" 5'
cmd "echo text drawn in the window" 1.5
shot 06-after-typing
D=$(ndiff 05-before-typing 06-after-typing)
check "guest text is drawn in the window (frames differ by ${D:-?} px)" '[ "${D:-0}" -ge 300 ]'

mark; key ctrl_r 0.6; key esc 1.5
check "Esc closes the window"                        'wait_new "[phantom-ui] closed" 8'
mark; cmd "phantom status" 1.5
ST=$(last_new "[phantom] linux state=")
check "guest keeps running with the window closed"   'echo "$ST" | grep -q -E "state=(running|sleeping)"'
mark; cmd "summon Phantom" 2
check "window reopens on the running guest"          'wait_new "[phantom-ui] state running" 10'
sleep 1
CL=$(last_new "[phantom-ui] layout view=console")
mark; click_at "$CL" screen; sleep 0.5
cmd "echo K6 >/dev/ttyS0" 1
check "reopened window continues the same session"   'wait_new "[guest] K6" 15'
mark; click_at "$CL" cad
check "Ctrl+Alt+Del button sends the keys"           'wait_new "[phantom-ui] sent Ctrl+Alt+Del" 5'
check "guest restarts itself on Ctrl+Alt+Del (= the end of the guest)" 'wait_new "[phantom] guest ended: guest asked for a reset" 30'
check "window shows that the guest has ended"        'wait_new "[phantom-ui] state ended" 10'
sleep 1; shot 07-ended
mark; click_at "$CL" stop
check "«Закрыть гостя» removes the guest"            'wait_new "[phantom] linux stopped in" 10 && wait_new "[phantom-ui] state idle" 10'

# ---- Alpine -----------------------------------------------------------------
sleep 1.5
LL=$(last_new "[phantom-ui] layout view=library")
[ -n "$LL" ] || LL=$(grep -a -F "[phantom-ui] layout view=library" "$LOG" | tail -1 | tr -d '\r')
R=$(row_of alpine.iso)
check "Alpine is in the list"                        '[ -n "$R" ]'
R=$(( ${R:-1} - 1 ))
mark; click_at "$LL" "img$R"
check "click selects Alpine"                         'wait_new_i "[phantom-ui] selected alpine.iso" 5'
mark; click_at "$LL" line
check "click focuses the boot line field"            'wait_new "[phantom-ui] focus line" 5'
type_text "virt"
mark; click_at "$LL" start
check "boot line from the field is passed on"        'wait_new_i "[phantom-ui] start /disk/vault/phantom/alpine.iso line: virt" 5'
sleep 3; shot 08-loading-alpine
check "Alpine started from the window"               'wait_new "[phantom] bios boot:" 180'
last_new "[phantom] bios boot:" | sed 's/.*; image read/        image read/' | cut -c1-60
A=$(line_of "[phantom] loading"); B=$(line_of "[phantom] bios boot:")
FPS=$(min_fps "${A:-0}" "${B:-0}")
echo "        frames while the 66 MB image was being read: min ${FPS:-?} per second"
check "interface keeps drawing while 66 MB are read from the IDE disk (>= 5 frames/s)" '[ -n "$FPS" ] && [ "$FPS" -ge 5 ]'
check "keyboard goes to the guest"                   'wait_new "[phantom-ui] keyboard: guest" 10'
# Приглашение login появляется на tty1 — в serial его нет. Пробуем войти, пока
# оболочка не ответит меткой через свой последовательный порт.
sleep 15
mark; GOT=0
for _ in 1 2 3 4 5; do
    cmd "root" 2.5
    cmd 'echo A7-$((6*7)) >/dev/ttyS0' 1
    if wait_new "[guest] A7-42" 6; then GOT=1; break; fi
done
check "login as root on tty1 with the window keyboard" '[ $GOT = 1 ]'
mark; cmd "cat /etc/alpine-release >/dev/ttyS0" 1
check "it is Alpine 3.24.2"                          'wait_new_re "^\[guest\] 3\.24\.2[[:space:]]*$" 15'
mark; cmd "cat /proc/interrupts >/dev/ttyS0" 2
since | grep -a -E '^\[guest\] +[0-9]+: ' | cut -c1-110 | sed 's/^/        /'
check "Alpine: keyboard on IRQ1, disk on its virtio line" 'since | grep -a -q -E "^\[guest\] +1: +[1-9][0-9]* .*i8042" && since | grep -a -q -E "^\[guest\] +[0-9]+: +[1-9][0-9]* .*virtio"'
cmd "clear" 0.5
cmd "cat /etc/os-release" 1.5
shot 09-alpine

# ---- MIND рядом с гостем ---------------------------------------------------------
mark; key ctrl_r 0.6
check "keyboard back to the system"                  'wait_new "[phantom-ui] keyboard: host" 5'
mark; key ctrl-spc 1
check "Ctrl+Space opens AMPLIFY MIND over the guest window" 'wait_new "[mind] widget open" 10'
check "MIND loaded its builtin model"                'wait_new "[mind] loaded stories260K" 30 || grep -a -q "\[mind\] loaded stories260K" "$LOG"'
sleep 1
mark; type_text "Once upon a time"; key ret 0.4
check "MIND answers while the guest is running"      'wait_new "[mind] reply done" 60'
since | grep -a "\[mind\] reply" | cut -c1-140 | sed 's/^/        /'
check "MIND token ids = reference (computation is intact next to the guest)" '[ "$(ids_of 1)" = "$(gold STORIES_IDS)" ]'
sleep 1; shot 10-mind-over-guest
mark; key esc 1.5
check "Esc closes MIND"                              'wait_new "[mind] widget close" 10'
mark; click_at "$CL" screen
check "keyboard returns to the guest by a click"     'wait_new "[phantom-ui] keyboard: guest" 5'
cmd "echo A8 >/dev/ttyS0" 1
check "guest is alive and responsive after MIND"     'wait_new "[guest] A8" 15'
check "guest kernel is healthy (no oops, no panic)"  "$guest_health"

# Клавиатура у гостя, но Super остаётся системе: Super+Space открывает
# терминал поверх окна гостя, и окно само отдаёт клавиатуру.
mark; key meta_l-spc 2
check "Super+Space works while the guest holds the keyboard" 'wait_new "[phantom-ui] keyboard: host" 5'
cmd "phantom status" 1.5
sleep 4
mark; cmd "phantom status" 1.5
ST=$(last_new "[phantom] linux state=")
echo "        ${ST#\[phantom\] }" | cut -c1-330
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "idle guest leaves its core free (run < 30%)"  'echo "$ST" | grep -q -E "state=(running|sleeping)" && [ -n "$RUN" ] && [ "$RUN" -lt 30 ]'
mark; cmd "phantom stop" 1.5
check "phantom stop from the terminal removes the guest" 'wait_new "[phantom] linux stopped in" 10'
check "window notices it and returns to the library" 'wait_new "[phantom-ui] state idle" 10'
mark; cmd "phantom selftest" 1
check "selftest still passes after both guests"      'wait_new "[phantom] selftest: 9 passed, 0 failed" 90'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-app.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: no VT-x (-cpu host,-vmx): the window says so =="
LOG="$WORK/serial-novmx.log"
make_disk
start_qemu "$LOG" host,-vmx
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
sleep 2
key meta_l-spc 2
mark; cmd "summon Phantom" 2
check "the Phantom window opens without VT-x"        'wait_new "[phantom-ui] open" 10'
sleep 1
LL=$(last_new "[phantom-ui] layout view=library")
shot 11-no-vtx
mark; click_at "$LL" start
check "start is refused with the reason"             'wait_new "[phantom-ui] start refused: no usable VT-x" 5'
check "no guest appeared"                            '! since | grep -a -q "\[phantom\] loading"'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-novmx.log"
stop_qemu

# ===========================================================================
echo "== Phase 3: guest images straight from the AMPLIFY-OS installation medium =="
if [ ! -f "$OS_ISO" ]; then
    echo "  [SKIP] $OS_ISO not found (make ARCH=x86_64 iso)"; SKIP=$((SKIP+1))
elif ! xorriso -indev "$OS_ISO" -ls /guests 2>/dev/null | grep -q -i "CorePure64"; then
    echo "  [SKIP] $OS_ISO has no /guests (build/guests was empty when the ISO was built)"; SKIP=$((SKIP+1))
else
    LOG="$WORK/serial-medium.log"
    make_disk noimages
    # Загрузка — с диска (ESP), как у установленной системы; ISO просто стоит
    # в приводе. bootindex у ESP обязателен: иначе прошивка загрузит ISO.
    start_qemu "$LOG" host \
        -drive file="$OS_ISO",format=raw,if=none,id=cd,media=cdrom,readonly=on \
        -device ide-cd,drive=cd,bus=ide.1,unit=0
    login "$LOG"
    check "desktop reached (booted from the disk, not from the ISO)" 'wait_log "$LOG" "[wm] main loop start" 60 && ! grep -a -q "\[installer\] starting" "$LOG"'
    sleep 2
    key meta_l-spc 2
    mark; cmd "summon Phantom" 3
    check "the Phantom window opens"                 'wait_new "[phantom-ui] open" 15'
    IMG=$(last_new "[phantom-ui] images:")
    echo "        ${IMG#\[phantom-ui\] }" | cut -c1-200
    check "library lists the images of the medium (/guests)" 'echo "$IMG" | grep -q "CorePure64-17.1.iso (26294272 bytes, medium)" && echo "$IMG" | grep -q "alpine-virt-3.24.2-x86_64.iso (69206016 bytes, medium)"'
    sleep 1
    LL=$(last_new "[phantom-ui] layout view=library")
    shot 12-medium-library
    R=$(row_of CorePure64)
    check "TinyCore is in the list"                  '[ -n "$R" ]'
    R=$(( ${R:-1} - 1 ))
    mark; click_at "$LL" "img$R"; sleep 0.3
    click_at "$LL" start
    check "image is read from the medium"            'wait_new "[phantom] loading /cdrom/guests/CorePure64-17.1.iso: 26294272 bytes from the installation medium" 5'
    check "guest started from the CD image"          'wait_new "[phantom] bios boot:" 180'
    last_new "[phantom] bios boot:" | sed 's/.*; image read/        image read/' | cut -c1-60
    A=$(line_of "[phantom] loading"); B=$(line_of "[phantom] bios boot:")
    FPS=$(min_fps "${A:-0}" "${B:-0}")
    echo "        frames while the image was being read from the CD: min ${FPS:-?} per second"
    check "interface keeps drawing while the CD is read (>= 5 frames/s)" '[ -z "$FPS" ] || [ "$FPS" -ge 5 ]'
    check "its boot loader shows the prompt"         'wait_new_re "^\[guest\] boot:" 30'
    mark; key ret 1
    sleep 30
    key ctrl_r 0.6
    key meta_l-spc 2
    mark; cmd "phantom screen" 2
    since | grep -a '^\[screen\]' | cut -c1-90 | tail -4 | sed 's/^/        /'
    check "TinyCore booted by default to its shell (text screen)" 'since | grep -a -q -E "^\[screen\] .*tc@box"'
    shot 13-medium-guest
    mark; cmd "phantom stop" 1.5
    check "guest removed"                            'wait_new "[phantom] linux stopped in" 10'
    check "no kernel panic"                          '! grep -a -q "PANIC" "$LOG"'
    cp "$LOG" "$SHOT_DIR/serial-medium.log"
    stop_qemu
fi

echo "== stage 50.5 app: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
