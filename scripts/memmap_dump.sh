#!/bin/bash
# ============================================================================
# memmap_dump.sh — карта памяти прошивки UEFI глазами нашего загрузчика (38.1).
# ----------------------------------------------------------------------------
# Кладёт на копию ESP файл \memmap.flag: BOOTX64.EFI, увидев его, печатает
# карту памяти ДО своих выделений (строки «MMAP тип начало-конец»).
# Нужна, чтобы решать, куда класть ядро: до 38.1 о занятом у OVMF адресе
# 0x800000 мы знали только по отказу AllocatePages.
#
#   bash scripts/memmap_dump.sh [RAM, по умолчанию 1G] [машина: pc|q35] [OVMF.fd]
# Итог — build/memmap-<машина>-<RAM>.txt и он же в stdout.
# ============================================================================
cd "$(dirname "$0")/.." || exit 1
MEM=${1:-1G}
MACH=${2:-pc}
OVMF=${3:-}
if [ -z "$OVMF" ]; then
    for f in /usr/share/pve-edk2-firmware/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd; do
        [ -f "$f" ] && OVMF=$f && break
    done
fi
[ -f "$OVMF" ] || { echo "no OVMF"; exit 1; }
[ -f build/esp.img ] || { echo "no build/esp.img (make esp)"; exit 1; }

W=/tmp/amplify-memmap
rm -rf $W; mkdir -p $W
cp build/esp.img $W/esp.img
echo 1 > $W/memmap.flag
mcopy -D o -i $W/esp.img $W/memmap.flag ::/memmap.flag </dev/null || exit 1

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACC="-enable-kvm -cpu host"; else ACC="-cpu max"; fi
QEMU=$(command -v qemu-system-x86_64 || echo /usr/bin/qemu-system-x86_64)
$QEMU -bios "$OVMF" $ACC -machine $MACH -m $MEM -smp 2 \
    -drive format=raw,file=$W/esp.img,if=ide,index=0 \
    -display none -serial file:$W/serial.log -no-reboot &
QPID=$!
# Карта печатается в первые секунды работы загрузчика; ждём её конца, но не
# дольше минуты (под TCG прошивка сама стартует секунд десять).
for i in $(seq 1 60); do
    grep -aq 'MMAP end' $W/serial.log 2>/dev/null && break
    sleep 1
done
sleep 2
kill -9 $QPID 2>/dev/null

OUT=build/memmap-$MACH-$MEM.txt
{
    echo "# OVMF: $OVMF  machine=$MACH  RAM=$MEM  accel=${ACC%% *}"
    grep -a 'MMAP' $W/serial.log | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
    echo "# loader:"
    grep -aE 'ELF|entry|top  |loaded @|FATAL' $W/serial.log | tr -d '\r' | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | head -8
} > $OUT
cat $OUT
