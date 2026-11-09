#!/usr/bin/env bash
# ============================================================================
# teardown_tap.sh — снятие TAP-интерфейса и NAT-правил.
# Запуск: sudo bash scripts/teardown_tap.sh
# ============================================================================
set -e
TAP_NAME=${TAP_NAME:-tap0}

if [ "$EUID" -ne 0 ]; then
  echo "ERROR: запусти через sudo: sudo bash $0" >&2
  exit 1
fi

ip link del "$TAP_NAME" 2>/dev/null || true
nft delete table ip qemu_nat 2>/dev/null || true
nft delete table ip qemu_filter 2>/dev/null || true

echo "[teardown_tap] $TAP_NAME удалён, NAT-правила сняты"
