#!/usr/bin/env bash
# ssh на узел Proxmox с повторами: шлюз pve-ui.example.local иногда не
# отвечает (Connection timed out). Код 255 — ошибка соединения, повторяем;
# другие коды — результат удалённой команды.
#   bash scripts/pve402_ssh.sh "<команда>"
for i in 1 2 3 4 5 6; do
    ssh.exe -o BatchMode=yes -o ConnectTimeout=25 pve-node "$1"
    rc=$?
    [ $rc -ne 255 ] && exit $rc
    sleep 10
done
exit 255
