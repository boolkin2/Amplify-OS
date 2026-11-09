#!/bin/bash
# Полный e2e тест: e1000 → DHCP → DNS → TCP → TLS → HTTPS GET к реальному сайту.
set -e
cd "$(dirname "$0")/.."

LOG=$(mktemp /tmp/amplify-e1000-https-XXXXXX.log)
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

echo "Running QEMU with e1000 + AMPLIFY_TLS_INTERNET..."
timeout 90 qemu-system-x86_64 \
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
echo "[Net events]"
grep -aE '\[e1000\]|\[net\]|\[dhcp\]|\[dns\]|\[tcp\]|\[tls\]|\[smoke 47\.7\]' "$LOG" | head -40 | sed 's/^/  /'
