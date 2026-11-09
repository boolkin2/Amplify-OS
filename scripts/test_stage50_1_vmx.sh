#!/bin/bash
# ============================================================================
# test_stage50_1_vmx.sh — подэтап 50.1: VMXON, VMCS, EPT, первый гость.
# ----------------------------------------------------------------------------
# Гость — встроенный проверочный (phantom_guest.asm): стартует в реальном
# режиме, сам переходит в защищённый и длинный и докладывает буквами RPLIFV.
# Фаза 1 (4 ядра, нужен KVM с вложенной виртуализацией):
#   * smoke 50.1 при загрузке: VMX на всех ядрах и обратно, гость на AP;
#   * `phantom selftest` — девять сценариев: ядра, режимы (EPT 2 МБ и 4 КБ),
#     изоляция памяти (запись за край RAM гостя = выход EPT, страница хоста
#     цела), тройной сбой, вытеснение зациклившегося гостя, цена выхода,
#     100 циклов «создать-запустить-снять» без утечки, делёж ядра с пулом;
#   * фоновый гость (`phantom spin`): счётчик гостя растёт, кадры WM идут,
#     `phantom stop` снимает его.
# Фаза 2 (2 ядра): единственный AP — он же дирижёр пула; сценарии те же.
# Фаза 3 (1 ядро): phantom честно отказывает, загрузка не страдает.
# Фаза 4 (нет VMX: `-cpu host,-vmx`, под TCG — `-cpu max`): то же.
# Без KVM фазы 1–3 — SKIP (TCG не эмулирует VMX).
# Запуск: scripts/test_stage50_1_vmx.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-50_1
WORK=/tmp/amplify-stage50_1
MON_PORT=4651

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

KVM=0; SLOW=${AOS_SLOW:-6}
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then KVM=1; SLOW=1; fi
NESTED=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null)
case "$NESTED" in Y|1) NESTED=1;; *) NESTED=0;; esac

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
keyname() { case "$1" in ' ') echo spc;; *) echo "$1";; esac; }
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.1; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

# start_qemu <log> <ускоритель и процессор> <число ядер>
start_qemu() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    # fstat=1: раз в секунду строка `[fstat] fps=…` — по ней видно, что кадры
    # идут, пока гость считает.
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
    printf 'fstat=1\n' > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    qemu-system-x86_64 -bios "$OVMF" $2 -machine pc -m 1G \
        -smp "cpus=$3,cores=$3,threads=1" \
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
    wait_log "$1" "[smoke 37.6c-1] RTC" $((120*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}
# Значение `counter=N` из последней строки `[phantom] spin …`.
spin_counter() { grep -a '\[phantom\] spin counter=' "$1" | tail -1 | sed -n 's/.*counter=\([0-9]*\).*/\1/p'; }

# Сценарии `phantom selftest`: общая часть фаз 1 и 2. $1 — лог.
selftest_checks() {
    local log="$1" t
    key meta_l-spc 2
    type_text "phantom selftest"; key ret 1
    check "phantom selftest finished"                'wait_log "$log" "[phantom] selftest:" $((90*SLOW))'
    grep -a '\[phantom\] test' "$log" | tr -d '\r' | sed 's/^/        /'
    for t in cores modes modes-4k-ept isolation triple-fault preempt exit-cost cycles pool-share; do
        check "scenario $t"                          'grep -a -q "\[phantom\] test $t: PASS" "$log"'
    done
    check "selftest: 9 passed, 0 failed"             'grep -a -q "\[phantom\] selftest: 9 passed, 0 failed" "$log"'
}

# ===========================================================================
echo "== Phase 1: 4 cores, VT-x (nested KVM) =="
if [ $KVM = 1 ] && [ $NESTED = 1 ]; then
    LOG="$WORK/serial-4.log"
    start_qemu "$LOG" "-enable-kvm -cpu host" 4
    check "smoke 50.1 block printed"                 'wait_log "$LOG" "[smoke 50.1] done in" $((90*SLOW))'
    grep -a '\[smoke 50.1\]' "$LOG" | tr -d '\r' | sed 's/^/        /'
    check "VMX switched on on all 4 cores and off again" 'grep -a "\[smoke 50.1\] cores: PASS" "$LOG" | grep -q "VMX on 4/4 cores, mask after the last VM=0x0"'
    check "guest went real -> protected -> long mode" 'grep -a "\[smoke 50.1\] guest real->protected->long: PASS" "$LOG" | grep -q "RPLIFV"'
    check "guest saw the bytes the host wrote"       'grep -a "\[smoke 50.1\] guest" "$LOG" | grep -q "memory sum matches"'
    check "smoke 50.1 PASS"                          'grep -a "\[smoke 50.1\] done in" "$LOG" | grep -q "PASS"'
    MS=$(grep -a '\[smoke 50.1\] done in' "$LOG" | sed -n 's/.*done in \([0-9]*\) ms.*/\1/p')
    echo "        smoke 50.1 took ${MS:-?} ms of boot time"
    check "smoke 50.1 costs under 100 ms of boot"    '[ -n "$MS" ] && [ "$MS" -lt 100 ]'
    check "pool self-test still passes (smoke 49)"   'wait_log "$LOG" "[smoke 49] done in" 5 && grep -a "\[smoke 49\] done in" "$LOG" | grep -q "PASS"'
    login "$LOG"
    check "boot goes on to the desktop"              'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
    selftest_checks "$LOG"

    # Фоновый гость: система живёт, пока он считает.
    type_text "phantom spin"; key ret 1
    check "background guest started"                 'wait_log "$LOG" "[phantom] spin started on core" $((10*SLOW))'
    sleep 3
    type_text "phantom status"; key ret 1.5
    C1=$(spin_counter "$LOG")
    F0=$(grep -a -c '\[fstat\] fps=' "$LOG")
    sleep 4
    type_text "phantom status"; key ret 1.5
    C2=$(spin_counter "$LOG")
    echo "        guest counter: ${C1:-?} -> ${C2:-?}"
    check "guest keeps counting in the background"   '[ -n "$C1" ] && [ -n "$C2" ] && [ "$C2" -gt "$C1" ]'
    # Кадры за то время, пока гость работал: строки [fstat] после F0.
    FPS=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+1)) | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p' | sort -n | head -1)
    BUSY=$(grep -a '\[fstat\] fps=' "$LOG" | tail -n +$((F0+1)) | sed -n 's/.*busy=\([0-9]*\)\..*/\1/p' | sort -n | tail -1)
    echo "        while the guest runs: min fps=${FPS:-?}, max BSP busy=${BUSY:-?}%"
    check "WM frames keep coming while the guest runs" '[ -n "$FPS" ] && [ "$FPS" -ge 5 ]'
    check "BSP is not pinned by the guest (busy < 60%)" '[ -n "$BUSY" ] && [ "$BUSY" -lt 60 ]'
    type_text "phantom stop"; key ret 1
    check "background guest stopped"                 'wait_log "$LOG" "[phantom] spin stopped in" $((10*SLOW))'
    US=$(grep -a '\[phantom\] spin stopped in' "$LOG" | sed -n 's/.*stopped in \([0-9]*\) us.*/\1/p')
    echo "        stop took ${US:-?} us"
    check "stop takes under 50 ms"                   '[ -n "$US" ] && [ "$US" -lt 50000 ]'
    type_text "phantom"; key ret 1
    check "system is fine after the guests (VT-x still ready)" 'wait_log "$LOG" "[phantom] status vmx=1 ready=1 cpus=4/4" $((10*SLOW))'
    mon "screendump $PWD/$OUT_DIR/selftest.ppm"; sleep 0.5
    check "no kernel panic"                          '! grep -a -q -E "PANIC|EXCEPTION|Triple" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-4.log"
    stop_qemu

    # =======================================================================
    echo "== Phase 2: 2 cores (the only AP is also the pool conductor) =="
    LOG="$WORK/serial-2.log"
    start_qemu "$LOG" "-enable-kvm -cpu host" 2
    check "smoke 50.1 PASS on 2 cores"               'wait_log "$LOG" "[smoke 50.1] done in" $((90*SLOW)) && grep -a "\[smoke 50.1\] done in" "$LOG" | grep -q "PASS"'
    check "VMX on 2/2 cores"                         'grep -a "\[smoke 50.1\] cores: PASS" "$LOG" | grep -q "VMX on 2/2 cores"'
    login "$LOG"
    check "boot goes on to the desktop"              'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
    selftest_checks "$LOG"
    check "no kernel panic"                          '! grep -a -q -E "PANIC|EXCEPTION|Triple" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-2.log"
    stop_qemu

    # =======================================================================
    echo "== Phase 3: 1 core (no AP for the guest) =="
    LOG="$WORK/serial-1.log"
    start_qemu "$LOG" "-enable-kvm -cpu host" 1
    check "smoke 50.1 says why it is skipped"        'wait_log "$LOG" "[smoke 50.1] skipped: phantom needs a second CPU core" $((90*SLOW))'
    check "smoke 50.1 PASS (a refusal is not an error)" 'grep -a "\[smoke 50.1\] done in" "$LOG" | grep -q "PASS"'
    login "$LOG"
    check "boot goes on to the desktop"              'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
    key meta_l-spc 2
    type_text "phantom selftest"; key ret 1
    check "phantom selftest: skipped"                'wait_log "$LOG" "[phantom] selftest: skipped" $((10*SLOW))'
    check "no kernel panic"                          '! grep -a -q -E "PANIC|EXCEPTION|Triple" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-1.log"
    stop_qemu
else
    skip "phases 1-3: need KVM with nested virtualization (kvm=$KVM nested=$NESTED); TCG has no VMX"
fi

# ===========================================================================
echo "== Phase 4: no VT-x =="
LOG="$WORK/serial-novmx.log"
if [ $KVM = 1 ]; then start_qemu "$LOG" "-enable-kvm -cpu host,-vmx" 4
else                  start_qemu "$LOG" "-accel tcg,thread=multi -cpu max" 4; fi
check "smoke 50.1 skipped: no VT-x"                  'wait_log "$LOG" "[smoke 50.1] skipped: no usable VT-x" $((90*SLOW))'
check "smoke 50.1 PASS"                              'grep -a "\[smoke 50.1\] done in" "$LOG" | grep -q "PASS"'
check "pool self-test passes (smoke 49)"             'grep -a "\[smoke 49\] done in" "$LOG" | grep -q "PASS"'
login "$LOG"
check "boot goes on to the desktop"                  'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
key meta_l-spc 2
type_text "phantom selftest"; key ret 1
check "phantom selftest: skipped"                    'wait_log "$LOG" "[phantom] selftest: skipped" $((10*SLOW))'
type_text "phantom spin"; key ret 1
check "phantom spin: refused"                        'wait_log "$LOG" "[phantom] spin: not started" $((10*SLOW))'
check "no kernel panic"                              '! grep -a -q -E "PANIC|EXCEPTION|Triple" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-novmx.log"
stop_qemu

echo "== stage 50.1 vmx: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
