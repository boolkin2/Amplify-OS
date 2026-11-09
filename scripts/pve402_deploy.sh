#!/usr/bin/env bash
# ============================================================================
# pve402_deploy.sh — залить свежее ядро на ВМ 402 без переустановки (Git Bash):
#   bash scripts/pve402_deploy.sh <имя_запуска>
# На узле: make dev-iso -> amplify-dev.iso в хранилище -> ide2 = этот ISO,
# порядок загрузки ide2;virtio1 (ядро с CD, система с диска), stop/start,
# serial ВМ пишется в /tmp/ai-serial.log, скрипт клавиш — в /tmp.
# Маркер по имени запуска: при обрыве шлюза pve_run.sh повторяет скрипт с
# начала, а второй stop/start ВМ посреди загрузки не нужен.
# `--boot` — ОТДЕЛЬНЫМ вызовом после смены ISO (CLAUDE.md §2).
# ============================================================================
set -u
cd "$(dirname "$0")/.."
[ $# -ge 1 ] || { echo "usage: pve402_deploy.sh <run-id>"; exit 2; }
WORKDIR="${PVE_WORK:-/tmp/ai-work}"
for i in 1 2 3 4; do
    scp.exe -O -o BatchMode=yes -o ConnectTimeout=25 scripts/pve402_keys.sh scripts/pve402_bench.sh pve-node:/tmp/ >/dev/null 2>&1 && break
    sleep 8
done
TMP="${TMPDIR:-/tmp}/pve402_deploy_$$.sh"
cat > "$TMP" <<EOF
. ~/local/env.sh
cd $WORKDIR/OS2 || exit 1
M=$WORKDIR/deploy-$1.marker
if [ -f "\$M" ]; then echo "already deployed"; sudo -n qm status 402; exit 0; fi
touch "\$M"
make -f Makefile.x86_64 dev-iso > build/reg/deviso-$1.log 2>&1 || { echo "dev-iso build FAILED"; tail -5 build/reg/deviso-$1.log; exit 1; }
cp build/amplify-dev.iso /mnt/pve/data/template/iso/amplify-dev.iso || exit 1
ls -la /mnt/pve/data/template/iso/amplify-dev.iso
pkill -f '[q]m terminal 402' 2>/dev/null
sudo -n qm stop 402
sudo -n qm set 402 --ide2 data:iso/amplify-dev.iso,media=cdrom
sudo -n qm set 402 --boot 'order=ide2;virtio1;net0'
sudo -n qm start 402
rm -f /tmp/ai-serial.log
nohup script -qfc 'sudo qm terminal 402' /tmp/ai-serial.log >/dev/null 2>&1 &
sleep 1
sudo -n qm status 402
echo DEPLOYED
EOF
bash scripts/pve_run.sh "$TMP"; rc=$?; rm -f "$TMP"; exit $rc
