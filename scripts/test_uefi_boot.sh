#!/bin/bash
cd "$(dirname "$0")/.."

# Find OVMF
OVMF=""
for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$p" ] && OVMF="$p" && break
done
if [ -z "$OVMF" ]; then
    echo "OVMF not found - install: sudo apt install ovmf"
    exit 1
fi
echo "OVMF: $OVMF"

timeout 25 qemu-system-x86_64 \
    -bios "$OVMF" \
    -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
    -drive format=raw,file=build/esp.img,if=ide,index=1 \
    -m 256M -vga std \
    -serial file:/tmp/aos_uefi_run.log \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
    -no-reboot -no-shutdown -display none 2>/dev/null

echo "=== Serial log (key lines) ==="
grep -a 'compositor\|AMPLIFY-OS 64-bit\|uefi\|doublebuf\|EBS\|jump' /tmp/aos_uefi_run.log
