#!/bin/bash
# ============================================================================
# test_c5_splash_bash.sh — bash-вариант test_c5_splash.py.
# Запускает QEMU в headless-режиме, использует TCP-монитор (вместо unix-socket,
# который python-версия) и встроенный в bash /dev/tcp/ для отправки команд.
# PPM-скриншоты конвертируются в PNG через Windows-python с Pillow.
# ============================================================================
set -e
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
DISK_ORIG=build/disk-x86_64.img
DISK_TEST=build/disk-c5-test.img
SERIAL_LOG=build/serial-c5.log
MON_PORT=4567
SHOT_DIR=build

PYEXE="/mnt/c/Users/manoo/AppData/Local/Python/bin/python.exe"

rm -f "$SERIAL_LOG" "$DISK_TEST"
rm -f $SHOT_DIR/c5-*.ppm $SHOT_DIR/c5-*.png 2>/dev/null || true
cp "$DISK_ORIG" "$DISK_TEST"

echo "[test] starting QEMU (monitor TCP :$MON_PORT)..."
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

# Ждём, пока монитор откроет порт.
for i in {1..50}; do
    if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ -z "$i" ]; then echo "[test] monitor port never opened"; exit 1; fi

# Сожрём приветствие монитора.
sleep 0.3
read -t 0.2 -r -u 3 _ || true
read -t 0.2 -r -u 3 _ || true

send_mon() {
    echo "$1" >&3
    sleep 0.05
    read -t 0.2 -r -u 3 _ || true
}

wait_log() {
    local pattern="$1"
    local timeout="${2:-15}"
    local end=$(( $(date +%s) + timeout ))
    while [ $(date +%s) -lt $end ]; do
        if [ -f "$SERIAL_LOG" ] && grep -q -F "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

shot() {
    local name="$1"
    local p="$SHOT_DIR/c5-$name.ppm"
    send_mon "screendump $p"
    sleep 0.8
    if [ -f "$p" ]; then
        "$PYEXE" -c "
from PIL import Image
import sys
Image.open(r'$p').save(r'$SHOT_DIR/c5-$name.png')
print('  + c5-$name.png')
" 2>&1 || echo "  ! PIL failed for $name"
    else
        echo "  ! no PPM produced for $name"
    fi
}

# Эмуляция набора текста через `sendkey`.
type_str() {
    local s="$1"
    local i ch low
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:$i:1}"
        case "$ch" in
            ' ') send_mon "sendkey spc" ;;
            [A-Z]) low=$(echo "$ch" | tr 'A-Z' 'a-z')
                   send_mon "sendkey shift-$low" ;;
            *)   send_mon "sendkey $ch" ;;
        esac
        sleep 0.02
    done
}

echo "[test] waiting for doublebuf..."
wait_log "[doublebuf] initialized" 10 || echo "  ! no doublebuf log"
sleep 1.5
shot "01-splash"

echo "[test] waiting for firstboot..."
wait_log "[firstboot] starting graphical wizard" 20 || echo "  ! no firstboot log"
sleep 1.5
shot "02-welcome"

send_mon "sendkey ret"; sleep 1.0
shot "03-policy"

send_mon "sendkey ret"; sleep 1.0
shot "04-create-empty"

type_str "admin"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "secret"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "secret"; sleep 0.3
shot "05-create-filled"

send_mon "sendkey ret"; sleep 1.5
shot "06-success"

echo "[test] waiting for login..."
sleep 3.0
shot "07-login"

type_str "admin"; sleep 0.3
send_mon "sendkey tab"; sleep 0.3
type_str "secret"; sleep 0.3
shot "08-login-filled"
send_mon "sendkey ret"; sleep 2.0

echo "[test] waiting for WM..."
wait_log "[wm] main loop start" 10 || echo "  ! no wm log"
sleep 2.0
shot "09-desktop"

send_mon "quit"
exec 3>&-
sleep 1
kill -9 $QEMU_PID 2>/dev/null || true

echo "[test] done. Checking serial log..."
for chk in \
    "[doublebuf] initialized" \
    "[firstboot] starting graphical wizard" \
    "[users] added 'admin'" \
    "[fat32] created /disk/USERS.TXT" \
    "[login] user=admin (authenticated)" \
    "[wm] main loop start"; do
    if grep -q -F "$chk" "$SERIAL_LOG" 2>/dev/null; then
        echo "  + $chk"
    else
        echo "  - MISSING: $chk"
    fi
done
