#!/usr/bin/env bash
# ============================================================================
# pve_test.sh — отправить исходники на узел Proxmox, собрать там и запустить
# один тест в фоне (Git Bash на Windows):
#   bash scripts/pve_test.sh <имя_запуска> <тест без .sh> [clean]
#   bash scripts/pve_test.sh r1 test_stage48_2_net clean
# Результат забирать отдельно:  bash scripts/pve_test.sh --log <имя_запуска> [строк]
# Имя запуска каждый раз новое: по нему ставится маркер на узле, и повтор
# скрипта при обрыве шлюза (pve_run.sh) не запускает сборку второй раз.
# `clean` — стереть объектные файлы ядра (нужно после правки заголовков).
# ============================================================================
set -u
cd "$(dirname "$0")/.."
WORKDIR="${PVE_WORK:-/tmp/ai-work}"
TMP="${TMPDIR:-/tmp}/pve_test_$$.sh"

if [ "${1:-}" = "--log" ]; then
    cat > "$TMP" <<EOF
cd $WORKDIR/OS2 || exit 1
tail -3 build/reg/build-$2.log 2>/dev/null | grep -a BUILD
grep -aE ' error|warning: ' build/reg/build-$2.log 2>/dev/null | grep -v 'gles.c\|app_glcube\|s_strcat' | head -20
tail -n ${3:-60} build/reg/$2.log 2>/dev/null
EOF
    bash scripts/pve_run.sh "$TMP"; rc=$?; rm -f "$TMP"; exit $rc
fi

[ $# -ge 2 ] || { echo "usage: pve_test.sh <run-id> <test> [clean] | --log <run-id> [lines]"; exit 2; }
CLEAN=0; [ "${3:-}" = clean ] && CLEAN=1
# Успех синхронизации — только по строке SYNC_OK: код возврата конвейера с
# `tail` ничего не говорит, а сборка из наполовину распакованных исходников
# хуже, чем отказ.
OUT=$(PVE_WORK="$WORKDIR" bash scripts/pve_sync.sh 2>&1)
echo "$OUT" | grep -q SYNC_OK || { echo "$OUT" | tail -3; echo "sync FAILED — nothing started"; exit 1; }
echo SYNC_OK
cat > "$TMP" <<EOF
. ~/local/env.sh
cd $WORKDIR/OS2 || exit 1
M=$WORKDIR/run-$1.marker
if [ -f "\$M" ]; then echo "already started"; exit 0; fi
touch "\$M"
mkdir -p build/reg
nohup bash -c '
  . ~/local/env.sh
  cd $WORKDIR/OS2
  [ $CLEAN = 1 ] && rm -rf build/x86_64/kernel
  make -f Makefile.x86_64 -j16 all > build/reg/build-$1.log 2>&1
  echo BUILD=\$? >> build/reg/build-$1.log
  bash scripts/$2.sh > build/reg/$1.log 2>&1
  echo "=== done rc=\$? ===" >> build/reg/$1.log
' > /dev/null 2>&1 &
echo "started $1"
EOF
bash scripts/pve_run.sh "$TMP"; rc=$?; rm -f "$TMP"; exit $rc
