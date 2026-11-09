#!/bin/bash
# ============================================================================
# test_stage50_4_alpine.sh — подэтап 50.4: диск для гостя, Alpine до login.
# ----------------------------------------------------------------------------
# Гость — неизменённый alpine-virt-3.24.2-x86_64.iso (sha256 3ab42476…e7130b).
# Наш BIOS запускает его загрузчик (syslinux 6.04), а после загрузки ядро
# гостя находит ТОТ ЖЕ образ как диск virtio-blk на шине PCI: монтирует его,
# берёт с него модули и пакеты и доходит до приглашения login.
# Фаза 1: в `phantom run` передаётся строка для загрузчика (у него на ввод
#   одна секунда): консоль на последовательный порт. Проверяется: диск найден
#   ядром, носитель смонтирован, login на порту, вход root, версия Alpine,
#   прерывания диска, выключение гостя.
# Фаза 2: без строки — загрузка по умолчанию; login появляется на текстовом
#   экране гостя.
# Нужен KVM с вложенной виртуализацией и build/guests/alpine-virt-3.24.2-x86_64.iso.
# Запуск: scripts/test_stage50_4_alpine.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
ISO=build/guests/alpine-virt-3.24.2-x86_64.iso
OUT_DIR=build/screens-50_4
WORK=/tmp/amplify-stage50_4
MON_PORT=4654
ISO_ON_DISK=/disk/vault/phantom/alpine.iso
# Метка из syslinux.cfg + свои параметры. loglevel=7 перекрывает `quiet` из
# конфига: видно, как ядро находит диск.
BOOT_LINE="virt console=ttyS0,115200 loglevel=7"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

KVM=0
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then KVM=1; fi
NESTED=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null)
case "$NESTED" in Y|1) NESTED=1;; *) NESTED=0;; esac
if [ $KVM != 1 ] || [ $NESTED != 1 ]; then
    echo "  [SKIP] needs KVM with nested virtualization (kvm=$KVM nested=$NESTED); TCG has no VMX"
    echo "== stage 50.4 alpine: 0 passed, 0 failed, 1 skipped =="
    exit 0
fi
if [ ! -f "$ISO" ]; then
    echo "  [SKIP] $ISO not found — copy the Alpine ISO there (scp -O from the PC)"
    echo "== stage 50.4 alpine: 0 passed, 0 failed, 1 skipped =="
    exit 0
fi

# Один экземпляр: общий рабочий каталог и порт монитора (CLAUDE.md §5.36).
if pgrep -f "[q]emu-system.*$WORK/" >/dev/null 2>&1; then
    echo "another run of this test is still alive (QEMU with $WORK) — wait for it or kill it"
    exit 1
fi
mkdir -p "$WORK" "$OUT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; true
}
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
mon()   { echo "$1" >&3; sleep 0.05; while read -t 0.02 -r -u 3 _; do :; done; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '.') echo dot;; '-') echo minus;; '=') echo equal;; ',') echo comma;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.1; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
wait_re_n() {
    local end=$(( $(date +%s) + $4 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if [ "$(grep -a -c -E "$2" "$1" 2>/dev/null)" -ge "$3" ]; then date +%s.%N; return 0; fi
        sleep 0.1
    done
    return 1
}
os_cmd() { type_text "$1"; key ret "${2:-1}"; }

start_qemu() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum ::/vault ::/vault/phantom </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    # Образ кладётся КАК ЕСТЬ, ни один байт в нём не меняется.
    mcopy -D o -i "$WORK/dev.img" "$ISO" ::/vault/phantom/alpine.iso </dev/null
    qemu-system-x86_64 -bios "$OVMF" -enable-kvm -cpu host -machine pc -m 2G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1280,yres=720 \
        -display none -serial "file:$1" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
        -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null
}
login() {
    wait_log "$1" "[smoke 37.6c-1] RTC" 120 || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}
guest_health='! grep -a -q -E "\[guest\].*(Oops|Kernel panic|general protection|BUG:)" "$LOG"'

# ===========================================================================
echo "== Phase 1: Alpine from the ISO: boot loader -> kernel -> disk -> login on serial =="
LOG="$WORK/serial-login.log"
start_qemu "$LOG"
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
T0=$(date +%s.%N)
os_cmd "phantom run $ISO_ON_DISK $BOOT_LINE" 1
check "BIOS found the El Torito boot image"          'wait_log "$LOG" "[phantom] bios boot:" 150'
T1=$(date +%s.%N)
grep -a '\[phantom\] bios boot\|\[phantom\] run refused' "$LOG" | tr -d '\r' | cut -c1-200 | sed 's/^/        /'
echo "        ISO (66 MB) read from the test's IDE disk and BIOS start: $(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }') s"
# Момент приглашения login — до проверок (CLAUDE.md §5.37).
T2=$(wait_re_n "$LOG" "^\[guest\] .*login:" 1 240)
echo "        from BIOS start to the login prompt: $(awk -v a="$T1" -v b="${T2:-0}" 'BEGIN { printf "%.1f", b - a }') s"
grep -a -E '\[guest\]' "$LOG" | tr -d '\r' | head -6 | cut -c1-120 | sed 's/^/        /'
grep -a -E '\[guest\].*(Linux version|Command line:|virtio_blk|virtio-pci|vda|ISO 9660|isofs|squashfs|PCI: Using|pci 0000:00:01|Welcome to Alpine|login:)' "$LOG" | tr -d '\r' | cut -c1-150 | head -24 | sed 's/^/        /'
check "the disc's own boot loader runs (ISOLINUX 6)" 'grep -a -q -E "\[guest\].*ISOLINUX 6" "$LOG"'
check "boot loader took the pre-typed boot line"     'grep -a -E "\[guest\].*Command line:" "$LOG" | grep -q -F "console=ttyS0,115200 loglevel=7"'
check "kernel prints its banner"                     'grep -a -q -E "\[guest\].*Linux version 6\.18" "$LOG"'
check "kernel found the PCI bus"                     'grep -a -q -E "\[guest\].*pci 0000:00:01\.0: \[1af4:1001\]" "$LOG"'
check "virtio-pci takes the device (legacy transport)" 'grep -a -q -E "\[guest\].*virtio-pci 0000:00:01\.0: virtio_pci: leaving for legacy driver" "$LOG"'
check "Alpine reached the login prompt on the serial console" '[ -n "$T2" ]'
check "guest kernel is healthy (no oops, no panic)"  "$guest_health"

os_cmd "phantom type root" 3
check "root logged in (shell prompt)"                'wait_re_n "$LOG" "^\[guest\] .*localhost:~#" 1 20 >/dev/null'
os_cmd "phantom type cat /etc/alpine-release" 2
check "it is Alpine 3.24.2 (cat /etc/alpine-release)" 'wait_re_n "$LOG" "^\[guest\] 3\.24\.2[[:space:]]*$" 1 15 >/dev/null'
os_cmd "phantom type uname -r" 2
check "kernel from the ISO (uname -r)"               'wait_re_n "$LOG" "^\[guest\] 6\.18\.52-0-virt" 1 15 >/dev/null'
# Журнал ядра к этому моменту на консоль не идёт (init Alpine его глушит),
# поэтому диск проверяем напрямую: размер в секторах и признак «только чтение».
os_cmd "phantom type cat /sys/block/vda/size /sys/block/vda/ro" 2
check "guest sees the disk: 135168 sectors (the whole ISO), read-only" 'wait_re_n "$LOG" "^\[guest\] 135168[[:space:]]*$" 1 15 >/dev/null && grep -a -q -E "^\[guest\] 1[[:space:]]*$" "$LOG"'
os_cmd "phantom type mount" 3
grep -a -E '^\[guest\] .*(/dev/vda|/dev/loop|modloop)' "$LOG" | tr -d '\r' | cut -c1-120 | head -4 | sed 's/^/        /'
check "boot media is the virtio disk, mounted as ISO 9660" 'grep -a -q -E "^\[guest\] /dev/vda.* on /media/vda type iso9660" "$LOG"'
os_cmd "phantom type cat /proc/interrupts" 3
grep -a -E '^\[guest\] +[0-9]+: ' "$LOG" | tr -d '\r' | cut -c1-110 | sed 's/^/        /'
check "disk interrupts arrive through the 8259 (virtio)" 'grep -a -q -E "^\[guest\] +[0-9]+: +[1-9][0-9]* .*virtio" "$LOG"'

os_cmd "phantom status" 1.5
sleep 4
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }" | cut -c1-360
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "idle guest leaves its core free (run < 20%)"  '[ -n "$RUN" ] && [ "$RUN" -lt 20 ]'
check "disk served requests without errors"          'echo "$ST" | grep -q -E "disk_reqs=[1-9]" && ! echo "$ST" | grep -q "disk_errors="'
os_cmd "phantom type poweroff" 1
check "guest powers off by itself"                   'wait_log "$LOG" "[phantom] guest ended:" 60'
grep -a '\[phantom\] guest ended' "$LOG" | tr -d '\r' | cut -c1-120 | sed 's/^/        /'
os_cmd "phantom stop" 1
check "guest removed"                                'wait_log "$LOG" "[phantom] linux stopped in" 10'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-login.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: default boot (nothing typed) — login on the text screen =="
LOG="$WORK/serial-default.log"
start_qemu "$LOG"
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "phantom run $ISO_ON_DISK" 1
check "BIOS found the El Torito boot image"          'wait_log "$LOG" "[phantom] bios boot:" 150'
sleep 60
os_cmd "phantom screen" 2
check "text screen dumped"                           'wait_log "$LOG" "[phantom] screen:" 10'
grep -a '^\[screen\]' "$LOG" | tr -d '\r' | cut -c1-100 | tail -8 | sed 's/^/        /'
check "Alpine booted to login on the text screen"    'grep -a -q -E "^\[screen\] .*login:" "$LOG"'
os_cmd "phantom status" 1.5
sleep 4
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }" | cut -c1-360
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "guest is alive and idle (run < 30%)"          'echo "$ST" | grep -q -E "state=(running|sleeping)" && [ -n "$RUN" ] && [ "$RUN" -lt 30 ]'
check "guest did not end by itself"                  '! grep -a -q "\[phantom\] guest ended" "$LOG"'
os_cmd "phantom stop" 1
check "guest stopped on request"                     'wait_log "$LOG" "[phantom] linux stopped in" 10'
os_cmd "phantom selftest" 1
check "selftest still passes after Alpine"           'wait_log "$LOG" "[phantom] selftest: 9 passed, 0 failed" 90'
mon "screendump $PWD/$OUT_DIR/default.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-default.log"
stop_qemu

echo "== stage 50.4 alpine: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
