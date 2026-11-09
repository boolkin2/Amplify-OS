#!/bin/bash
# ============================================================================
# bench_stage41_2_frames.sh — замер кадров/с и времени фаз кадра (подэтап 41.2).
# ----------------------------------------------------------------------------
# Грузит dev-диск с /Sanctum/perf.conf (fstat=1): ядро раз в секунду пишет в
# serial строку `[fstat] fps=... scene=avg/max ...` (см. framestat.h). Скрипт
# снимает три режима и печатает строки каждого:
#   idle    — рабочий стол без ввода;
#   mouse   — непрерывное движение мыши (QEMU monitor mouse_move);
#   window  — то же при открытом окне терминала (Super+Space).
#
# Запуск:  scripts/bench_stage41_2_frames.sh [секунд на режим=6] [cpus=4] [WxH=1920x1080]
# Под KVM (узел Proxmox) цифры настоящие; под TCG (ПК) — только проверка, что
# счётчики живые.
# ============================================================================
set -u
cd "$(dirname "$0")/.."

SECS="${1:-6}"; CPUS="${2:-4}"; RES="${3:-1920x1080}"
XRES="${RES%x*}"; YRES="${RES#*x}"
ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
WORK=/tmp/amplify-bench41_2
LOG="$WORK/serial.log"
MON_PORT=4582

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd \
         /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1; ACCEL=KVM
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}; ACCEL=TCG
fi
echo "== bench 41.2: $RES, $CPUS cpus, $ACCEL, ${SECS}s per mode =="

mkdir -p "$WORK"; rm -f "$WORK"/*.log "$WORK"/*.img
QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; true; }
trap cleanup EXIT

mon_connect() {
    for _ in $(seq 1 100); do
        if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
            sleep 0.3; read -t 0.3 -r -u 3 _ || true; read -t 0.3 -r -u 3 _ || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}
drain() { while read -t 0.02 -r -u 3 _; do :; done; }
mon()   { echo "$1" >&3; sleep 0.05; drain; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
wait_log() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -q -F "$1" "$LOG" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# Непрерывное движение мыши ~100 событий/с в течение $1 секунд: зигзаг, чтобы
# курсор не упёрся в край. Эхо монитора вычитываем пачками — иначе сокет
# заполнится и QEMU притормозит монитор.
wiggle() {
    local end=$(( $(date +%s) + $1 )) i=0 dx=9 dy=4
    while [ "$(date +%s)" -lt "$end" ]; do
        echo "mouse_move $dx $dy" >&3
        i=$((i+1))
        if [ $((i % 40)) = 0 ]; then dx=$((-dx)); dy=$((-dy)); drain; fi
        sleep 0.01
    done
    drain
}
# Строки [fstat], появившиеся после отметки $1 (число строк лога до режима).
mode_report() {
    echo "-- $2 --"
    tail -n +"$(( $1 + 1 ))" "$LOG" | grep -a -F "[fstat] fps" | tail -n +2 | tr -d '\r'
}
mark() { wc -l < "$LOG"; }

# dev-диск + perf.conf. mtools: -D s / -D o и </dev/null (CLAUDE.md §5.13).
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
printf 'fstat=1\n' > "$WORK/perf.conf"
mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null

qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
    -smp cpus=$CPUS,cores=$CPUS,threads=1 \
    -vga none -device virtio-vga,xres=$XRES,yres=$YRES \
    -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }

wait_log "[smoke 37.6c-1] RTC" $((60*SLOW)) || { echo "FAIL: boot did not reach login"; exit 1; }
sleep 2
key ret 0.4
for c in a d m i n; do key $c 0.12; done
key ret 1
wait_log "[wm] main loop start" $((60*SLOW)) || { echo "FAIL: desktop not reached"; exit 1; }
wait_log "[fstat] serial report enabled" 10   || { echo "FAIL: perf.conf not picked up"; exit 1; }
grep -a -E "\[virtio_gpu\] display|\[drm\] active|fallback" "$LOG" | tr -d '\r' | head -3
sleep 3

M=$(mark); sleep "$SECS";  mode_report "$M" "idle"
M=$(mark); wiggle "$SECS"; mode_report "$M" "mouse"
key meta_l-spc 2
M=$(mark); wiggle "$SECS"; mode_report "$M" "window+mouse"

mkdir -p build/bench41_2
cp "$LOG" "build/bench41_2/serial-$ACCEL-$RES.log"
mon "quit"; sleep 1
echo "== done; log: build/bench41_2/serial-$ACCEL-$RES.log =="
