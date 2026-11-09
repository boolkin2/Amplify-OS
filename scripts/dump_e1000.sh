#!/bin/bash
# Запустить QEMU c e1000 и сделать pcap-дамп всего трафика.
set -e
cd "$(dirname "$0")/.."
DUMP=/tmp/e1000.pcap
SRL=/tmp/e1000-srl.log
rm -f "$DUMP" "$SRL"

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

timeout 30 qemu-system-x86_64 \
    -bios "$OVMF" -machine pc -cpu max \
    -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
    -drive format=raw,file=build/esp.img,if=ide,index=1 \
    -m 1G -smp 2 \
    -vga none -device virtio-vga,xres=1024,yres=768 \
    -serial file:"$SRL" -display none \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -object filter-dump,id=f0,netdev=net0,file="$DUMP" \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown >/dev/null 2>&1 || true

echo "=== pcap stats ==="
ls -la "$DUMP" 2>&1
echo ""
echo "=== tcpdump ==="
tcpdump -r "$DUMP" -n 2>&1 | head -20
echo ""
echo "=== serial e1000 lines ==="
grep -aE '\[e1000|\[dhcp|\[arp|\[net' "$SRL" | head -15
