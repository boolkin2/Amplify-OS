#!/bin/bash
# ============================================================================
# test_stage41_2_input.sh — подэтап 41.2: virtio-input вместо PS/2.
# ----------------------------------------------------------------------------
# QEMU с `-device virtio-keyboard-pci -device virtio-tablet-pci`. Проверяется:
#   1. оба устройства найдены, у каждого прерывание MSI-X (общий слой virtio);
#   2. вход в систему клавишами через virtio-клавиатуру (sendkey QEMU идёт в
#      неё, как только драйвер её активировал) — строка `[vinput] first key`;
#   3. планшет: абсолютное событие через QMP (input-send-event) ставит курсор
#      ровно в заданную точку экрана — строка `first absolute pointer event`;
#   4. курсор на экране действительно переехал (сравнение скриншотов);
#   5. щелчок планшетом по плитке rail открывает приложение;
#   6. запасной путь: без устройств virtio система грузится на PS/2.
# Запуск: scripts/test_stage41_2_input.sh   (узел Proxmox, KVM или TCG)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-41_2-input
WORK=/tmp/amplify-stage41_2-input
LOG="$WORK/serial.log"
LOG2="$WORK/serial-ps2.log"
MON_PORT=4585
QMP_PORT=4586

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}
    echo "  (no KVM access — TCG, timeouts x$SLOW)"
fi

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img "$SHOT_DIR"/*.ppm
gcc -O2 -o build/ppmdiff tools/ppmdiff.c || { echo "cannot build tools/ppmdiff.c"; exit 1; }

QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null; rm -f "$WORK"/*.img; true; }
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
drain() { while read -t 0.02 -r -u 3 _; do :; done; }
mon()   { echo "$1" >&3; sleep 0.05; drain; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
# QMP: отдельный сокет, JSON-команды. Ответы вычитываем, чтобы не копились.
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
# Абсолютное событие планшета: координаты 0..32767 по каждой оси.
abs_move()  { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}]}}"; }
abs_click() {
    qmp '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}'
    sleep 0.2
    qmp '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}'
}
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
shot()  { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 1; }
ndiff() { build/ppmdiff "$SHOT_DIR/$1.ppm" "$SHOT_DIR/$2.ppm" 2>/dev/null | sed -n 's/^diff=\(-\?[0-9]*\).*/\1/p'; }
start_qemu() {
    local log="$1"; shift
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -qmp "tcp:127.0.0.1:$QMP_PORT,server,nowait" \
        -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
        -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null
}
login() {
    wait_log "$1" "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}

cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
# perf.conf: строка [fstat] раз в секунду — по ней видно число команд
# MOVE_CURSOR (hwmoves). mtools: -D s / -D o и </dev/null (CLAUDE.md §5.13).
export MTOOLS_SKIP_CHECK=1
mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
printf 'fstat=1\n' > "$WORK/perf.conf"
mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null

# ===========================================================================
echo "== Phase 1: virtio-keyboard + virtio-tablet =="
start_qemu "$LOG" -device virtio-keyboard-pci -device virtio-tablet-pci
qmp_connect || echo "  (QMP port never opened)"

check "two virtio-input devices found"        'wait_log "$LOG" "[vinput] devices: 2" $((60*SLOW))'
check "virtio-gpu commands complete by MSI-X" 'wait_log "$LOG" "[vgpu] controlq completion by MSI-X interrupt: OK" $((30*SLOW))'
check "keyboard on MSI-X"                     'grep -a "\[vinput\].*(keyboard" "$LOG" | grep -q "irq=MSI-X"'
check "tablet reports absolute axes on MSI-X" 'grep -a "\[vinput\].*(tablet, abs max=32767x32767" "$LOG" | grep -q "irq=MSI-X"'
login "$LOG"
check "login typed through virtio-keyboard"   'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
check "keys arrived via virtio-input"         'wait_log "$LOG" "[vinput] first key event" 5'
sleep 2; shot 01-desktop

# Центр экрана: 16384/32767 * 1919 = 959, 16384/32767 * 1079 = 539.
abs_move 16384 16384
check "tablet event reached the kernel"       'wait_log "$LOG" "[vinput] first absolute pointer event at" 5'
POS=$(grep -a -m1 "first absolute pointer event at" "$LOG" | tr -d '\r' | sed 's/.* at //')
check "pointer lands at the screen centre (got $POS)" '[ "$POS" = "959,539" ]'
# С планшетом WM включает аппаратный курсор: в кадре (и на скриншоте QEMU) его
# нет, сцена при движении не меняется, а в очередь курсора уходят MOVE_CURSOR.
check "hardware cursor enabled with the tablet" 'grep -a -q "\[wm\] hardware cursor: virtio-gpu cursor queue" "$LOG"'
sleep 1; shot 02-centre
M=$(wc -l < "$LOG")
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do abs_move $((4000 + i*2000)) $((3000 + i*1500)); done
sleep 2; shot 03-moved
D=$(ndiff 02-centre 03-moved)
check "pointer motion does not touch the frame (diff=$D px)"  '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'
HW=$(tail -n +"$((M+1))" "$LOG" | grep -a -o "hwmoves=[0-9]*" | cut -d= -f2 | sort -n | tail -1)
check "MOVE_CURSOR commands sent (max ${HW:-0}/s)"            '[ "${HW:-0}" -ge 1 ]'
SC=$(tail -n +"$((M+1))" "$LOG" | grep -a "\[fstat\] fps" | sed -n 's/.* scene=\([0-9]*\)\/.*/\1/p' | sort -n | head -1)
check "scene is not redrawn while the pointer moves"          '[ "${SC:-1}" = 0 ]'

# --- Перетаскивание окна: цель подэтапа — >= 60 кадров/с при движении ОКОН. --
# Кнопка About на рабочем столе (логические 438..516 x 366..398 -> экран
# ~(611, 413)) открывает классическое окно 340x140 в (120, 80); его заголовок —
# y 80..108. Тащим за заголовок ~80 событий/с и смотрим счётчики кадров.
px2abs_x() { echo $(( $1 * 32767 / 1919 )); }
px2abs_y() { echo $(( $1 * 32767 / 1079 )); }
abs_fast() { echo "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(px2abs_x $1)}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(px2abs_y $2)}}]}}" >&4; }
abs_move "$(px2abs_x 611)" "$(px2abs_y 413)"; sleep 0.5; abs_click; sleep 1.5; shot 03b-about
D=$(ndiff 03-moved 03b-about)
check "About window opened by a tablet click (diff=$D px)"   '[ "${D:--1}" -gt 10000 ]'
abs_move "$(px2abs_x 200)" "$(px2abs_y 92)"; sleep 0.3
qmp '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}'
M=$(wc -l < "$LOG")
END=$(( $(date +%s) + 6 )); i=0; x=200; y=92; dx=7; dy=3
while [ "$(date +%s)" -lt "$END" ]; do
    x=$((x+dx)); y=$((y+dy)); abs_fast $x $y; i=$((i+1))
    if [ $((i % 60)) = 0 ]; then dx=$((-dx)); dy=$((-dy)); while read -t 0.01 -r -u 4 _; do :; done; fi
    sleep 0.01
done
while read -t 0.05 -r -u 4 _; do :; done
qmp '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}'
DRAG=$(tail -n +"$((M+1))" "$LOG" | grep -a "\[fstat\] fps" | tr -d '\r' | sed '1d;$d')
echo "$DRAG" | tail -1 | cut -c1-200 | sed 's/^/        /'
DF=$(echo "$DRAG" | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p' | sort -n | head -1)
DS=$(echo "$DRAG" | sed -n 's/.* frame=\([0-9]*\)\/.*/\1/p' | sort -n | tail -1)
if [ "$SLOW" = 1 ]; then
    check "window drag: >= 60 frames/s (min ${DF:-?})"       '[ "${DF:-0}" -ge 60 ]'
    check "window drag: frame < 8 ms (max avg ${DS:-?} us)"   '[ "${DS:-99999}" -lt 8000 ]'
else
    echo "  [SKIP] window drag frame rate (TCG)"
fi
sleep 1; shot 03c-dragged
D=$(ndiff 03b-about 03c-dragged)
check "the window really moved (diff=$D px)"                  '[ "${D:--1}" -gt 1000 ]'

# Щелчок по плитке Terminal в rail (логическая сетка 1600x1000: слот 2 —
# x=10..54, y=216..268; на 1920x1080 сетка 1728x1080 с отступом 96 слева):
# экран ~ (131, 261) -> 0..32767: x=2237, y=7925.
abs_move 2237 7925; sleep 0.5; abs_click; sleep 2; shot 04-clicked
D=$(ndiff 03c-dragged 04-clicked)
check "tablet click on the rail opens an app (diff=$D px)" '[ "${D:--1}" -gt 100000 ]'
check "no kernel panic"                        '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-virtio.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: no virtio-input devices -> PS/2 fallback =="
start_qemu "$LOG2"
check "driver reports no devices"             'wait_log "$LOG2" "[vinput] no virtio-input devices" $((60*SLOW))'
login "$LOG2"
check "login through PS/2 keyboard"           'wait_log "$LOG2" "[wm] main loop start" $((60*SLOW))'
check "no virtio key events on the PS/2 path" '! grep -a -q "\[vinput\] first key event" "$LOG2"'
cp "$LOG2" "$SHOT_DIR/serial-ps2.log"
stop_qemu

echo "== stage 41.2 input: $PASS passed, $FAIL failed =="
[ $FAIL = 0 ]
