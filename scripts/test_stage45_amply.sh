#!/bin/bash
# ============================================================================
# scripts/test_stage45_amply.sh — автотест пакетного менеджера Amply.
# ----------------------------------------------------------------------------
# 1) Сборка ОС с initrd, содержащим amply_index.txt + 3 .amf пакета.
# 2) Проверка, что host-утилита mkamf корректно подписывает.
# 3) Headless boot через QEMU с дисковым образом, проверка serial-логов.
# 4) Через monitor-команды send-key мы пока не управляем shell'ом
#    интерактивно (нет PTY); вместо этого делаем "автозапуск" через
#    специальную команду в kernel boot path: kernel вызывает amply_init,
#    мы проверяем что он напечатал в serial.
#
# Этот скрипт НЕ требует X/SDL, идёт со -display none и пишет всё в serial.
# ============================================================================

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
EXPECTED=0

check() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local cmd="$2"
    if eval "$cmd" >/dev/null 2>&1; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

check_grep() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local file="$2"; local pat="$3"
    if grep -aqE "$pat" "$file" 2>/dev/null; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name (no '$pat' in $file)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "  Stage 45 — Amply package manager"
echo "=========================================="

echo ""
echo "[1/5] Build host tools"
check "tools/mkamf.c exists" "test -f tools/mkamf.c"
check "tools/mkinitrd.c exists" "test -f tools/mkinitrd.c"

echo ""
echo "[2/5] Build kernel + amply packages"
make ARCH=x86_64 >/tmp/amply_build.log 2>&1 || {
    echo "  FAIL: build failed, see /tmp/amply_build.log"
    exit 1
}
check "kernel-x86_64.bin built" "test -f build/kernel-x86_64.bin"
check ".amf welcome built"      "test -f build/amply/amply_welcome.amf"
check ".amf cyber-theme built"  "test -f build/amply/amply_cyber_theme.amf"
check ".amf notes-pack built"   "test -f build/amply/amply_notes_pack.amf"
check "amply_index.txt built"   "test -f build/amply/amply_index.txt"
check "initrd has .amf packages" \
    "strings build/initrd-x86_64.img | grep -q amply_welcome.amf"

echo ""
echo "[3/5] Validate .amf format"
# Magic 'AMF1' = 0x31 0x46 0x4D 0x41 little-endian → 'AMF1' ASCII в первых 4 байтах.
check "AMF1 magic in welcome.amf" \
    "head -c 4 build/amply/amply_welcome.amf | grep -q 'AMF1'"
check "AMF1 magic in cyber-theme.amf" \
    "head -c 4 build/amply/amply_cyber_theme.amf | grep -q 'AMF1'"
check "AMF1 magic in notes-pack.amf" \
    "head -c 4 build/amply/amply_notes_pack.amf | grep -q 'AMF1'"

echo ""
echo "[4/5] Boot AMPLIFY-OS, observe amply_init"
LOG=build/test_stage45.log
rm -f "$LOG"

# Регенерируем disk image, чтобы тест был детерминированным:
# Sanctum-каталога ещё нет, при amply_init будет создан (видимо в логе).
rm -f build/disk-x86_64.img
make ARCH=x86_64 build/disk-x86_64.img >/dev/null 2>&1 || true

# Find OVMF.
OVMF=""
for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$p" ] && OVMF="$p" && break
done

if [ -z "$OVMF" ]; then
    echo "  SKIP: OVMF.fd not found"
else
    timeout 90 qemu-system-x86_64 -bios "$OVMF" \
        -machine pc \
        -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/esp.img,if=ide,index=1 \
        -m 1G -smp cpus=4,cores=4,threads=1 \
        -vga std -display none \
        -serial file:"$LOG" \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
        -audiodev none,id=audio0 \
        -device AC97,audiodev=audio0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 || true

    check "boot log produced" "test -s $LOG"
    check_grep "fat32 subdir mkdir works" "$LOG" "\[fat32\] mkdir /disk/Sanctum"
    check_grep "amply repo loaded"        "$LOG" "\[amply\] repo loaded 3"
    check_grep "amply initialized"        "$LOG" "\[amply\] initialized"
    check_grep "initrd has amply_welcome" "$LOG" "amply_welcome\\.amf"
    check_grep "initrd has amply_index"   "$LOG" "amply_index\\.txt"
    # Smoke install/verify/remove cycle.
    check_grep "install signature valid" "$LOG" "signature valid"
    check_grep "install dep resolved"    "$LOG" "cyber-theme -> welcome"
    check_grep "install files unpacked"  "$LOG" "\\[install\\] /disk/Sanctum/themes/cyber/theme\\.cfg"
    check_grep "install ok"              "$LOG" "\\[smoke 45\\] install r=0 \\(OK\\)"
    check_grep "verify ok"               "$LOG" "\\[ok\\] verify cyber-theme OK"
    check_grep "remove deletes file"     "$LOG" "\\[remove\\] /disk/Sanctum/themes/cyber/"
    check_grep "remove db_count drops"   "$LOG" "db_count after remove=1"
fi

echo ""
echo "[5/5] Mkamf round-trip — manually parse our own .amf"
EXPECTED=$((EXPECTED + 1))
if python3 - <<'PY'; then PASS=$((PASS+1)); echo "  PASS  amf python round-trip"; else echo "  FAIL  amf python round-trip"; FAIL=$((FAIL+1)); fi
import struct, sys, glob

def fnv1a64(buf, seed):
    h = seed
    for b in buf:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

SEED_PUB = 0xCBF29CE484222325
SEED_SEC = 0x414D504C59475231

ok = True
for f in sorted(glob.glob('build/amply/amply_*.amf')):
    with open(f, 'rb') as fp:
        data = fp.read()
    magic, ver, mlen, fcnt, plen, comp, _, sig = struct.unpack('<IIIIQII16s', data[:48])
    if magic != 0x31464D41:
        print(f'  bad magic in {f}'); ok = False; continue
    if ver != 1:
        print(f'  bad ver in {f}');   ok = False; continue
    body = data[48:]
    sigbody_len = mlen + fcnt*120 + plen
    if len(body) != sigbody_len:
        print(f'  size mismatch in {f}: body={len(body)} vs expected={sigbody_len}')
        ok = False; continue
    sig_pub_calc = fnv1a64(body, SEED_PUB)
    sig_sec_calc = fnv1a64(body, SEED_SEC)
    sig_pub_file = struct.unpack('<Q', sig[0:8])[0]
    sig_sec_file = struct.unpack('<Q', sig[8:16])[0]
    if sig_pub_calc != sig_pub_file or sig_sec_calc != sig_sec_file:
        print(f'  sig mismatch in {f}'); ok = False; continue
    table_off = mlen
    paths = []
    for i in range(fcnt):
        ent = body[table_off + i*120 : table_off + i*120 + 120]
        path = ent[:96].split(b'\0', 1)[0].decode('utf-8', errors='replace')
        paths.append(path)
    print(f'    ok  {f}  files={fcnt}  paths={paths}')

sys.exit(0 if ok else 2)
PY

echo ""
echo "=========================================="
if [ $FAIL -eq 0 ]; then
    echo "  Stage 45 RESULT: $PASS/$EXPECTED PASS"
    exit 0
else
    echo "  Stage 45 RESULT: $PASS PASS, $FAIL FAIL"
    exit 1
fi
