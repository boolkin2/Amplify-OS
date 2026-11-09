#!/bin/bash
# ============================================================================
# test_stage50_2_linux.sh — подэтап 50.2: устройства PC и настоящее ядро Linux.
# ----------------------------------------------------------------------------
# Гость — ядро и initrd TinyCore (CorePure64-17.1.iso, sha256 5720c593…826b10),
# загруженные НАПРЯМУЮ (стенд `phantom linux`; штатный путь через BIOS — 50.3).
# Консоль гостя — последовательный порт: вывод виден в serial ядра строками
# `[guest] …`, ввод подаётся командой `phantom type …`.
# Фаза 1 (4 ядра): ядро Linux доходит до пользовательской оболочки; у него
#   есть часы (kvmclock), таймер (IRQ0 через 8259) и порт (IRQ4); команды в
#   оболочке исполняются; время гостя идёт со скоростью времени хоста; в
#   простое гость спит и ядро свободно; кадры WM идут; перезагрузка гостя
#   и остановка работают; после гостя `phantom selftest` всё ещё проходит.
# Фаза 2: тот же гость со штатным init TinyCore.
# Нужен KVM с вложенной виртуализацией; без него — SKIP.
# Запуск: scripts/test_stage50_2_linux.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
ISO=build/guests/CorePure64-17.1.iso
GUEST=build/guests/tc
OUT_DIR=build/screens-50_2
WORK=/tmp/amplify-stage50_2
MON_PORT=4652

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
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
    echo "== stage 50.2 linux: 0 passed, 0 failed, 1 skipped =="
    exit 0
fi

# Ядро и initrd гостя — из ISO. Сам ISO в дереве не лежит (26 МБ, чужой): его
# кладут в build/guests вручную; рабочий каталог узла — tmpfs, после
# перезагрузки узла образ надо скопировать заново. Нет образа — SKIP, не PASS.
if [ ! -f "$GUEST/vmlinuz64" ] || [ ! -f "$GUEST/corepure64.gz" ]; then
    if [ ! -f "$ISO" ]; then
        echo "  [SKIP] $ISO not found — copy the TinyCore ISO there (scp -O from the PC)"
        echo "== stage 50.2 linux: 0 passed, 0 failed, 1 skipped =="
        exit 0
    fi
    rm -rf "$GUEST"; mkdir -p "$GUEST"
    xorriso -osirrox on -indev "$ISO" -extract /boot "$GUEST" >/dev/null 2>&1
    chmod -R u+rw "$GUEST"
fi
for f in "$GUEST/vmlinuz64" "$GUEST/corepure64.gz"; do [ -f "$f" ] || { echo "$f missing after extraction"; exit 1; }; done

# Два экземпляра теста делят рабочий каталог и порт монитора: второй стёр бы
# файлы первого и подключился бы к ЕГО QEMU (так пропал запуск 2026-10-05,
# начатый, пока предыдущий ещё ждал своих таймаутов).
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
keyname() { case "$1" in ' ') echo spc;; '/') echo slash;; '.') echo dot;; '-') echo minus;; *) echo "$1";; esac; }
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.1; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# Ждать n-ю строку, подходящую под регулярное выражение; печатает время хоста
# (секунды с дробью), когда она появилась.
wait_re_n() {
    local end=$(( $(date +%s) + $4 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if [ "$(grep -a -c -E "$2" "$1" 2>/dev/null)" -ge "$3" ]; then date +%s.%N; return 0; fi
        sleep 0.1
    done
    return 1
}
# Набрать команду в терминале ОС (не в госте).
os_cmd() { type_text "$1"; key ret "${2:-1}"; }

start_qemu() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum ::/vault ::/vault/phantom </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    mcopy -D o -i "$WORK/dev.img" "$GUEST/vmlinuz64" "$GUEST/corepure64.gz" ::/vault/phantom/ </dev/null
    qemu-system-x86_64 -bios "$OVMF" -enable-kvm -cpu host -machine pc -m 2G \
        -smp "cpus=$2,cores=$2,threads=1" \
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

# ===========================================================================
echo "== Phase 1: Linux kernel to a userspace shell (4 cores) =="
LOG="$WORK/serial-shell.log"
start_qemu "$LOG" 4
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
T0=$(date +%s.%N)
os_cmd "phantom linux" 1
check "guest started"                                'wait_log "$LOG" "[phantom] linux started" 60'
T1=$(date +%s.%N)
grep -a '\[phantom\] linux' "$LOG" | tr -d '\r' | sed 's/^/        /'
check "kernel prints its banner on the serial port"  'wait_log "$LOG" "[guest] Linux version" 60'
T2=$(wait_re_n "$LOG" "Run /bin/sh as init process" 1 90)
check "kernel reaches userspace: /bin/sh as init"    '[ -n "$T2" ]'
echo "        files read and guest created in $(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }') s (includes typing the command); kernel start to shell: $(awk -v a="$T1" -v b="${T2:-0}" 'BEGIN { printf "%.1f", b - a }') s"
sleep 3
grep -a -E '\[guest\].*(Linux version|Hypervisor detected|kvm-clock: Using|tsc: Detected|Switched to clocksource|ttyS0 at I/O|NULL legacy PIC|No local APIC|Memory:|Run /bin/sh)' "$LOG" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
check "guest takes its clock from us (kvm-clock)"    'grep -a -q "\[guest\].*kvm-clock: Using msrs" "$LOG"'
check "guest knows the TSC frequency"                'grep -a -q -E "\[guest\].*tsc: Detected [0-9]+\.[0-9]+ MHz" "$LOG"'
check "guest runs on kvm-clock, not on jiffies"      'grep -a -q "\[guest\].*Switched to clocksource kvm-clock" "$LOG"'
check "guest found the serial port (16550A, IRQ 4)"  'grep -a -q -E "\[guest\].*ttyS0 at I/O 0x3f8 \(irq = 4.*16550A" "$LOG"'

# Команды в оболочке гостя.
os_cmd "phantom type uname -r" 1
check "shell runs a command (uname -r)"              'wait_re_n "$LOG" "^\[guest\] [0-9]+\.[0-9]+\.[0-9]+-tinycore64" 1 15 >/dev/null'
os_cmd "phantom type echo abc123" 1
check "shell echoes text back"                       'wait_re_n "$LOG" "^\[guest\] abc123[[:space:]]*$" 1 15 >/dev/null'

# В голой оболочке initramfs /proc не смонтирован — монтируем сами.
os_cmd "phantom type mount -t proc proc /proc" 1.5
# Время гостя против времени хоста: два чтения /proc/uptime.
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

os_cmd "phantom type cat /proc/interrupts" 2
grep -a -E '^\[guest\] +[0-9]+: ' "$LOG" | tr -d '\r' | cut -c1-110 | sed 's/^/        /'
check "timer interrupts arrive through the 8259 (IRQ 0)" 'grep -a -E "^\[guest\] +0: +[1-9][0-9]* .*timer" "$LOG" | grep -q PIC'
check "serial interrupts arrive (IRQ 4)"             'grep -a -q -E "^\[guest\] +4: +[1-9][0-9]* .*ttyS0" "$LOG"'

# Простой: гость спит по HLT, ядро свободно.
os_cmd "phantom status" 1.5
F0=$(grep -a -c '\[fstat\] fps=' "$LOG")
sleep 4
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }"
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "idle guest leaves its core free (run < 20%)"  '[ -n "$RUN" ] && [ "$RUN" -lt 20 ]'
check "idle guest sleeps on HLT"                     'echo "$ST" | grep -q -E "sleeps=[1-9]"'
FPS=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+1)) | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p' | sort -n | head -1)
BUSY=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+1)) | sed -n 's/.*busy=\([0-9]*\)\..*/\1/p' | sort -n | tail -1)
echo "        with the guest alive: min fps=${FPS:-?}, max BSP busy=${BUSY:-?}%"
check "WM frames keep coming"                        '[ -n "$FPS" ] && [ "$FPS" -ge 5 ]'
check "no unknown-MSR storm"                         '! echo "$ST" | grep -q -E "unknown_msr=[0-9]{4,}"'
check "guest kernel is healthy (no oops, no panic)"  '! grep -a -q -E "\[guest\].*(Oops|Kernel panic|general protection|BUG:)" "$LOG"'

# Перезагрузка гостя — это его конец, а не конец хоста.
os_cmd "phantom type reboot -f" 1
check "guest reboot is caught as a reset request"    'wait_log "$LOG" "[phantom] guest ended: guest asked for a reset" 20'
grep -a '\[phantom\] guest ended' "$LOG" | tr -d '\r' | cut -c1-260 | sed 's/^/        /'
os_cmd "phantom stop" 1
check "guest removed"                                'wait_log "$LOG" "[phantom] linux stopped in" 10'
os_cmd "phantom selftest" 1
check "selftest still passes after a Linux guest"    'wait_log "$LOG" "[phantom] selftest: 9 passed, 0 failed" 90'
mon "screendump $PWD/$OUT_DIR/shell.ppm"; sleep 0.5
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-shell.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: the same guest with TinyCore's own init =="
LOG="$WORK/serial-init.log"
start_qemu "$LOG" 4
login "$LOG"
check "desktop reached"                              'wait_log "$LOG" "[wm] main loop start" 60'
key meta_l-spc 2
os_cmd "phantom linux init" 1
check "guest started"                                'wait_log "$LOG" "[phantom] linux started" 60'
check "kernel runs /init"                            'wait_log "$LOG" "Run /init as init process" 90'
sleep 25
grep -a '\[guest\]' "$LOG" | tr -d '\r' | tail -14 | cut -c1-120 | sed 's/^/        /'
os_cmd "phantom status" 1.5
ST=$(grep -a '\[phantom\] linux state=' "$LOG" | tail -1 | tr -d '\r')
echo "        ${ST#\[phantom\] }"
check "guest is still alive after init ran"          'echo "$ST" | grep -q -E "state=(running|sleeping)"'
check "guest did not end by itself"                  '! grep -a -q "\[phantom\] guest ended" "$LOG"'
check "guest kernel is healthy (no oops, no panic)"  '! grep -a -q -E "\[guest\].*(Oops|Kernel panic|general protection|BUG:)" "$LOG"'
RUN=$(echo "$ST" | sed -n 's/.* run=\([0-9]*\)%.*/\1/p')
check "after init the guest is idle (run < 30%)"     '[ -n "$RUN" ] && [ "$RUN" -lt 30 ]'
os_cmd "phantom stop" 1
check "guest stopped on request"                     'wait_log "$LOG" "[phantom] linux stopped in" 10'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-init.log"
stop_qemu

echo "== stage 50.2 linux: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
