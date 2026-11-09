#!/usr/bin/env bash
# ============================================================================
# test_stage57_installer.sh — сквозной тест установщика AMPLIFY-OS (этап 57).
# ----------------------------------------------------------------------------
# Фаза 1 (установка):  QEMU + OVMF грузит build/myos-x86_64.iso как CD
#   (UEFI-ветка: El Torito efiboot.img → BOOTX64.EFI → live.flag → ядро с
#   cmdline "live installer") рядом с ПУСТЫМ целевым диском.  Через монитор
#   QEMU (sendkey) проходим все шесть экранов: WELCOME → LICENSE (принять) →
#   DESTINATION → OPTIONS (логин/пароль, тёмная тема) → INSTALL → COMPLETE.
#   Serial-лог должен показать "[install] completed successfully".
# Фаза 2 (проверка диска на хосте): GPT + разделы читаются sfdisk/mtools,
#   на ESP лежат EFI/BOOT/BOOTX64.EFI, kernel.bin, initrd.img, на системном
#   разделе — users.txt, Sanctum/system.conf.
# Фаза 3 (загрузка установленной системы): QEMU + OVMF только с целевым
#   диском.  Ядро должно смонтировать системный раздел по GPT, загрузить
#   пользователя и пустить его на экране входа с паролем из установщика.
#
# Скриншоты каждого экрана — в build/screens-57/*.ppm (+ .png, если есть PIL).
# Запуск из WSL:  bash scripts/test_stage57_installer.sh
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ISO=build/myos-x86_64.iso
SHOT_DIR=build/screens-57
WORK=/tmp/amplify-stage57
TARGET="$WORK/target.img"
LOG1="$WORK/serial-install.log"
LOG2="$WORK/serial-installed.log"
MON_PORT=4577
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
[ -f "$ISO" ]  || { echo "$ISO not found — run: make ARCH=x86_64 iso"; exit 1; }

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
rm -f "$LOG1" "$LOG2" "$TARGET" "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.png
truncate -s 4G "$TARGET"

QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; true; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Монитор QEMU через bash /dev/tcp.
# ---------------------------------------------------------------------------
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

wait_log() {  # wait_log <file> <pattern> <timeout_s>
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

start_qemu() {  # start_qemu <serial-log> <drive args...>
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
echo "== Phase 1: install from ISO (UEFI) onto an empty 4G disk =="
start_qemu "$LOG1" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    $TGT_DEV,bootindex=1 \
    -drive file="$ISO",format=raw,if=none,id=live,media=cdrom,readonly=on \
    -device ide-cd,drive=live,bus=ide.1,unit=0,bootindex=0

check "UEFI loader saw live.flag"          'wait_log "$LOG1" "[installer] live mode requested" 90'
check "installer UI started"               'wait_log "$LOG1" "[installer] starting" 60'
check "one install target detected"        'grep -q "\[install\] targets found: 1" "$LOG1"'
sleep 2; shot 01-welcome

key ret 0.6;                shot 02-license
key spc 0.4;                shot 03-license-accepted
key ret 0.6;                shot 04-destination
key ret 0.6
key ret 0.6;                shot 05a-options-invalid   # пустые поля → подсветка ошибок
key tab;  type_text "$USER_NAME"                 # ADMINISTRATOR
key tab;  type_text "$USER_PASS"                 # PASSPHRASE
key tab;  type_text "$USER_PASS"                 # CONFIRM PASSPHRASE
key tab;  key tab                                # VAULT → DARK
key spc 0.4                                      # тёмная тема
shot 05-options
key ret 0.6;                shot 05b-confirm     # INSTALL NOW → диалог
key ret 0.3                                      # ERASE AND INSTALL
sleep 1.5; shot 06-installing

# [41.2] 1200 с вместо 300: под KVM диск на IDE (PIO) в разы МЕДЛЕННЕЕ, чем
# под TCG — каждое обращение к порту IDE (а их несколько на сектор) это выход
# в гипервизор и в процесс QEMU. Копия SmolLM2 (145 МБ, запись + сверка) под
# KVM на узле Proxmox не укладывалась в 300 с; под TCG хватало с запасом.
check "installation completed"             'wait_log "$LOG1" "[install] completed successfully" 1200'
sleep 2.5; shot 07-complete
grep -q "\[install\] ERROR" "$LOG1" && grep "\[install\] ERROR" "$LOG1"
# [50.5] Образы гостей phantom (/guests на ISO — всё из build/guests). Там, где
# гостей можно запускать (KVM с вложенной виртуализацией: у ВМ теста есть
# VT-x), установщик копирует их в /vault/phantom; где нельзя (TCG) — честно
# пропускает, иначе установка на IDE-диск растянулась бы на минуты.
NG=$(ls build/guests/*.iso 2>/dev/null | wc -l)
GUESTS_COPIED=0
if [ "$NG" -gt 0 ]; then
    NEST=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null)
    if echo "$CPU" | grep -q enable-kvm && { [ "$NEST" = Y ] || [ "$NEST" = 1 ]; }; then
        GUESTS_COPIED=1
        grep "\[install\] guest image" "$LOG1" | tr -d '\r' | cut -c1-150 | sed 's/^/        /'
        check "guest images for Phantom copied and verified ($NG)" '[ "$(grep -c "\[install\] guest image copied: /vault/phantom/" "$LOG1")" = "$NG" ]'
    else
        check "guest images skipped where guests cannot run (no VT-x)" 'grep -q "\[install\] guest images skipped: no usable VT-x" "$LOG1"'
    fi
fi
stop_qemu

# ===========================================================================
echo "== Phase 2: inspect the target disk from the host =="
if command -v sfdisk >/dev/null 2>&1 || [ -x /usr/sbin/sfdisk ]; then
    SFDISK=$(command -v sfdisk || echo /usr/sbin/sfdisk)
    PT=$("$SFDISK" -d "$TARGET" 2>/dev/null)
    check "GPT label present"               'echo "$PT" | grep -q "label: gpt"'
    check "EFI System partition"            'echo "$PT" | grep -q "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"'
    check "system data partition"           'echo "$PT" | grep -q "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"'
    ESP_START=$(echo "$PT" | grep -m1 "C12A7328" | sed -E 's/.*start= *([0-9]+).*/\1/')
    SYS_START=$(echo "$PT" | grep -m1 "EBD0A0A2" | sed -E 's/.*start= *([0-9]+).*/\1/')
    if [ -n "$ESP_START" ] && [ -n "$SYS_START" ]; then
        ESP="$TARGET@@$((ESP_START*512))"; SYS="$TARGET@@$((SYS_START*512))"
        check "ESP: EFI/BOOT/BOOTX64.EFI"   'mdir -i "$ESP" ::/EFI/BOOT 2>/dev/null | grep -q BOOTX64'
        check "ESP: kernel.bin"             'mdir -i "$ESP" ::/ 2>/dev/null | grep -q "kernel.bin"'
        check "ESP: initrd.img"             'mdir -i "$ESP" ::/ 2>/dev/null | grep -q "initrd.img"'
        check "ESP: no live.flag"           '! mdir -i "$ESP" ::/ 2>/dev/null | grep -q "live.flag"'
        mcopy -o -i "$ESP" ::/kernel.bin "$WORK/kernel.bin" 2>/dev/null
        check "ESP: kernel.bin byte-identical" 'cmp -s "$WORK/kernel.bin" build/kernel-x86_64.bin'
        check "SYS: users.txt has $USER_NAME" 'mtype -i "$SYS" ::/users.txt 2>/dev/null | grep -q "^$USER_NAME:"'
        check "SYS: system.conf"            'mtype -i "$SYS" ::/Sanctum/system.conf 2>/dev/null | grep -q "installed=1"'
        check "SYS: dark theme saved"       'mtype -i "$SYS" ::/Sanctum/system/ui-config.ini 2>/dev/null | grep -q "dark=1"'
        if [ $GUESTS_COPIED = 1 ]; then
            G=$(ls build/guests/*.iso | head -1)
            mcopy -o -i "$SYS" "::/vault/phantom/$(basename "$G")" "$WORK/guest.iso" </dev/null 2>/dev/null
            check "SYS: guest image byte-identical ($(basename "$G"))" 'cmp -s "$WORK/guest.iso" "$G"'
            rm -f "$WORK/guest.iso"
        fi
        if command -v fsck.fat >/dev/null 2>&1 || [ -x /usr/sbin/fsck.fat ]; then
            FSCK=$(command -v fsck.fat || echo /usr/sbin/fsck.fat)
            for part in ESP SYS; do
                PSTART=$([ $part = ESP ] && echo "$ESP_START" || echo "$SYS_START")
                PSIZE=$(echo "$PT" | grep -m1 "start= *$PSTART," | sed -E 's/.*size= *([0-9]+).*/\1/')
                # conv=sparse: системный раздел — гигабайты нулей, а /tmp в WSL —
                # tmpfs размером с половину RAM.  Без дыр извлечённый образ
                # туда просто не помещается.
                dd if="$TARGET" of="$WORK/$part.part" bs=1M iflag=skip_bytes,count_bytes \
                   skip=$((PSTART*512)) count=$((PSIZE*512)) conv=sparse status=none
                if "$FSCK" -n "$WORK/$part.part" > "$SHOT_DIR/fsck-$part.txt" 2>&1; then
                    ok "fsck.fat: $part clean"
                else
                    bad "fsck.fat: $part clean"; sed 's/^/        /' "$SHOT_DIR/fsck-$part.txt" | head -15
                fi
                rm -f "$WORK/$part.part"
            done
        fi
    fi
else
    echo "  (sfdisk not found — skipping host-side checks)"
fi

# ===========================================================================
echo "== Phase 3: boot the installed disk (UEFI, no installation media) =="
start_qemu "$LOG2" \
    -drive file="$TARGET",format=raw,if=none,id=target \
    $TGT_DEV,bootindex=0

check "firmware booted the installed ESP"  'wait_log "$LOG2" "[fat32] mounted" 90'
check "installer NOT started"              '! grep -q "\[installer\] starting" "$LOG2"'
check "system partition mounted via GPT"   'grep "\[fat32\] mounted" "$LOG2" | grep -qv "base_lba=0 "'
check "installed user loaded"              'wait_log "$LOG2" "[users] loaded 1 record" 30'
check "hostname applied from system.conf"  'wait_log "$LOG2" "[sysconf] hostname=amplify" 60'
check "dark theme applied from ui-config"  'wait_log "$LOG2" "[sysconf] theme=dark" 10'
# Последняя строка перед экраном входа — RTC-smoke; дальше login_run().
wait_log "$LOG2" "[smoke 37.6c-1] RTC" 60
sleep 2; shot 08-installed-login
key ret 0.4                                      # имя уже выбрано → поле пароля
type_text "$USER_PASS"
key ret 1
check "login with installer credentials"  'wait_log "$LOG2" "[identity] unlocked user=$USER_NAME" 30'
sleep 4; shot 09-installed-desktop
stop_qemu

# ===========================================================================
echo "== Phase 4: the same ISO through legacy BIOS + GRUB =="
LOG3="$WORK/serial-bios.log"; rm -f "$LOG3"
truncate -s 3G "$WORK/blank.img"
qemu-system-x86_64 $CPU -machine pc -m 1G -smp cpus=2,cores=2,threads=1 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display none -serial "file:$LOG3" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive file="$WORK/blank.img",format=raw,if=ide,index=0,media=disk \
    -cdrom "$ISO" -boot d -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }
check "GRUB passed 'live installer' cmdline" 'wait_log "$LOG3" "[installer] live mode requested" 120'
check "installer UI started under BIOS"      'wait_log "$LOG3" "[installer] starting" 60'
check "GRUB framebuffer is FullHD"           'grep -q "1920x1080" "$LOG3"'
sleep 2; shot 10-bios-welcome
stop_qemu
rm -f "$WORK/blank.img"
cp -f "$LOG3" "$SHOT_DIR/" 2>/dev/null

# PPM → PNG, если есть PIL (Linux) — иначе оставляем PPM.
if python3 -c "import PIL" 2>/dev/null; then
    for f in "$SHOT_DIR"/*.ppm; do
        python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[1][:-4]+'.png')" "$f"
    done
fi

# /tmp в WSL — tmpfs и исчезает при остановке дистрибутива: логи — рядом
# со скриншотами.
cp -f "$LOG1" "$LOG2" "$SHOT_DIR/" 2>/dev/null

echo
echo "== stage 57 installer: $PASS passed, $FAIL failed =="
echo "   serial logs: $SHOT_DIR/$(basename "$LOG1"), $SHOT_DIR/$(basename "$LOG2")"
[ "$FAIL" -eq 0 ]
