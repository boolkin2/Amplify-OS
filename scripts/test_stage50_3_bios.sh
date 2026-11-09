#!/bin/bash
# ============================================================================
# test_stage50_3_bios.sh — подэтап 50.3: свой BIOS и загрузка неизменённого ISO.
# ----------------------------------------------------------------------------
# `phantom run <iso>`: файл ISO (TinyCore CorePure64-17.1, sha256
# 5720c593…826b10) целиком читается в память, наш BIOS находит загрузочный
# образ El Torito и запускает его в реальном режиме. Дальше — штатный
# загрузчик isolinux С САМОГО ДИСКА: он читает свой конфиг, ядро и initrd
# службой INT 13h, печатает через INT 10h, ждёт строку через INT 16h.
# Фаза 1: приглашение `boot:`; набираем метку и параметры (консоль на
#   последовательный порт, оболочка вместо init) — ядро Linux грузится через
#   BIOS и работает так же, как в 50.2: команды, время, прерывания.
# Фаза 2: просто Enter — загрузка по умолчанию, как у пользователя. Консоль
#   ядра тогда — текстовый экран (память B8000): в нём должно появиться
#   приглашение TinyCore `tc@box`.
# Нужен KVM с вложенной виртуализацией и build/guests/CorePure64-17.1.iso.
# Запуск: scripts/test_stage50_3_bios.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
ISO=build/guests/CorePure64-17.1.iso
OUT_DIR=build/screens-50_3
WORK=/tmp/amplify-stage50_3
MON_PORT=4653
ISO_ON_DISK=/disk/vault/phantom/core.iso
# Что набирается в приглашении загрузчика после метки. printk.time=1 — метки
# времени в журнале ядра гостя (в ядре TinyCore они выключены); по ним тест
# показывает, где ядро ждало. AOS_GUEST_ARGS — подставить свои параметры.
GUEST_ARGS="${AOS_GUEST_ARGS:-console=ttyS0 loglevel=7 printk.time=1 rdinit=/bin/sh}"

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
    echo "== stage 50.3 bios: 0 passed, 0 failed, 1 skipped =="
    exit 0
fi
if [ ! -f "$ISO" ]; then
    echo "  [SKIP] $ISO not found — copy the TinyCore ISO there (scp -O from the PC)"
    echo "== stage 50.3 bios: 0 passed, 0 failed, 1 skipped =="
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
        ' ') echo spc;; '/') echo slash;; '.') echo dot;; '-') echo minus;; '=') echo equal;;
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
    mcopy -D o -i "$WORK/dev.img" "$ISO" ::/vault/phantom/core.iso </dev/null
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
echo "== Phase 1: ISO -> BIOS -> isolinux -> Linux shell on the serial console =="
LOG="$WORK/serial-shell.log"
start_qemu "$LOG"
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
T0=$(date +%s.%N)
os_cmd "phantom run $ISO_ON_DISK" 1
check "BIOS found the El Torito boot image"          'wait_log "$LOG" "[phantom] bios boot:" 90'
T1=$(date +%s.%N)
grep -a '\[phantom\] bios boot\|\[phantom\] run refused' "$LOG" | tr -d '\r' | cut -c1-200 | sed 's/^/        /'
check "the disc's own boot loader runs (ISOLINUX banner)" 'wait_log "$LOG" "[guest] ISOLINUX" 20'
check "boot loader shows its prompt (boot:)"         'wait_re_n "$LOG" "^\[guest\] boot:" 1 20 >/dev/null'
grep -a '\[guest\]' "$LOG" | tr -d '\r' | head -12 | cut -c1-110 | sed 's/^/        /'
echo "        ISO read from the test's IDE disk and BIOS start: $(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }') s"

# Метка из isolinux.cfg + свои параметры: консоль на порт, оболочка вместо init.
os_cmd "phantom type corepure64 $GUEST_ARGS" 1
T2=$(date +%s.%N)
# Сначала — момент появления оболочки, и только потом проверки: проверка с
# таймаутом, поставленная перед замером, однажды прибавила к нему свои 60 с
# (2026-10-05: «загрузка за минуту», которой не было).
T3=$(wait_re_n "$LOG" "Run /bin/sh as init process" 1 120)
check "boot loader loads the kernel through INT 13h" 'grep -a -q "Loading /boot/vmlinuz64" "$LOG"'
check "kernel prints its banner"                     'grep -a -q -E "^\[guest\] (\[ *[0-9.]+\] )?Linux version" "$LOG"'
check "kernel reaches userspace: /bin/sh as init"    '[ -n "$T3" ]'
sleep 3
BOOT_S=$(awk -v a="$T2" -v b="${T3:-0}" 'BEGIN { printf "%.1f", b - a }')
echo "        from Enter at the boot prompt to the shell: $BOOT_S s"
# Обычное значение — 1.3–1.4 с (плюс секунда паузы после набора строки).
# То же по часам ХОСТА (метки ядра гостя есть не всегда): в том же журнале раз
# в секунду идёт строка [fstat] — считаем их между соседними строками гостя.
sed -n '1,/Run \/bin\/sh as init/p' "$LOG" | tr -d '\r' | awk '/\[fstat\] fps=/ { sec++; next } /\[guest\]/ { if (prev != "" && sec - psec >= 3) printf "%4d s of silence after: %s\n", sec - psec, substr(prev, 1, 100); prev = $0; psec = sec }' | sort -rn | head -4 | sed 's/^/        host-gap /'
check "kernel boots through the BIOS in under 15 s"  'awk -v s="$BOOT_S" "BEGIN { exit !(s > 0 && s < 15) }"'
# Где ядро гостя ждало дольше всего: пять самых больших разрывов между метками
# времени соседних строк его журнала (printk.time=1) — строка ПОСЛЕ разрыва.
grep -a -E '^\[guest\] \[ *[0-9]+\.[0-9]+\]' "$LOG" | tr -d '\r' | awk '{ t = $2; gsub(/[\[\]]/, "", t); if (t == "") { t = $3; gsub(/[\[\]]/, "", t) } if (NR > 1 && t - p > 0.5) printf "%8.2f s before: %s\n", t - p, substr($0, 1, 110); p = t }' | sort -rn | head -5 | sed 's/^/        gap /'
grep -a -E '\[guest\].*(Loading /boot|Linux version|Command line:|BIOS-e820|Hypervisor detected|tsc: Detected|Switched to clocksource|ttyS0 at I/O|Console: colour|Run /bin/sh)' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "kernel got the command line typed at the prompt" 'grep -a -E "\[guest\].*Command line:" "$LOG" | grep -q -F "$GUEST_ARGS"'
check "boot loader passed the initrd from the ISO"   'grep -a -E "\[guest\].*Command line:" "$LOG" | grep -q "corepure64.gz"'
# Верх обычной памяти — 256 или 512 МБ: сколько гостю дал `phantom run` (50.4).
check "kernel got the memory map from INT 15h E820"  'grep -a -q -E "\[guest\].*BIOS-e820: \[mem 0x0000000000100000-0x00000000[01]fffffff\] usable" "$LOG"'
check "kernel sees a VGA text screen (from INT 10h)" 'grep -a -q -E "\[guest\].*Console: colour VGA\+ 80x25" "$LOG"'
check "guest runs on kvm-clock"                      'grep -a -q "\[guest\].*Switched to clocksource kvm-clock" "$LOG"'

os_cmd "phantom type uname -r" 1
check "shell runs a command (uname -r)"              'wait_re_n "$LOG" "^\[guest\] [0-9]+\.[0-9]+\.[0-9]+-tinycore64" 1 15 >/dev/null'
os_cmd "phantom type echo abc123" 1
check "shell echoes text back"                       'wait_re_n "$LOG" "^\[guest\] abc123[[:space:]]*$" 1 15 >/dev/null'
os_cmd "phantom type mount -t proc proc /proc" 1.5
UPRE='^\[guest\] [0-9]+\.[0-9]+ [0-9]+\.[0-9]+'
os_cmd "phantom type cat /proc/uptime" 0.2
H1=$(wait_re_n "$LOG" "$UPRE" 1 15)
sleep 6
os_cmd "phantom type cat /proc/uptime" 0.2
H2=$(wait_re_n "$LOG" "$UPRE" 2 15)
G1=$(grep -a -E "$UPRE" "$LOG" | sed -n '1p' | awk '{print $2}')
G2=$(grep -a -E "$UPRE" "$LOG" | sed -n '2p' | awk '{print $2}')
DRIFT=$(awk -v h1="${H1:-0}" -v h2="${H2:-0}" -v g1="${G1:-0}" -v g2="${G2:-0}" 'BEGIN { dh = h2 - h1; dg = g2 - g1; if (dh <= 0) { print 999; exit } d = (dg - dh) / dh * 100; if (d < 0) d = -d; printf "%.1f", d }')
echo "        guest uptime ${G1:-?} -> ${G2:-?} s while the host clock advanced $(awk -v a="${H1:-0}" -v b="${H2:-0}" 'BEGIN { printf "%.2f", b - a }') s (difference ${DRIFT}%)"
check "guest time runs at the host's pace (within 10%)" '[ -n "$G1" ] && [ -n "$G2" ] && awk -v d="$DRIFT" "BEGIN { exit !(d < 10) }"'

os_cmd "phantom status" 1.5
sleep 4
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }" | cut -c1-330
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "idle guest leaves its core free (run < 20%)"  '[ -n "$RUN" ] && [ "$RUN" -lt 20 ]'
check "BIOS served disk reads from the ISO"          'echo "$ST" | grep -q -E "disk_reads=[1-9]"'
check "guest kernel is healthy (no oops, no panic)"  "$guest_health"
os_cmd "phantom type reboot -f" 1
check "guest reboot is caught"                       'wait_log "$LOG" "[phantom] guest ended:" 20'
grep -a '\[phantom\] guest ended' "$LOG" | tr -d '\r' | cut -c1-120 | sed 's/^/        /'
os_cmd "phantom stop" 1
check "guest removed"                                'wait_log "$LOG" "[phantom] linux stopped in" 10'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-shell.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: default boot (just Enter) — console on the text screen =="
LOG="$WORK/serial-default.log"
start_qemu "$LOG"
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "phantom run $ISO_ON_DISK" 1
check "BIOS found the El Torito boot image"          'wait_log "$LOG" "[phantom] bios boot:" 90'
check "boot loader shows its prompt (boot:)"         'wait_re_n "$LOG" "^\[guest\] boot:" 1 20 >/dev/null'
os_cmd "phantom type" 1
check "boot loader loads the kernel"                 'wait_log "$LOG" "Loading /boot/vmlinuz64" 20'
sleep 30
os_cmd "phantom screen" 2
check "text screen dumped"                           'wait_log "$LOG" "[phantom] screen:" 10'
grep -a '^\[screen\]' "$LOG" | tr -d '\r' | cut -c1-100 | sed 's/^/        /'
check "TinyCore booted to its prompt on the text screen (tc@box)" 'grep -a -q -E "^\[screen\] .*tc@box" "$LOG"'
os_cmd "phantom status" 1.5
sleep 4
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }" | cut -c1-330
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "guest is alive and idle (run < 30%)"          'echo "$ST" | grep -q -E "state=(running|sleeping)" && [ -n "$RUN" ] && [ "$RUN" -lt 30 ]'
check "guest did not end by itself"                  '! grep -a -q "\[phantom\] guest ended" "$LOG"'
os_cmd "phantom stop" 1
check "guest stopped on request"                     'wait_log "$LOG" "[phantom] linux stopped in" 10'
os_cmd "phantom selftest" 1
check "selftest still passes after BIOS guests"      'wait_log "$LOG" "[phantom] selftest: 9 passed, 0 failed" 90'
mon "screendump $PWD/$OUT_DIR/default.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-default.log"
stop_qemu

echo "== stage 50.3 bios: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
