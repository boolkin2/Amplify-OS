#!/usr/bin/env bash
# ============================================================================
# pve402_full_iso.sh — полный установочный ISO на стенд, ВМ 402 (Git Bash):
#   bash scripts/pve402_full_iso.sh upload <имя.iso>
#       build/myos-x86_64.iso узла (ядро + установщик + модели MIND из
#       build/mind + образы гостей из build/guests) -> хранилище ISO Proxmox
#       под именем <имя.iso>, рядом — <имя>.sha256. ВМ не трогает. Повтор
#       безопасен: тот же файл второй раз не копируется.
#   bash scripts/pve402_full_iso.sh switch <имя_запуска> <имя.iso>
#       ide2 ВМ 402 = этот ISO, порядок загрузки ide2;virtio1, stop/start,
#       serial ВМ -> /tmp/ai-serial.log. ВМ ПЕРЕЗАПУСКАЕТСЯ — сначала сказать
#       пользователю. Маркер по имени запуска: при обрыве шлюза pve_run.sh
#       повторяет скрипт с начала, а второй stop/start не нужен.
# ВМ после этого грузит установщик с ISO (live): Esc на первом экране — живая
# система поверх тестового диска, либо установка заново. Вернуть быструю
# заливку ядра — scripts/pve402_deploy.sh.
# `--boot` — ОТДЕЛЬНЫМ вызовом после смены ISO (CLAUDE.md §2).
# ============================================================================
set -u
cd "$(dirname "$0")/.."
WORKDIR="${PVE_WORK:-/tmp/ai-work}"
STORE=/mnt/pve/data/template/iso
TMP="${TMPDIR:-/tmp}/pve402_full_$$.sh"

case "${1:-}" in
upload)
    [ $# -ge 2 ] || { echo "usage: pve402_full_iso.sh upload <name.iso>"; exit 2; }
    cat > "$TMP" <<EOF
. ~/local/env.sh
cd $WORKDIR/OS2 || exit 1
ISO=build/myos-x86_64.iso
DST=$STORE/$2
[ -f "\$ISO" ] || { echo "\$ISO not found - build it first (make iso)"; exit 1; }
echo "El Torito entries:"
xorriso -indev "\$ISO" -report_el_torito plain 2>/dev/null | grep -E 'boot img' | cut -c1-110
echo "payload:"
xorriso -indev "\$ISO" -lsl /mind /guests 2>/dev/null | awk '{ print "  " \$5, \$NF }'
S=\$(sha256sum "\$ISO" | cut -d' ' -f1)
if [ -f "\$DST" ] && [ "\$(sha256sum "\$DST" | cut -d' ' -f1)" = "\$S" ]; then
    echo "already in the storage"
else
    cp "\$ISO" "\$DST.part" && mv "\$DST.part" "\$DST" || { echo "copy FAILED"; exit 1; }
fi
echo "\$S  $2" > "\${DST%.iso}.sha256"
ls -la "\$DST" | cut -c1-110
echo "UPLOADED sha256=\$S"
EOF
    ;;
switch)
    [ $# -ge 3 ] || { echo "usage: pve402_full_iso.sh switch <run-id> <name.iso>"; exit 2; }
    cat > "$TMP" <<EOF
M=$WORKDIR/fulliso-$2.marker
if [ -f "\$M" ]; then echo "already switched"; sudo -n qm status 402; exit 0; fi
[ -f $STORE/$3 ] || { echo "$3 is not in the storage - upload it first"; exit 1; }
touch "\$M"
pkill -f '[q]m terminal 402' 2>/dev/null
sudo -n qm stop 402
sudo -n qm set 402 --ide2 data:iso/$3,media=cdrom
sudo -n qm set 402 --boot 'order=ide2;virtio1;net0'
sudo -n qm start 402
rm -f /tmp/ai-serial.log
nohup script -qfc 'sudo qm terminal 402' /tmp/ai-serial.log >/dev/null 2>&1 &
sleep 1
sudo -n qm status 402
sudo -n qm config 402 | grep -E '^(boot|ide2):' | cut -c1-120
echo SWITCHED
EOF
    ;;
*)
    echo "usage: pve402_full_iso.sh upload <name.iso> | switch <run-id> <name.iso>"; exit 2;;
esac
bash scripts/pve_run.sh "$TMP"; rc=$?; rm -f "$TMP"; exit $rc
