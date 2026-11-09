#!/bin/bash
# Runs all 5 stage-47 test scripts in sequence.
set +e
cd "$(dirname "$0")/.."
echo "=== test_stage47_crypto ==="
bash scripts/test_stage47_crypto.sh 2>&1 | tail -3
echo "=== test_stage47_live ==="
bash scripts/test_stage47_live.sh 2>&1 | tail -3
echo "=== test_stage47_modern ==="
bash scripts/test_stage47_modern.sh 2>&1 | tail -3
echo "=== test_stage47_ecdsa ==="
bash scripts/test_stage47_ecdsa.sh 2>&1 | tail -3
echo "=== test_stage47_send_recv ==="
bash scripts/test_stage47_send_recv.sh 2>&1 | tail -3
