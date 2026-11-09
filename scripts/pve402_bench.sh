#!/usr/bin/env bash
# ============================================================================
# pve402_bench.sh — замер кадров на ВМ 402 (KVM, настоящая конфигурация ВМ).
# Запускать НА УЗЛЕ; система в ВМ загружена, выполнен вход, serial пишется в
# /tmp/ai-serial.log (`script -qfc 'sudo qm terminal 402' ...`), рядом лежит
# /tmp/pve402_keys.sh.   bash pve402_bench.sh [секунд на режим=7] [watch]
# Аргумент `watch` — сначала открыть терминал и включить `compstat watch`
# (если отчёт счётчиков ещё не включён).
# Режимы: окно терминала (простой / мышь), затем пустой стол (простой / мышь).
# Мышь двигается через монитор QEMU (`qm monitor`: mouse_move), ~80 событий/с.
# ============================================================================
SECS="${1:-7}"
K=/tmp/pve402_keys.sh
L=/tmp/ai-serial.log
wig() {
    ( end=$(( $(date +%s) + $1 )); i=0; dx=9; dy=4
      while [ "$(date +%s)" -lt "$end" ]; do
          echo "mouse_move $dx $dy"; i=$((i+1))
          [ $((i % 40)) = 0 ] && { dx=$((-dx)); dy=$((-dy)); }
          sleep 0.01
      done; echo quit ) | sudo -n qm monitor 402 >/dev/null 2>&1
}
mark() { wc -l < "$L"; }
rep()  { echo "-- $2 --"; tail -n +"$(( $1 + 1 ))" "$L" | grep -a -F '[fstat] fps' | tail -n +2 | tr -d '\r'; }

bash $K meta_l-spc; sleep 2
if [ "${2:-}" = watch ]; then bash $K "text:compstat watch" ret; sleep 2; fi
M=$(mark); sleep "$SECS"; rep "$M" "window idle"
M=$(mark); wig "$SECS";   rep "$M" "window + mouse"
bash $K meta_l-h; sleep 2
M=$(mark); sleep "$SECS"; rep "$M" "desktop idle"
M=$(mark); wig "$SECS";   rep "$M" "desktop + mouse"
ps -o %cpu= -p "$(pgrep -f '[k]vm.* -id 402 ' | head -1)" 2>/dev/null | sed 's/^/host %cpu of VM 402 (lifetime avg): /'
echo BENCH402_DONE
