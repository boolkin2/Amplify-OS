#!/bin/bash
# ============================================================================
# test_mouse_stress.sh — стресс-тест мыши.
# Очень быстро шлёт MANY mouse_move в QEMU monitor (имитация 1000 dpi мыши),
# периодически снимает state из serial dump и сравнивает что курсор
# реально двигается.
# ============================================================================
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
DISK_ORIG=build/disk-x86_64.img
DISK_TEST=build/disk-mousetest.img
SERIAL_LOG=build/serial-stress.log
MON_PORT=4572
SHOT_DIR=build

rm -f "$SERIAL_LOG"
rm -f $SHOT_DIR/stress-*.ppm 2>/dev/null

pkill -9 -f "monitor=tcp:127.0.0.1:$MON_PORT" 2>/dev/null
sleep 0.5

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
trap "kill -9 $QEMU_PID 2>/dev/null; true" EXIT

for i in {1..50}; do
    if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then break; fi
    sleep 0.1
done
sleep 0.3
while read -t 0.1 -r -u 3 _; do :; done

send_mon() { echo "$1" >&3; }
flush_mon() { while read -t 0.1 -r -u 3 _; do :; done; }

type_str() {
    local s="$1"
    local i ch
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:i:1}"
        send_mon "sendkey $ch"
        sleep 0.04
    done
}

wait_log() {
    local pattern="$1"
    local timeout="${2:-30}"
    local end=$(( $(date +%s) + timeout ))
    while [ $(date +%s) -lt $end ]; do
        if grep -a -q -F "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.4
    done
    return 1
}

shot() {
    local name="$1"
    send_mon "screendump $SHOT_DIR/stress-$name.ppm"
    sleep 0.8
    flush_mon
}

if ! wait_log "[doublebuf] initialized" 60; then
    echo "boot fail"; exit 1
fi

# login (USERS.TXT уже есть: admin/test)
sleep 5
type_str "admin"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "test"; sleep 0.3
send_mon "sendkey ret"

if ! wait_log "[wm] main loop start" 30; then
    echo "wm failed to start"
    tail -40 "$SERIAL_LOG"
    exit 2
fi
sleep 4
flush_mon

echo "=== before stress ==="
shot "00-start"

# STRESS 1: rapid-fire mouse_move без задержек.
echo "=== STRESS BATCH A: 200 fast mouse_move (10 10) ==="
for n in {1..200}; do
    send_mon "mouse_move 10 10"
done
sleep 2
flush_mon
shot "01-stressA"

echo "=== STRESS BATCH B: 200 fast mouse_move (-15 -10) ==="
for n in {1..200}; do
    send_mon "mouse_move -15 -10"
done
sleep 2
flush_mon
shot "02-stressB"

echo "=== STRESS BATCH C: tiny mouse_moves to specific target ==="
for n in {1..50}; do
    send_mon "mouse_move 2 2"
done
sleep 2
flush_mon
shot "03-stressC"

# Тут попробуем "сесть на месте" — нет команд, проверим что система не зависла.
echo "=== IDLE 3 sec ==="
sleep 3
shot "04-idle"

# Финальная попытка движения — если зависло, курсор не сдвинется.
echo "=== POST-IDLE MOVE ==="
for n in {1..10}; do
    send_mon "mouse_move 30 0"
    sleep 0.2
done
sleep 2
shot "05-postmove"

send_mon "quit"
sleep 0.5
kill -9 $QEMU_PID 2>/dev/null

echo
echo "=== MOUSE DBG (last 25) ==="
grep -a -F "[mouse-dbg]" "$SERIAL_LOG" | tail -25

echo
echo "=== screenshots md5 ==="
md5sum $SHOT_DIR/stress-*.ppm 2>/dev/null
