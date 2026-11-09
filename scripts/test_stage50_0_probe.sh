#!/bin/bash
# ============================================================================
# test_stage50_0_probe.sh — подэтап 50.0: разведка VT-x (VMX), без VMXON.
# ----------------------------------------------------------------------------
# Фаза 1 (нужен KVM с вложенной виртуализацией): QEMU `-cpu host`. Ядро должно
#   увидеть VMX на всех ядрах, прочитать MSR IA32_VMX_* и вынести вердикт
#   READY. Биты возможностей сверяются со списком, который тот же QEMU отдаёт
#   по QMP (query-cpu-model-expansion): это независимая проверка, что мы
#   читаем нужные MSR и правильно понимаем номера битов. Команда `phantom`
#   (и алиас `vm`) в терминале показывает то же.
# Фаза 2: та же машина без VMX (`-cpu host,-vmx`; под TCG — `-cpu max`): ядро
#   не трогает ни одного MSR VMX, пишет вердикт ABSENT и спокойно доходит до
#   рабочего стола — так оно грузится на ПК разработчика и на любой машине
#   без VT-x.
# TCG не эмулирует VMX: без KVM фаза 1 — SKIP (не PASS).
# Запуск: scripts/test_stage50_0_probe.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-50_0
WORK=/tmp/amplify-stage50_0
MON_PORT=4650

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
rm -f "$WORK"/*.log "$WORK"/*.img "$WORK"/*.txt

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
# Сколько раз строка встретилась в логе (для команд, набранных дважды).
count_log() { grep -a -c -F "$2" "$1" 2>/dev/null; }

# start_qemu <log> <опции ускорителя и процессора>
start_qemu() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    qemu-system-x86_64 -bios "$OVMF" $2 -machine pc -m 1G \
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
    wait_log "$1" "[smoke 37.6c-1] RTC" $((120*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}

# Что QEMU обещает гостю с `-cpu host`: строки вида `vmx-ept:true`.
# Отдельный запуск с остановленным процессором (-S) — один вопрос и выход.
qemu_vmx_props() {
    ( printf '{"execute":"qmp_capabilities"}\n{"execute":"query-cpu-model-expansion","arguments":{"type":"full","model":{"name":"host"}}}\n'
      sleep 2; printf '{"execute":"quit"}\n' ) \
      | timeout 20 qemu-system-x86_64 -enable-kvm -cpu host -machine pc -nodefaults -display none -S -qmp stdio 2>/dev/null \
      | tr ',{' '\n\n' | grep '"vmx' | tr -d ' "\r' > "$WORK/qemu-vmx.txt"
}

# Имя бита в ядре : свойство QEMU. host64 и eptwb у QEMU свойств не имеют.
MAP="extint:vmx-intr-exit nmiexit:vmx-nmi-exit vnmi:vmx-vnmi ptimer:vmx-preemption-timer
pi:vmx-posted-intr intwin:vmx-vintr-pending tscoff:vmx-tsc-offset hltexit:vmx-hlt-exit
ioexit:vmx-io-exit iobmp:vmx-io-bitmap mtf:vmx-mtf msrbmp:vmx-msr-bitmap sec:vmx-secondary-ctls
ept:vmx-ept desc:vmx-desc-exit rdtscp:vmx-rdtscp-exit vpid:vmx-vpid ug:vmx-unrestricted-guest
apicv:vmx-apicv-vid ple:vmx-ple invpcid:vmx-invpcid-exit vmfunc:vmx-vmfunc shadow:vmx-shadow-vmcs
pml:vmx-pml xsaves:vmx-xsaves tscscale:vmx-tsc-scaling ackintr:vmx-exit-ack-intr
xsvefer:vmx-exit-save-efer xldefer:vmx-exit-load-efer xsvptimer:vmx-exit-save-preemption-timer
ia32e:vmx-entry-ia32e-mode eldefer:vmx-entry-load-efer eptx:vmx-ept-execonly walk4:vmx-page-walk-4
ept2m:vmx-ept-2mb ept1g:vmx-ept-1gb invept:vmx-invept eptad:vmx-eptad invvpid:vmx-invvpid
acthlt:vmx-activity-hlt insouts:vmx-ins-outs truectl:vmx-true-ctls"

# ===========================================================================
echo "== Phase 1: VT-x visible (-cpu host under nested KVM) =="
if [ $KVM = 1 ] && [ $NESTED = 1 ]; then
    LOG="$WORK/serial-vmx.log"
    start_qemu "$LOG" "-enable-kvm -cpu host"
    check "smoke 50 block printed"                   'wait_log "$LOG" "[smoke 50] done in" $((90*SLOW))'
    grep -a '\[smoke 50\]' "$LOG" | tr -d '\r' | sed 's/^/        /'
    check "kernel sees VMX (CPUID.1:ECX[5])"         'grep -a -q "CPUID.1:ECX.VMX=1" "$LOG"'
    check "IA32_FEATURE_CONTROL lets us do VMXON"    'grep -a "\[smoke 50\] feature_control=" "$LOG" | grep -q -E "locked, VMXON allowed|unlocked"'
    check "VMCS revision and region size read"       'grep -a -q -E "\[smoke 50\] vmcs revision=0x[0-9a-f]{8} region=[1-9][0-9]* " "$LOG"'
    check "all five control MSRs are non-zero"       'grep -a "\[smoke 50\] msr" "$LOG" | tr " " "\n" | grep -E "^(pin|proc|proc2|exit|entry)=" | grep -v -c "=0x0000000000000000" | grep -q "^5$"'
    CAPS=$(grep -a '\[smoke 50\] caps' "$LOG" | tr -d '\r')
    check "EPT can be enabled"                       'echo "$CAPS" | grep -q " ept=1"'
    check "unrestricted guest (real mode for BIOS)"  'echo "$CAPS" | grep -q " ug=1"'
    check "VT-x on every online core (4 of 4)"       'grep -a -q "\[smoke 50\] cpus=4/4 " "$LOG"'
    check "verdict READY"                            'grep -a -q "\[smoke 50\] verdict: READY" "$LOG"'
    check "probe self-check PASS"                    'grep -a "\[smoke 50\] done in" "$LOG" | grep -q "PASS"'

    # Сверка с QEMU: каждое распознанное нами «можно поставить 1» должно
    # совпасть с тем, что QEMU сам объявляет для этой модели процессора.
    qemu_vmx_props
    MISM=""; NCMP=0
    for p in $MAP; do
        k=${p%%:*}; q=${p#*:}
        kv=$(echo "$CAPS" | grep -o " $k=[01]" | head -1 | sed 's/.*=//')
        qv=$(grep -m1 "^$q:" "$WORK/qemu-vmx.txt" | sed 's/.*://')
        [ -z "$qv" ] && continue          # эта версия QEMU такого свойства не знает
        NCMP=$((NCMP+1))
        case "$kv:$qv" in 1:true|0:false) ;; *) MISM="$MISM $k=$kv($q=$qv)";; esac
    done
    echo "        compared $NCMP bits with QEMU; mismatches:${MISM:- none}"
    check "capability bits equal QEMU's own list ($NCMP compared)" '[ $NCMP -ge 30 ] && [ -z "$MISM" ]'

    login "$LOG"
    check "boot goes on to the desktop"              'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
    key meta_l-spc 2
    type_text "phantom"; key ret 1
    check "phantom: status in the terminal"          'wait_log "$LOG" "[phantom] status vmx=1 ready=1 cpus=4/4" $((10*SLOW))'
    type_text "vm caps"; key ret 1
    sleep 1
    check "vm is an alias of phantom"                '[ "$(count_log "$LOG" "[phantom] status vmx=1 ready=1")" = 2 ]'
    type_text "phantom run a"; key ret 1
    check "phantom run: honest refusal for now"      'wait_log "$LOG" "[phantom] run refused" $((10*SLOW))'
    mon "screendump $PWD/$OUT_DIR/phantom-caps.ppm"; sleep 0.5
    check "no kernel panic"                          '! grep -a -q "PANIC" "$LOG"'
    cp "$LOG" "$OUT_DIR/serial-vmx.log"; cp "$WORK/qemu-vmx.txt" "$OUT_DIR/" 2>/dev/null
    stop_qemu
else
    skip "phase 1: needs KVM with nested virtualization (kvm=$KVM nested=$NESTED); TCG has no VMX"
fi

# ===========================================================================
echo "== Phase 2: no VT-x (the kernel must not touch VMX MSRs) =="
LOG2="$WORK/serial-novmx.log"
if [ $KVM = 1 ]; then start_qemu "$LOG2" "-enable-kvm -cpu host,-vmx"
else                  start_qemu "$LOG2" "-accel tcg,thread=multi -cpu max"; fi
check "smoke 50 block printed"                       'wait_log "$LOG2" "[smoke 50] done in" $((90*SLOW))'
grep -a '\[smoke 50\]' "$LOG2" | tr -d '\r' | sed 's/^/        /'
check "kernel sees no VMX"                           'grep -a -q "CPUID.1:ECX.VMX=0" "$LOG2"'
check "verdict ABSENT"                               'grep -a -q "\[smoke 50\] verdict: ABSENT" "$LOG2"'
check "no VMX MSR was read"                          '! grep -a -q "\[smoke 50\] msr" "$LOG2"'
check "probe self-check PASS (absence is not an error)" 'grep -a "\[smoke 50\] done in" "$LOG2" | grep -q "PASS"'
login "$LOG2"
check "boot goes on to the desktop"                  'wait_log "$LOG2" "[wm] main loop start" $((60*SLOW))'
key meta_l-spc 2
type_text "phantom"; key ret 1
check "phantom: reports no VT-x"                     'wait_log "$LOG2" "[phantom] status vmx=0 ready=0" $((10*SLOW))'
type_text "phantom run a"; key ret 1
check "phantom run: refused"                         'wait_log "$LOG2" "[phantom] run refused" $((10*SLOW))'
check "no kernel panic"                              '! grep -a -q "PANIC" "$LOG2"'
cp "$LOG2" "$OUT_DIR/serial-novmx.log"
stop_qemu

echo "== stage 50.0 probe: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]
