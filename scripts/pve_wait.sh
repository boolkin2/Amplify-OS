#!/usr/bin/env bash
# ============================================================================
# pve_wait.sh — дождаться конца запуска, начатого pve_test.sh, и показать итог:
#   bash scripts/pve_wait.sh <имя_запуска> [шаблон grep для строк итога]
# Ждёт НА УЗЛЕ (один ssh-вызов, без опроса с ПК); при обрыве шлюза pve_run.sh
# просто начинает ждать заново. Запускать в фоне.
# ============================================================================
set -u
cd "$(dirname "$0")/.."
WORKDIR="${PVE_WORK:-/tmp/ai-work}"
PAT="${2:-FAIL|passed|failed|done rc|BUILD|error}"
TMP="${TMPDIR:-/tmp}/pve_wait_$$.sh"
cat > "$TMP" <<EOF
cd $WORKDIR/OS2 || exit 1
for i in \$(seq 1 720); do
    grep -aq '=== done' build/reg/$1.log 2>/dev/null && break
    grep -aq 'BUILD=[1-9]' build/reg/build-$1.log 2>/dev/null && break
    sleep 5
done
grep -a BUILD build/reg/build-$1.log 2>/dev/null | tail -1
grep -aE ' error' build/reg/build-$1.log 2>/dev/null | head -10
grep -aE '$PAT' build/reg/$1.log 2>/dev/null | tail -60
EOF
bash scripts/pve_run.sh "$TMP"; rc=$?; rm -f "$TMP"; exit $rc
