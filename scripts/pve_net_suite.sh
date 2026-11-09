#!/bin/bash
# ============================================================================
# pve_net_suite.sh — сетевой набор подэтапа 48.2 одним запуском (на узле):
# тест test_stage48_2_net.sh (свой http-сервер), затем обзор настоящих сайтов
# survey_sites.sh с записью трафика гостя и поиском «вставших» соединений
# (tools/pcap_tcp.py: паузы дольше 1.5 с внутри TCP-потока).
# Запуск с ПК:  bash scripts/pve_test.sh <имя> pve_net_suite
# Итог теста — build/reg/net-suite-test.log, обзора — build/screens-sites/.
# ============================================================================
cd "$(dirname "$0")/.."
mkdir -p build/reg
bash scripts/test_stage48_2_net.sh > build/reg/net-suite-test.log 2>&1
grep -a -E 'FAIL|SKIP|passed' build/reg/net-suite-test.log
echo "--- survey"
PCAP="${TMPDIR:-/tmp}/amplify-sites.pcap"
rm -f "$PCAP"
AOS_PCAP="$PCAP" bash scripts/survey_sites.sh "${1:-scripts/sites.txt}"
echo "--- TCP flows with pauses > 1.5 s (0 = none)"
python3 tools/pcap_tcp.py "$PCAP" 1.5 10 > build/screens-sites/tcp-gaps.txt 2>&1
grep -c 'gaps>1.5s: [1-9]' build/screens-sites/tcp-gaps.txt
grep -a 'gaps>1.5s: [1-9]' build/screens-sites/tcp-gaps.txt | head -20
grep -a -E '\[tls\] handshake complete' build/screens-sites/serial.log | sed 's/.*complete: //' | sort | uniq -c | sort -rn
