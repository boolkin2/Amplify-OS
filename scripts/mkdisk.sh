#!/bin/bash
# ============================================================================
# mkdisk.sh - Скрипт создания виртуального диска с FAT32 для MyOS
# ============================================================================

IMG_FILE=$1

if [ -z "$IMG_FILE" ]; then
    echo "Использование: $0 <путь_к_образу>"
    exit 1
fi

echo "Создание образа 64MB..."
dd if=/dev/zero of=$IMG_FILE bs=1M count=64 status=none

echo "Форматирование в FAT32..."
# -F 32: FAT32
# -n MYOS: Метка тома
mkfs.fat -F 32 -n MYOS $IMG_FILE > /dev/null

echo "Создание иерархии каталогов..."
# Используем mtools
export MTOOLS_SKIP_CHECK=1
export MTOOLSRC=/tmp/mtoolsrc_myos
echo "drive m: file=\"$IMG_FILE\"" > $MTOOLSRC

# Создаем уникальные папки
mmd m:/vault
mmd m:/nexus
mmd m:/forge
mmd m:/echo
mmd m:/spark
mmd m:/archive

mmd m:/vault/docs
mmd m:/vault/media
mmd m:/nexus/config

# Создаем тестовый файл
echo "Dobro pozhalovat' v MyOS Phase 2!" > /tmp/test.txt
mcopy -i $IMG_FILE /tmp/test.txt ::/vault/docs/test.txt

rm -f /tmp/test.txt $MTOOLSRC

echo "Виртуальный диск готов: $IMG_FILE"
