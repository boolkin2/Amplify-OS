#!/bin/bash
# ============================================================================
# test_mouse_ultra.sh — экстремальный стресс мыши.
# Шлёт 2000 mouse_move так быстро, как может, без задержек между ними.
# Имитирует флуд событий, который происходит когда пользователь
# быстро дёргает реальной мышью.
# ============================================================================
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
DISK_TEST=build/disk-mousetest.img
SERIAL_LOG=build/serial-ultra.log
MON_PORT=4573
SHOT_DIR=build

rm -f "$SERIAL_LOG"
rm -f $SHOT_DIR/ultra-*.ppm 2>/dev/null

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
type_str() {
    local s="$1" i ch
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:i:1}"
        send_mon "sendkey $ch"
        sleep 0.04
    done
}
wait_log() {
    local end=$(( $(date +%s) + ${2:-30} ))
    while [ $(date +%s) -lt $end ]; do
        grep -a -q -F "$1" "$SERIAL_LOG" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}
shot() {
    send_mon "screendump $SHOT_DIR/ultra-$1.ppm"
    sleep 0.8
    while read -t 0.1 -r -u 3 _; do :; done
}

wait_log "[doublebuf] initialized" 60 || { echo BOOT_FAIL; exit 1; }
sleep 5
type_str "admin"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "test"; sleep 0.3
send_mon "sendkey ret"
wait_log "[wm] main loop start" 30 || { echo WM_FAIL; exit 2; }
sleep 4
while read -t 0.1 -r -u 3 _; do :; done

shot "00-start"

# === RAW SPAM: 2000 events with NO sleep at all ===
echo "=== ULTRA STRESS: 2000 mouse_move raw spam, no sleep ==="
{
    for n in $(seq 1 2000); do
        # Чередуем направления, чтобы курсор оставался в видимой области.
        if (( n % 2 )); then
            echo "mouse_move 5 3"
        else
            echo "mouse_move -3 -2"
        fi
    done
} >&3
sleep 3
while read -t 0.1 -r -u 3 _; do :; done

shot "01-after-ultra"

# Дать системе восстановиться, потом маленькое движение.
sleep 2
for n in {1..5}; do send_mon "mouse_move 20 0"; sleep 0.2; done
sleep 1
shot "02-final-move"

# Финальный idle dump
sleep 1
shot "03-idle"

send_mon "quit"
sleep 0.5
kill -9 $QEMU_PID 2>/dev/null

echo
echo "=== ALL mouse-dbg dumps ==="
grep -a -F "[mouse-dbg]" "$SERIAL_LOG"

echo
echo "=== md5 of shots ==="
md5sum $SHOT_DIR/ultra-*.ppm 2>/dev/null
