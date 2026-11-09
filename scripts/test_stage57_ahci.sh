#!/usr/bin/env bash
# ============================================================================
# test_stage57_ahci.sh — установка на SATA AHCI (q35) + модель MIND с носителя.
# ----------------------------------------------------------------------------
# Воспроизводит конфигурацию Proxmox-ВМ на чипсете q35: диск и CD висят на
# AHCI-контроллере ICH9, legacy IDE-портов нет. Раньше установщик видел там
# «0 дисков», а модель SmolLM2 на установленную систему попасть не могла.
#
# Фаза 1: live-ISO (UEFI) на q35 — AHCI видит диск и ATAPI-CD, ISO9660
#         находит носитель, установка копирует /mind/*.gguf -> /vault/mind
#         и сверяет побайтово.
# Фаза 2: хост — GPT, файл модели на системном разделе совпадает по sha256.
# Фаза 3: загрузка установленного диска на q35 (без CD), вход, Ctrl+Space —
#         MIND сам находит SmolLM2 в /disk/vault/mind и отвечает.
#
# Запуск из WSL:  bash scripts/test_stage57_ahci.sh
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
MODEL=build/mind/SmolLM2-135M-Instruct-Q8_0.gguf
MODEL_NAME=SmolLM2-135M-Instruct-Q8_0.gguf
SHOT_DIR=build/screens-57-ahci
WORK=/tmp/amplify-ahci
TARGET="$WORK/target.img"
LOG1="$WORK/serial-install.log"
LOG2="$WORK/serial-installed.log"
MON_PORT=4581
USER_NAME=mark
USER_PASS=test1234

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
[ -f "$ISO" ] || { echo "$ISO not found — run: make ARCH=x86_64"; exit 1; }
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then CPU="-enable-kvm -cpu host"; SLOW=1
else CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-5}; echo "  (no KVM access — TCG, timeouts x$SLOW)"; fi

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$TARGET" "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.png

QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img "$WORK"/*.part; true; }
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
mon() { echo "$1" >&3; sleep 0.05; while read -t 0.1 -r -u 3 _; do :; done; }
key() { mon "sendkey $1"; sleep "${2:-0.25}"; }
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '?') echo shift-slash;; '.') echo dot;;
        ',') echo comma;; '-') echo minus;; [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.12; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
shot() {
    local p="$PWD/$SHOT_DIR/$1.ppm"
    mon "screendump $p"; sleep 1
    [ -f "$p" ] && echo "  [shot] $SHOT_DIR/$1.ppm"
}
# q35: встроенный ICH9 AHCI — шины ide.0..ide.5 это его порты.
start_q35() {
    local log="$1"; shift
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine q35 -m 2G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null
}

HAVE_MODEL=0
xorriso -indev "$ISO" -find /mind -name "$MODEL_NAME" 2>/dev/null | grep -q "$MODEL_NAME" && HAVE_MODEL=1

# ===========================================================================
echo "== Phase 1: install on q35 (AHCI disk + AHCI CD, UEFI) =="
check "ISO carries /mind/$MODEL_NAME"        '[ $HAVE_MODEL = 1 ]'
truncate -s 4G "$TARGET"
start_q35 "$LOG1" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    -device ide-hd,drive=target,bus=ide.0,bootindex=1 \
    -drive file="$ISO",format=raw,if=none,id=live,media=cdrom,readonly=on \
    -device ide-cd,drive=live,bus=ide.2,bootindex=0

check "installer UI started"                 'wait_log "$LOG1" "[installer] starting" $((90*SLOW))'
check "AHCI controller found"                'grep -q "\[ahci\] controller" "$LOG1"'
check "AHCI port: hard disk 4096 MB"         'grep -q "\[ahci\] port 0: QEMU HARDDISK - 4096 MB" "$LOG1"'
check "AHCI port: ATAPI CD"                  'grep -q "\[ahci\] port 2: .*(ATAPI)" "$LOG1"'
check "legacy IDE absent, no false disks"    '! grep -q "\[ata\] IDE" "$LOG1"'
check "one install target detected"          'grep -q "\[install\] targets found: 1" "$LOG1"'
grep -E "\[ahci\]|\[install\] targets" "$LOG1" | sed 's/^/        /'
sleep 2; shot 01-welcome

key ret 0.6
key spc 0.4
key ret 0.6;                shot 02-destination
key ret 0.6
key ret 0.6                                      # пустые поля -> подсветка (как в тесте 57)
key tab;  type_text "$USER_NAME"
key tab;  type_text "$USER_PASS"
key tab;  type_text "$USER_PASS"
shot 03-options
key ret 0.6
key ret 0.3
sleep 2; shot 04-installing

check "ISO9660: installation medium mounted" 'wait_log "$LOG1" "[iso] AMPLIFY-OS medium on SATA port 2" $((120*SLOW))'
check "MIND model copied and verified"       'wait_log "$LOG1" "[install] mind model copied: /vault/mind/$MODEL_NAME 144811360 bytes, verified" $((240*SLOW))'
sleep 1; shot 05-model-step
check "installation completed"               'wait_log "$LOG1" "[install] completed successfully" $((120*SLOW))'
grep -q "\[install\] ERROR" "$LOG1" && grep "\[install\] ERROR" "$LOG1" | sed 's/^/        /'
sleep 2; shot 06-complete
stop_qemu
cp -f "$LOG1" "$SHOT_DIR/" 2>/dev/null

# ===========================================================================
echo "== Phase 2: the model on the system partition (host) =="
SFDISK=$(command -v sfdisk || echo /usr/sbin/sfdisk)
PT=$("$SFDISK" -d "$TARGET" 2>/dev/null)
check "GPT written"                          'echo "$PT" | grep -q "label: gpt"'
SYS_START=$(echo "$PT" | grep -m1 "EBD0A0A2" | sed -E 's/.*start= *([0-9]+).*/\1/')
if [ -n "$SYS_START" ]; then
    SYS="$TARGET@@$((SYS_START*512))"
    export MTOOLS_SKIP_CHECK=1
    check "SYS: /vault/mind lists the model"  'mdir -i "$SYS" ::/vault/mind 2>/dev/null | grep -q "SmolLM2"'
    mcopy -o -i "$SYS" "::/vault/mind/$MODEL_NAME" "$WORK/model.copy" 2>/dev/null
    check "SYS: model sha256 equals the source" '[ "$(sha256sum < "$WORK/model.copy" | cut -d" " -f1)" = "$(sha256sum < "$MODEL" | cut -d" " -f1)" ]'
    rm -f "$WORK/model.copy"
fi

# ===========================================================================
echo "== Phase 3: boot the installed disk on q35 and ask MIND =="
start_q35 "$LOG2" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    -device ide-hd,drive=target,bus=ide.0,bootindex=0
check "installed system mounted over AHCI"   'wait_log "$LOG2" "[fat32] mounted" $((90*SLOW))'
check "[smoke 49] PASS on q35"               'wait_log "$LOG2" "[smoke 49] AMPLIFY MIND self-test PASS" $((60*SLOW))'
wait_log "$LOG2" "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
sleep 2
key ret 0.4; type_text "$USER_PASS"; key ret 1
check "login"                                'wait_log "$LOG2" "[identity] unlocked user=$USER_NAME" 30'
wait_log "$LOG2" "[wm] main loop start" 30 || true
sleep 3
key ctrl-spc 0.6
check "MIND finds the model in /disk/vault/mind" 'wait_log "$LOG2" "[mind] load /disk/vault/mind/$MODEL_NAME" 15'
check "SmolLM2 loaded on the installed system"   'wait_log "$LOG2" "[mind] loaded SmolLM2-135M-Instruct-Q8_0: layers=30" $((60*SLOW))'
type_text "What is the capital of France?"; key ret 0.4
check "MIND answered"                        'wait_log "$LOG2" "[mind] reply done" $((60*SLOW))'
grep -E "\[mind\] (loaded|reply:)" "$LOG2" | sed 's/^/        /'
sleep 1; shot 07-installed-mind
stop_qemu
cp -f "$LOG2" "$SHOT_DIR/" 2>/dev/null

echo
echo "== stage 57 AHCI + MIND model: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
