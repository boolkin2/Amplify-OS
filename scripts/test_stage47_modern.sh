#!/bin/bash
# ============================================================================
# scripts/test_stage47_modern.sh — live ECDHE-RSA TLS handshake (этап 47.6).
# ----------------------------------------------------------------------------
# Расширение test_stage47_live.sh: вместо TLS_RSA_WITH_AES_128_GCM_SHA256
# поднимаем openssl s_server с **TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256**
# (openssl name: ECDHE-RSA-AES128-GCM-SHA256, hex 0xC02F) и проверяем,
# что наш TLS-клиент успешно делает handshake через ECDHE-RSA с P-256.
#
# Это ключевой тест этапа 47.6 — ECDH P-256 + RSA-signed SKE + strict chain
# validation в действии.
#
# Использование: bash scripts/test_stage47_modern.sh
# ============================================================================

set -e
cd "$(dirname "$0")/.."

PASS=0; FAIL=0; EXPECTED=0
check_grep() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local file="$2"; local pat="$3"
    if grep -aqE "$pat" "$file" 2>/dev/null; then
        echo "  PASS  $name"; PASS=$((PASS + 1))
    else
        echo "  FAIL  $name (no '$pat')"; FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "  Stage 47.6 LIVE — ECDHE-RSA P-256 e2e test"
echo "=========================================="

# 1. Generate self-signed RSA-2048 cert for "10.0.2.2".
TMP=/tmp/amplify-tls-modern
mkdir -p "$TMP"
if [ ! -f "$TMP/srv.crt" ]; then
    echo "[1/4] Generating self-signed RSA-2048 cert for CN=10.0.2.2..."
    openssl req -x509 -newkey rsa:2048 -keyout "$TMP/srv.key" -out "$TMP/srv.crt" \
        -days 30 -nodes -subj "/CN=10.0.2.2" \
        -addext "subjectAltName=DNS:10.0.2.2,IP:10.0.2.2" 2>/dev/null
fi

# 2. Start openssl s_server in background — ТОЛЬКО ECDHE-RSA suite.
echo "[2/4] Starting openssl s_server on 0.0.0.0:4443 (ECDHE-RSA-AES128-GCM-SHA256)..."
# -named_curve P-256, -cipher only ECDHE-RSA-AES128-GCM-SHA256.
openssl s_server -cert "$TMP/srv.crt" -key "$TMP/srv.key" \
    -accept 0.0.0.0:4443 \
    -tls1_2 -cipher "ECDHE-RSA-AES128-GCM-SHA256" \
    -named_curve prime256v1 \
    -no_ticket -www >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null || true" EXIT

# Wait for actual listening.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if (echo > /dev/tcp/127.0.0.1/4443) 2>/dev/null; then
        echo "  server listening on 127.0.0.1:4443 (after ${i} probes)"
        break
    fi
    sleep 0.3
done

# 3. Build kernel.
echo "[3/4] Building kernel..."
make ARCH=x86_64 >/dev/null 2>&1

# 4. Run QEMU with NAT.
echo "[4/4] Running QEMU with NAT..."
LOG=$(mktemp /tmp/amplify-modern-XXXXXX.log)
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    if [ -f "$f" ]; then OVMF="$f"; break; fi
done

if [ -n "$OVMF" ]; then
    timeout 90 qemu-system-x86_64 \
        -bios "$OVMF" -machine pc -cpu max \
        -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/esp.img,if=ide,index=1 \
        -m 1G -smp 2 \
        -vga none -device virtio-vga,xres=1024,yres=768 \
        -serial file:"$LOG" -display none \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
        -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 || true
fi

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# 5. Validate: serial should show ECDHE handshake completed.
echo ""
echo "[Parsing $LOG]"
check_grep "log non-empty"            "$LOG" "."
check_grep "TLS connect"              "$LOG" "\\[smoke 47\\.5 live\\] connecting"
# 47.6 — ключевая проверка: handshake complete с ECDHE-RSA cipher suite.
check_grep "ECDHE handshake complete" "$LOG" "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"
# 47.6 smoke-блоки внутри ядра:
check_grep "47.6 SHA-1"               "$LOG" "\\[smoke 47\\.6\\] SHA-1\\(empty\\): OK"
check_grep "47.6 P-256 ECDH"          "$LOG" "\\[smoke 47\\.6\\] P-256 ECDH self-roundtrip: OK"
check_grep "47.6 chain root OK"       "$LOG" "\\[smoke 47\\.6\\] chain\\(root only\\) result: OK"
check_grep "self-test PASS"           "$LOG" "\\[crypto\\] self-test: PASS \\(26/26\\)"

echo ""
echo "  Last 40 lines:"
tail -n 40 "$LOG" | sed 's/^/    /'

echo "=========================================="
echo "  RESULT: $PASS/$EXPECTED PASS, $FAIL FAIL"
echo "=========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1
