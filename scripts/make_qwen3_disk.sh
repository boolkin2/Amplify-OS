#!/usr/bin/env bash
# ============================================================================
# make_qwen3_disk.sh — тестовый системный диск для Qwen3-1.7B (подэтап 49.1).
# ----------------------------------------------------------------------------
# dev-образ build/disk-x86_64.img — 1 ГБ, модель 1.8 ГБ на него не влезает.
# Поэтому: свежий «голый» FAT32 (как dev-образ, без таблицы разделов — ядро
# находит его probe_fat32_at(dev, 0)) нужного размера, в него — всё дерево
# dev-образа, модель в /vault/mind и mind.conf. Кластер 32 КБ — меньше
# записей FAT на 1.8 ГБ весов.
#   bash scripts/make_qwen3_disk.sh <img> <size> <model|-> '<mind.conf с \n>'
# ============================================================================
set -eu
cd "$(dirname "$0")/.."
IMG="$1"; SIZE="$2"; MODEL="$3"; CONF="$4"
DEVDISK=build/disk-x86_64.img
export MTOOLS_SKIP_CHECK=1
TREE=$(mktemp -d)
trap 'rm -rf "$TREE"' EXIT
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
/usr/sbin/mkfs.fat -F 32 -s 64 -n AMPLIFY "$IMG" >/dev/null
mcopy -s -n -i "$DEVDISK" "::/*" "$TREE/" </dev/null
mcopy -s -D o -i "$IMG" "$TREE"/* ::/ </dev/null
mmd -D s -i "$IMG" ::/vault ::/vault/mind ::/Sanctum </dev/null 2>/dev/null || true
if [ "$MODEL" != "-" ]; then mcopy -D o -i "$IMG" "$MODEL" ::/vault/mind/ </dev/null; fi
printf "$CONF" > "$TREE/mind.conf"
mcopy -D o -i "$IMG" "$TREE/mind.conf" ::/Sanctum/mind.conf </dev/null
