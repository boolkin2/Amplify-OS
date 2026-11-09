#!/bin/bash
# ============================================================================
# test_mouse_fix.sh — диагностика регрессии мыши после этапа 42.
# Автоматически проходит firstboot (создаёт admin) → login → WM.
# Затем посылает mouse_move через monitor, делает screenshot и проверяет
# счётчики mouse_debug_dump в serial.
# ============================================================================
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
DISK_ORIG=build/disk-x86_64.img
DISK_TEST=build/disk-mousetest.img
SERIAL_LOG=build/serial-mousetest.log
MON_PORT=4571
SHOT_DIR=build

# Удалили USERS.TXT в build/disk-mousetest.img заранее → firstboot запустится.

rm -f "$SERIAL_LOG"
rm -f $SHOT_DIR/mouse-*.ppm 2>/dev/null

pkill -9 -f "monitor=tcp:127.0.0.1:$MON_PORT" 2>/dev/null
sleep 0.3

echo "[test] starting QEMU on monitor :$MON_PORT"
qemu-system-x86_64 \
    -cdrom "$ISO" -boot d -m 256M \
    -display none -vga std \
    -serial "file:$SERIAL_LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive "file=$DISK_TEST,format=raw,if=ide,index=0,media=disk" \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 \
    -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
echo "[test] qemu pid=$QEMU_PID"
trap "kill -9 $QEMU_PID 2>/dev/null; true" EXIT

# Подключаемся к монитору.
for i in {1..50}; do
    if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if ! exec 3<&3 2>/dev/null; then
    echo "[test] monitor never opened"
    exit 1
fi
sleep 0.3
while read -t 0.1 -r -u 3 _; do :; done

send_mon() {
    echo "$1" >&3
    sleep 0.06
    while read -t 0.2 -r -u 3 _; do :; done
}

# Шлёт строку как последовательность sendkey.
type_str() {
    local s="$1"
    local i ch
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:i:1}"
        case "$ch" in
            [a-zA-Z]) send_mon "sendkey $ch" ;;
            [0-9])    send_mon "sendkey $ch" ;;
            _)        send_mon "sendkey shift-minus" ;;
            -)        send_mon "sendkey minus" ;;
            .)        send_mon "sendkey dot" ;;
        esac
        sleep 0.05
    done
}

wait_log() {
    local pattern="$1"
    local timeout="${2:-30}"
    local end=$(( $(date +%s) + timeout ))
    while [ $(date +%s) -lt $end ]; do
        if grep -q -F "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.4
    done
    return 1
}

shot() {
    local name="$1"
    local p="$SHOT_DIR/mouse-$name.ppm"
    send_mon "screendump $p"
    sleep 0.8
    [ -f "$p" ] && echo "[test] shot $p ($(stat -c%s "$p") bytes)" || echo "[test] SHOT FAILED: $p"
}

echo "[test] waiting for splash..."
if ! wait_log "[doublebuf] initialized" 40; then
    echo "[test] timeout waiting for doublebuf"
    tail -30 "$SERIAL_LOG"
    exit 2
fi

# Если в образе уже есть USERS.TXT (admin), firstboot пропускается;
# идём сразу в login. Иначе — проходим firstboot.
if grep -q "loaded.*record" "$SERIAL_LOG" 2>/dev/null; then
    echo "[test] firstboot skipped — going straight to login"
    sleep 4
else
    sleep 3
    echo "[test] firstboot welcome → Enter"
    send_mon "sendkey ret"; sleep 1.5
    echo "[test] firstboot policy → Enter"
    send_mon "sendkey ret"; sleep 1.5
    echo "[test] firstboot: type name=admin"
    type_str "admin"; sleep 0.3
    send_mon "sendkey tab"; sleep 0.3
    type_str "test"; sleep 0.3
    send_mon "sendkey tab"; sleep 0.3
    type_str "test"; sleep 0.3
    send_mon "sendkey ret"
    if ! wait_log "[firstboot] done" 15; then
        echo "[test] firstboot did not complete"
        tail -30 "$SERIAL_LOG"
        exit 3
    fi
    sleep 5
fi

# === Login ===
echo "[test] login: type admin"
type_str "admin"; sleep 0.4
send_mon "sendkey tab"; sleep 0.4
echo "[test] login: type password"
type_str "test"; sleep 0.4
send_mon "sendkey ret"

echo "[test] waiting for [wm] main loop start..."
if ! wait_log "[wm] main loop start" 30; then
    echo "[test] TIMEOUT waiting for WM"
    tail -60 "$SERIAL_LOG"
    exit 3
fi
echo "[test] WM is up"

# Дать About анимироваться.
sleep 4

# Снимок ДО.
shot "01-before"

# Сейчас попробуем подвинуть мышь МНОГО раз с большими шагами.
echo "[test] === MOUSE MOVE BATCH 1 ==="
for n in {1..15}; do
    send_mon "mouse_move 100 80"
    sleep 0.1
done
sleep 2
shot "02-moved-DR"

echo "[test] === MOUSE MOVE BATCH 2 (reverse) ==="
for n in {1..15}; do
    send_mon "mouse_move -90 -60"
    sleep 0.1
done
sleep 2
shot "03-moved-UL"

echo "[test] === MOUSE MOVE BATCH 3 (small steady) ==="
for n in {1..30}; do
    send_mon "mouse_move 5 5"
    sleep 0.05
done
sleep 2
shot "04-final"

# Финальный дамп.
sleep 1

send_mon "quit"
sleep 0.5
kill -9 $QEMU_PID 2>/dev/null

echo
echo "[test] === MOUSE DEBUG DUMPS FROM SERIAL ==="
grep -F "[mouse-dbg]" "$SERIAL_LOG" | tail -20

echo
echo "[test] === SCREENSHOTS ==="
for f in mouse-01-before mouse-02-moved-DR mouse-03-moved-UL mouse-04-final; do
    if [ -f "$SHOT_DIR/$f.ppm" ]; then
        h=$(md5sum "$SHOT_DIR/$f.ppm" | cut -d' ' -f1)
        echo "  $f.ppm md5=$h"
    fi
done

echo
echo "[test] === RELEVANT SERIAL TAIL ==="
tail -20 "$SERIAL_LOG"
