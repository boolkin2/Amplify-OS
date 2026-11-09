#!/bin/bash
# ============================================================================
# bench_stage41_2_disk.sh — скорость чтения диска: IDE (PIO) / AHCI / virtio-blk.
# ----------------------------------------------------------------------------
# Один и тот же dev-диск (1 ГБ) подключается к QEMU тремя способами; ядро при
# старте WM читает с него до 256 МиБ кусками по 1 МиБ (perf.conf: diskbench=1)
# и печатает `[diskbench] <шина>: sequential read N MiB in M ms = X MiB/s`.
# Запуск: scripts/bench_stage41_2_disk.sh   (узел Proxmox; под KVM цифры
# настоящие, под TCG — только проверка, что замер живой)
# ============================================================================
set -u
cd "$(dirname "$0")/.."
ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
WORK=/tmp/amplify-bench41_2-disk
MON_PORT=4589

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then CPU="-enable-kvm -cpu host"; SLOW=1; ACCEL=KVM
else CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}; ACCEL=TCG; fi
echo "== disk bench 41.2 ($ACCEL) =="

mkdir -p "$WORK" build/bench41_2
QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; true; }
trap cleanup EXIT
mon_connect() {
    for _ in $(seq 1 100); do
        if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
            sleep 0.3; read -t 0.3 -r -u 3 _ || true; read -t 0.3 -r -u 3 _ || true; return 0
        fi
        sleep 0.1
    done
    return 1
}
mon() { echo "$1" >&3; sleep 0.05; while read -t 0.02 -r -u 3 _; do :; done; }
key() { mon "sendkey $1"; sleep "${2:-0.25}"; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do grep -a -q -F "$2" "$1" 2>/dev/null && return 0; sleep 0.5; done
    return 1
}

# one <имя> <машина> <аргументы устройства системного диска...>
one() {
    local name="$1" machine="$2"; shift 2
    local log="$WORK/serial-$name.log"
    rm -f "$log"
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"; cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
    printf 'diskbench=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine "$machine" -m 1G -smp 4 \
        -vga none -device virtio-vga,xres=1280,yres=720 -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -drive file="$WORK/dev.img",format=raw,if=none,id=sys,cache=none \
        -drive file="$WORK/esp.img",format=raw,if=none,id=esp \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "  $name: monitor port never opened"; return; }
    wait_log "$log" "[smoke 37.6c-1] RTC" $((120*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
    wait_log "$log" "[diskbench] " $((120*SLOW)) || echo "  $name: no [diskbench] line"
    grep -a "\[diskbench\]" "$log" | tr -d '\r' | sed "s/^/  $name: /"
    cp "$log" "build/bench41_2/disk-$name-$ACCEL.log"
    mon "quit"; sleep 1; kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; QEMU_PID=""; exec 3>&- 2>/dev/null
}

# IDE (PIO): i440fx, оба диска на legacy IDE.
one ide  pc  -device ide-hd,drive=sys,bus=ide.0,unit=0 -device ide-hd,drive=esp,bus=ide.0,unit=1,bootindex=0
# AHCI: q35, системный диск на SATA-порту.
one ahci q35 -device ide-hd,drive=sys,bus=ide.0        -device ide-hd,drive=esp,bus=ide.1,bootindex=0
# virtio-blk: оба диска на virtio (системный — первым: ядро монтирует первый
# найденный том FAT по порядку слотов, а IDE-слоты идут раньше virtio).
one virtio pc -device virtio-blk-pci,drive=sys         -device virtio-blk-pci,drive=esp,bootindex=0
echo "== done =="
