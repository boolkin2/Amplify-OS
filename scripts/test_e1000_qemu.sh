#!/bin/bash
# Тест e1000 драйвера в QEMU.  Должны увидеть:
#   [e1000] found device ...
#   [e1000] MAC=...
#   [e1000] link=UP
#   [e1000] init OK
#   [net] e1000 up
#   [dhcp] ... bound (получили IP через slirp DHCP сервер 10.0.2.2:67)
set -e
cd "$(dirname "$0")/.."

LOG=$(mktemp /tmp/amplify-e1000-XXXXXX.log)
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done
[ -z "$OVMF" ] && { echo "OVMF not found"; exit 1; }

echo "Running QEMU with -device e1000 (no rtl8139)..."
timeout 60 qemu-system-x86_64 \
    -bios "$OVMF" -machine pc -cpu max \
    -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
    -drive format=raw,file=build/esp.img,if=ide,index=1 \
    -m 1G -smp 2 \
    -vga none -device virtio-vga,xres=1024,yres=768 \
    -serial file:"$LOG" -display none \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 || true

echo ""
echo "[e1000 lines in serial log]"
grep -aE '^\[e1000\]|^\[net\]|^\[dhcp\]|^\[netif\]' "$LOG" | head -25 | sed 's/^/  /'
echo ""
echo "[Last 25 lines]"
tail -n 25 "$LOG" | sed 's/^/  /'
