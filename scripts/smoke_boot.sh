#!/bin/bash
# ============================================================================
# smoke_boot.sh — короткая проверка «ядро грузится» (ПК, TCG, 1–2 минуты).
# ----------------------------------------------------------------------------
# Грузит dev-систему через наш BOOTX64.EFI на КОПИИ диска (QEMU гасится
# kill -9, а это портит FAT рабочего образа — CLAUDE.md §5.33) и печатает
# строки serial, по которым видно, как прошла ранняя загрузка: память, ядра,
# smoke-блоки, паники. Это не тест «прошёл/нет» — настоящие тесты на узле.
#
#   bash scripts/smoke_boot.sh [секунд, по умолчанию 90] [RAM, 1G] [ядер, 4]
# ============================================================================
cd "$(dirname "$0")/.." || exit 1
SECS=${1:-90}; MEM=${2:-1G}; CPUS=${3:-4}
OVMF=""
for f in /usr/share/pve-edk2-firmware/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd; do
    [ -f "$f" ] && OVMF=$f && break
done
W=/tmp/amplify-smoke
rm -rf $W; mkdir -p $W
cp build/disk-x86_64.img $W/disk.img
cp build/esp.img $W/esp.img
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACC="-enable-kvm -cpu host"; else ACC="-accel tcg,thread=multi -cpu max"; fi
QEMU=$(command -v qemu-system-x86_64 || echo /usr/bin/qemu-system-x86_64)
$QEMU -bios "$OVMF" $ACC -machine pc -m $MEM -smp $CPUS \
    -drive format=raw,file=$W/disk.img,if=ide,index=0,media=disk \
    -drive format=raw,file=$W/esp.img,if=ide,index=1 \
    -vga virtio -display none -serial file:$W/serial.log \
    -d cpu_reset -D $W/reset.log -no-reboot &
QPID=$!
for i in $(seq 1 $SECS); do
    grep -aqE 'smoke 37.6c-1\] RTC|PANIC' $W/serial.log 2>/dev/null && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
sleep 3
kill -9 $QPID 2>/dev/null
mkdir -p build
cp $W/serial.log build/smoke-serial.log
echo "lines: $(wc -l < $W/serial.log)  cpu resets: $(grep -c 'CPU Reset' $W/reset.log 2>/dev/null)"
grep -aE '^\[pmm\]|\[heap\] (init|OOM)|\[paging\]|\[smp\].*(online|started|cores|APs)|PANIC|Page Fault|EXCEPTION|FAIL|\[smoke (47\.4|48|49|51\.5)\]|smoke 37.6c-1\] RTC' $W/serial.log | cut -c1-140 | head -${SMOKE_LINES:-40}
