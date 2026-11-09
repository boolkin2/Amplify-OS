#!/bin/bash
# ============================================================================
# scripts/test_stage47_ecdsa.sh — live ECDHE-ECDSA-P256 handshake (этап 47.7).
# ----------------------------------------------------------------------------
# Поднимает openssl s_server с self-signed ECDSA-P256 сертификатом и
# cipher_suite ECDHE-ECDSA-AES128-GCM-SHA256 (0xC02B), затем выполняет TLS
# handshake из QEMU.  Подтверждает что наш стек поддерживает ECDSA leaf-cert
# (как у Cloudflare, Google, Let's Encrypt-with-ECDSA сертификатов).
#
# Использование: bash scripts/test_stage47_ecdsa.sh
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
echo "  Stage 47.7 LIVE — ECDHE-ECDSA P-256 e2e"
echo "=========================================="

# 1. Generate ECDSA-P256 self-signed cert.
TMP=/tmp/amplify-tls-ecdsa
mkdir -p "$TMP"
if [ ! -f "$TMP/srv.crt" ]; then
    echo "[1/4] Generating ECDSA-P256 self-signed cert for CN=10.0.2.2..."
    # Сначала EC param-файл с prime256v1.
    openssl ecparam -name prime256v1 -genkey -noout -out "$TMP/srv.key" 2>/dev/null
    openssl req -x509 -new -key "$TMP/srv.key" -out "$TMP/srv.crt" \
        -days 30 -subj "/CN=10.0.2.2" \
        -addext "subjectAltName=DNS:10.0.2.2,IP:10.0.2.2" 2>/dev/null
fi

# 2. Start openssl s_server with ONLY ECDHE-ECDSA-AES128-GCM-SHA256.
echo "[2/4] Starting openssl s_server (ECDHE-ECDSA only)..."
openssl s_server -cert "$TMP/srv.crt" -key "$TMP/srv.key" \
    -accept 0.0.0.0:4443 \
    -tls1_2 -cipher "ECDHE-ECDSA-AES128-GCM-SHA256" \
    -named_curve prime256v1 \
    -no_ticket -www >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null || true" EXIT

for i in 1 2 3 4 5 6 7 8 9 10; do
    if (echo > /dev/tcp/127.0.0.1/4443) 2>/dev/null; then
        echo "  server listening (after ${i} probes)"
        break
    fi
    sleep 0.3
done

# 3. Build kernel (no flags needed — все 3 cipher suites зашиты по умолчанию).
echo "[3/4] Building kernel..."
make ARCH=x86_64 >/dev/null 2>&1

# 4. Run QEMU with NAT.
echo "[4/4] Running QEMU with NAT..."
LOG=$(mktemp /tmp/amplify-ecdsa-XXXXXX.log)
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

# 5. Validate.
echo ""
echo "[Parsing $LOG]"
check_grep "log non-empty"             "$LOG" "."
check_grep "TLS connect"               "$LOG" "\\[smoke 47\\.5 live\\] connecting"
check_grep "ECDHE-ECDSA handshake OK"  "$LOG" "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"

echo ""
echo "  Last 30 lines:"
tail -n 30 "$LOG" | sed 's/^/    /'

echo "=========================================="
echo "  RESULT: $PASS/$EXPECTED PASS, $FAIL FAIL"
echo "=========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1
