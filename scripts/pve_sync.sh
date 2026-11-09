#!/usr/bin/env bash
# ============================================================================
# pve_sync.sh — отправить исходники AMPLIFY-OS на узел Proxmox (с 2026-10-01
# сборка и все тяжёлые тесты идут там, под KVM; см. CLAUDE.md §1, §2).
# ----------------------------------------------------------------------------
# Запускать из Git Bash на Windows:   bash scripts/pve_sync.sh
# Архив идёт потоком через ssh (SFTP для ai закрыт, scp — только с -O).
# build/ не отправляется: узел собирает сам. Модели MIND на узле берутся из
# ISO, уже лежащего в /mnt/pve/data/template/iso (2 ГБ по сети не гоняем).
# Удалённые файлы, которых больше нет в исходниках, НЕ стираются — после
# удаления/переименования исходника почистить каталог на узле руками.
#
#   PVE_WORK — рабочий каталог ai на узле (по умолчанию /mnt/pve/data/ai-work).
# ============================================================================
set -u
cd "$(dirname "$0")/.."
REMOTE="${PVE_WORK:-/mnt/pve/data/ai-work}/OS2"

for i in 1 2 3 4; do
    tar czf - --exclude=./build --exclude=./node_modules --exclude=./lectures \
              --exclude=./.claude . \
      | ssh.exe -o BatchMode=yes -o ConnectTimeout=25 pve-node \
          "mkdir -p '$REMOTE' && tar xzf - -C '$REMOTE' && echo SYNC_OK"
    rc=${PIPESTATUS[1]}
    [ "$rc" -ne 255 ] && exit "$rc"       # 255 — обрыв шлюза, повторяем
    sleep 10
done
exit 255
