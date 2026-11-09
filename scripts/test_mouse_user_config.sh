#!/bin/bash
# ============================================================================
# Воспроизводим точную конфигурацию пользователя: UEFI + SMP=4 + 1GB.
# ============================================================================
cd "$(dirname "$0")/.."

DISK_IMG=build/disk-mousetest.img
ESP_IMG=build/esp.img
SERIAL_LOG=build/serial-user.log
MON_PORT=4574
SHOT_DIR=build

# Находим OVMF.
OVMF=""
for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    if [ -f "$p" ]; then OVMF="$p"; break; fi
done
if [ -z "$OVMF" ]; then echo "OVMF not found"; exit 1; fi
echo "OVMF: $OVMF"

rm -f "$SERIAL_LOG"
rm -f $SHOT_DIR/user-*.ppm 2>/dev/null

pkill -9 -f "monitor=tcp:127.0.0.1:$MON_PORT" 2>/dev/null
sleep 0.5

qemu-system-x86_64 \
    -bios "$OVMF" \
    -machine pc \
    -drive format=raw,file=$DISK_IMG,if=ide,index=0,media=disk \
    -drive format=raw,file=$ESP_IMG,if=ide,index=1 \
    -m 1G -smp cpus=4,cores=4,threads=1 \
    -display none \
    -vga std \
    -serial "file:$SERIAL_LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 \
    -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
trap "kill -9 $QEMU_PID 2>/dev/null; true" EXIT
echo "qemu pid=$QEMU_PID"

for i in {1..50}; do
    exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null && break
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
    send_mon "screendump $SHOT_DIR/user-$1.ppm"
    sleep 0.8
    while read -t 0.1 -r -u 3 _; do :; done
}

echo "[test] waiting for boot..."
if ! wait_log "[doublebuf] initialized" 90; then
    echo "BOOT_FAIL"
    tail -50 "$SERIAL_LOG"
    exit 1
fi

sleep 5
echo "[test] login"
type_str "admin"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "test"; sleep 0.3
send_mon "sendkey ret"

if ! wait_log "[wm] main loop start" 40; then
    echo "WM_FAIL"
    tail -50 "$SERIAL_LOG"
    exit 2
fi
echo "[test] WM started"
sleep 5

shot "00-start"

echo "=== batch 1: 100 small moves DR ==="
for n in {1..100}; do
    send_mon "mouse_move 8 6"
    sleep 0.02
done
sleep 2
shot "01-batch1"

echo "=== batch 2: 100 reverse ==="
for n in {1..100}; do
    send_mon "mouse_move -10 -8"
    sleep 0.02
done
sleep 2
shot "02-batch2"

echo "=== batch 3: 50 small spiral ==="
for n in {1..50}; do
    send_mon "mouse_move 5 3"
    sleep 0.05
done
sleep 2
shot "03-batch3"

send_mon "quit"
sleep 0.5
kill -9 $QEMU_PID 2>/dev/null

echo
echo "=== mouse-dbg dumps (last 25) ==="
grep -a "mouse-dbg" "$SERIAL_LOG" | tail -25

echo
echo "=== md5 ==="
md5sum $SHOT_DIR/user-*.ppm 2>/dev/null
