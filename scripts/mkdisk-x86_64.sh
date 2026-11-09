#!/bin/bash
# ============================================================================
# scripts/mkdisk-x86_64.sh — собирает компактный FAT32-образ для AMPLIFY-OS
# 64-bit. На этапе 37.5b root содержит несколько простых текстовых файлов,
# которые kernel читает через ATA→FAT32 цепочку.
# ============================================================================

set -e
IMG=$1
[ -z "$IMG" ] && { echo "usage: $0 <image.img>"; exit 1; }

# mkfs.fat обычно лежит в /sbin — добавим в PATH если не найден.
if ! command -v mkfs.fat >/dev/null 2>&1; then
    export PATH="/sbin:/usr/sbin:$PATH"
fi
if ! command -v mkfs.fat >/dev/null 2>&1; then
    echo "[mkdisk-x86_64] FATAL: mkfs.fat not found (install dosfstools)"
    exit 1
fi

echo "[mkdisk-x86_64] creating 1 GB FAT32 image: $IMG"
dd if=/dev/zero of="$IMG" bs=1M count=1024 status=none

# 1 GB FAT32: даём mkfs.fat выбрать оптимальный sectors_per_cluster
# автоматически. Достаточно для всех пакетов Amply, БД, заметок, пейнтов и
# свободного места под пользовательские файлы.
mkfs.fat -F 32 -n AMPLIFY "$IMG" >/dev/null 2>&1

export MTOOLS_SKIP_CHECK=1
MTOOLSRC=/tmp/mtoolsrc_amplify_x86_64
echo "drive m: file=\"$IMG\"" > "$MTOOLSRC"
export MTOOLSRC

echo "[mkdisk-x86_64] populating root with sample files"

TMPF=$(mktemp)
echo ">>> Hello from FAT32 disk @ /disk/HELLO.TXT" > "$TMPF"
echo "If you see this in the smoke test — ATA + FAT32 read works!" >> "$TMPF"
mcopy -i "$IMG" "$TMPF" ::/HELLO.TXT >/dev/null
rm -f "$TMPF"

TMPF=$(mktemp)
cat > "$TMPF" <<'EOF'
AMPLIFY-OS 64-bit
Stage 37.5b: ATA PIO + FAT32 read-only.
EOF
mcopy -i "$IMG" "$TMPF" ::/BANNER.TXT >/dev/null
rm -f "$TMPF"

# Преднастроенная БД пользователей: admin / admin
# (FNV-1a 64-bit('admin') = e5cde7fdda328454).  Это убирает firstboot wizard
# при boot из чистого образа — удобно для headless-тестов.  В первый раз
# войдите как admin/admin и поменяйте пароль через будущую утилиту passwd.
TMPF=$(mktemp)
echo "admin:e5cde7fdda328454" > "$TMPF"
mcopy -i "$IMG" "$TMPF" ::/users.txt >/dev/null
rm -f "$TMPF"

rm -f "$MTOOLSRC"

echo "[mkdisk-x86_64] done"
