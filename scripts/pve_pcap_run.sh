#!/bin/bash
# Разбор «вставших» страниц: обзор короткого списка с записью трафика гостя.
cd "$(dirname "$0")/.."
AOS_PCAP=/tmp/ai-work/sites.pcap bash scripts/survey_sites.sh scripts/sites_debug.txt
which tcpdump tshark 2>/dev/null
ls -la /tmp/ai-work/sites.pcap
