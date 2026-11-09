#!/usr/bin/env bash
# ============================================================================
# test_stage48_fde.sh — шифрование системного раздела (этап 48), end-to-end.
# ----------------------------------------------------------------------------
# Фаза 1: dev-загрузка (esp.img + dev-диск) — KAT'ы крипто (PBKDF2, XTS) и
#         [smoke 48]: LUKS1 + blkcrypt на RAM-диске.
# Фаза 2: установка с ISO на пустой диск с включённым «Encrypt the drive».
# Фаза 3: проверка диска на хосте:
#           * тип раздела «Linux LUKS» в GPT, заголовок LUKS1;
#           * в сыром разделе нет открытого текста (маркер, system.conf);
#           * tools/luks_interop — НАСТОЯЩАЯ libcryptsetup открывает том
#             паролем, OpenSSL расшифровывает payload в FAT32 BPB;
#             неверный пароль — отказ.
# Фаза 4: загрузка установленного диска: экран разблокировки, неверный
#         пароль отвергнут, верный — /disk смонтирован через blkcrypt,
#         вход без второго ввода пароля (единый вход), тёмная тема.
#
# Запуск из WSL:  bash scripts/test_stage48_fde.sh
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-48
WORK=/tmp/amplify-stage48
TARGET="$WORK/target.img"
LOG0="$WORK/serial-dev.log"
LOG1="$WORK/serial-install.log"
LOG2="$WORK/serial-unlock.log"
MON_PORT=4578
USER_NAME=mark
USER_PASS=test1234

# [41.2] AOS_DISK_BUS=virtio — целевой диск установки на virtio-blk вместо IDE:
# тот же сценарий (установка, проверка образа на хосте, загрузка) проверяет
# драйвер virtio_blk.c и загрузку OVMF с такого диска.
if [ "${AOS_DISK_BUS:-ide}" = virtio ]; then
    TGT_DEV="-device virtio-blk-pci,drive=target"
    echo "  (target disk bus: virtio-blk)"
else
    TGT_DEV="-device ide-hd,drive=target,bus=ide.0,unit=0"
fi

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found (sudo apt install ovmf)"; exit 1; }
for f in "$ISO" "$ESP" "$DEVDISK"; do
    [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }
done

# [41.2] KVM — по умолчанию только для диска на virtio-blk. Диск на IDE (PIO)
# под KVM пишет ~0.2 МиБ/с (QEMU делает отдельную асинхронную операцию и
# прерывание на КАЖДЫЙ сектор, ядро между ними крутит опрос порта) —
# установка 145 МБ растягивается на четверть часа; под TCG тот же сценарий
# занимает минуты. AOS_IDE_KVM=1 — всё-таки гонять IDE под KVM.
if [ -r /dev/kvm ] && [ -w /dev/kvm ] && { [ "${AOS_DISK_BUS:-ide}" = virtio ] || [ -n "${AOS_IDE_KVM:-}" ]; }; then
    CPU="-enable-kvm -cpu host"
else
    CPU="-cpu max"
fi

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$TARGET" "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.png

QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; rm -rf "$WORK"/*.img "$WORK"/*.part; true; }
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
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "${s:i:1}" 0.12; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.3
    done
    return 1
}
shot() {
    local p="$PWD/$SHOT_DIR/$1.ppm"
    mon "screendump $p"
    sleep 1
    [ -f "$p" ] && echo "  [shot] $SHOT_DIR/$1.ppm"
}
start_qemu() {
    local log="$1"; shift
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
        -smp cpus=2,cores=2,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -netdev user,id=net0 -device rtl8139,netdev=net0 \
        -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null
}

# ===========================================================================
echo "== Phase 1: crypto KATs + [smoke 48] on the development boot =="
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
start_qemu "$LOG0" \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1
check "crypto KATs incl. PBKDF2 + AES-XTS"   'wait_log "$LOG0" "[crypto] self-test: PASS" 90'
check "[smoke 48] FDE self-test PASS"        'wait_log "$LOG0" "[smoke 48] self-test PASS" 90'
check "dev disk reported as not encrypted"   'grep -q "system volume: not encrypted" "$LOG0"'
check "no FAIL lines in [smoke 48]"          '! grep -q "\[smoke 48\].*FAIL" "$LOG0"'
grep "\[smoke 48\]" "$LOG0" | sed 's/^/        /'
stop_qemu
rm -f "$WORK/dev.img" "$WORK/esp.img"

# ===========================================================================
echo "== Phase 2: install from ISO with 'Encrypt the drive' enabled =="
truncate -s 4G "$TARGET"
start_qemu "$LOG1" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    $TGT_DEV,bootindex=1 \
    -drive file="$ISO",format=raw,if=none,id=live,media=cdrom,readonly=on \
    -device ide-cd,drive=live,bus=ide.1,unit=0,bootindex=0
check "installer UI started"                 'wait_log "$LOG1" "[installer] starting" 120'
sleep 2
key ret 0.6                                      # WELCOME
key spc 0.3; key ret 0.6                         # LICENSE
key ret 0.6                                      # DESTINATION
key tab;  type_text "$USER_NAME"
key tab;  type_text "$USER_PASS"
key tab;  type_text "$USER_PASS"
key tab;  key tab; key spc                       # VAULT → DARK (вкл.)
key tab;  key spc 0.4                            # ENCRYPT (вкл.)
shot 01-options-encrypt
key ret 0.6; shot 02-confirm-encrypt
key ret 0.3
sleep 2; shot 03-installing
# [41.2] 1200 с: IDE PIO под KVM медленнее, чем под TCG (см. test_stage57_installer.sh).
check "installation completed"               'wait_log "$LOG1" "[install] completed successfully" 1200'
# [41.2] Номер устройства зависит от шины (IDE — 0, virtio-blk — 4); отличаем
# установку от самотеста на RAM-диске (dev 16 @ lba 0) по ненулевому LBA.
check "LUKS1 header created"                 'grep -qE "\[luks\] formatted dev [0-7] @ lba [1-9]" "$LOG1"'
check "system written through blkcrypt"      'grep -q "\[blkcrypt\] mapped install-system" "$LOG1"'
check "verify re-opened the slot by password" 'grep -q "\[luks\] key slot 0 unlocked" "$LOG1"'
grep -q "\[install\] ERROR" "$LOG1" && grep "\[install\] ERROR" "$LOG1"
sleep 2; shot 04-complete
stop_qemu

# ===========================================================================
echo "== Phase 3: inspect the encrypted disk from the host =="
SFDISK=$(command -v sfdisk || echo /usr/sbin/sfdisk)
PT=$("$SFDISK" -d "$TARGET" 2>/dev/null)
SYS_LINE=$(echo "$PT" | grep 'name="AMPLIFY-SYSTEM"')
SYS_START=$(echo "$SYS_LINE" | sed -E 's/.*start= *([0-9]+),.*/\1/')
SYS_SIZE=$(echo "$SYS_LINE" | sed -E 's/.*size= *([0-9]+),.*/\1/')
check "system partition type is Linux LUKS"  'echo "$SYS_LINE" | grep -qi "CA7D7CCB-63ED-4C53-861C-1742536059CC"'
if [ -n "$SYS_START" ] && [ -n "$SYS_SIZE" ]; then
    HEAD="$WORK/luks-head.part"
    dd if="$TARGET" of="$HEAD" bs=512 skip="$SYS_START" count=$((4096 + 64)) conv=sparse status=none
    MAGIC=$(od -An -tx1 -N6 "$HEAD" | tr -d ' \n')
    check "LUKS1 magic at the partition start"   '[ "$MAGIC" = "4c554b53babe" ]'
    check "cipher aes / xts-plain64 / sha256" \
          '[ "$(dd if=$HEAD bs=1 skip=8 count=3 status=none)" = "aes" ] &&
           [ "$(dd if=$HEAD bs=1 skip=40 count=11 status=none)" = "xts-plain64" ] &&
           [ "$(dd if=$HEAD bs=1 skip=72 count=6 status=none)" = "sha256" ]'
    # tr '\0' '\n': незаписанная часть разреженного раздела — гигабайты нулей
    # без перевода строки; grep копил их в одну «строку» и ловил OOM в WSL
    # (этап 49). Маркеры NUL не содержат — счёт совпадений не меняется.
    LEAK=$(dd if="$TARGET" bs=1M iflag=skip_bytes,count_bytes skip=$((SYS_START*512)) \
              count=$((SYS_SIZE*512)) status=none | tr '\000' '\n' | \
              grep -a -c -E "AMPLIFY-OS installed|hostname=amplify|$USER_NAME:" )
    check "no plaintext on the raw system partition" '[ "${LEAK:-1}" = "0" ]'

    if [ -x build/luks_interop ] || gcc -O2 -o build/luks_interop tools/luks_interop.c \
            -l:libcryptsetup.so.12 -lcrypto 2>/dev/null; then
        OUT=$(build/luks_interop "$HEAD" "$USER_PASS" 2>&1); RC=$?
        echo "$OUT" | sed 's/^/        /'
        check "libcryptsetup opens the volume with the passphrase" '[ $RC -eq 0 ]'
        build/luks_interop "$HEAD" "wrong-passphrase" >/dev/null 2>&1; RC2=$?
        check "libcryptsetup rejects a wrong passphrase"          '[ $RC2 -ne 0 ]'
    else
        echo "  [SKIP] libcryptsetup/libssl-dev not available — interop check skipped"
    fi
fi

# ===========================================================================
echo "== Phase 4: boot the encrypted installation =="
start_qemu "$LOG2" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    $TGT_DEV,bootindex=0
check "locked system volume detected"        'wait_log "$LOG2" "system volume is LOCKED" 90'
check "/disk NOT mounted before unlock"      '! grep -q "\[fat32\] mounted" "$LOG2"'
check "unlock screen shown"                  'wait_log "$LOG2" "[fde] unlock screen" 60'
sleep 1.5; shot 05-unlock-screen
type_text "wrongpass1"; key ret 0.3
check "wrong passphrase rejected"            'wait_log "$LOG2" "[fde] wrong passphrase" 30'
sleep 1; shot 06-unlock-wrong
type_text "$USER_PASS"; key ret 0.3
check "volume unlocked with the passphrase"  'wait_log "$LOG2" "[fde] system volume unlocked" 30'
check "/disk mounted from the crypt vdev"    'grep -q "\[fat32\] mounted AMPLIFY-OS.*dev=8 " "$LOG2"'
check "user database read from encrypted /disk" 'wait_log "$LOG2" "[users] loaded 1 record" 20'
check "dark theme applied"                   'wait_log "$LOG2" "[sysconf] theme=dark" 60'
check "[smoke 48] sees the unlocked volume"  'wait_log "$LOG2" "system volume: unlocked" 60'
check "single sign-on: no second password"   'wait_log "$LOG2" "[identity] unlocked user=$USER_NAME (via disk unlock)" 60'
sleep 4; shot 07-desktop
stop_qemu

for l in "$LOG0" "$LOG1" "$LOG2"; do cp -f "$l" "$SHOT_DIR/" 2>/dev/null; done
if python3 -c "import PIL" 2>/dev/null; then
    for f in "$SHOT_DIR"/*.ppm; do
        python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[1][:-4]+'.png')" "$f"
    done
fi

echo
echo "== stage 48 FDE: $PASS passed, $FAIL failed =="
echo "   screenshots and serial logs: $SHOT_DIR/"
[ "$FAIL" -eq 0 ]
