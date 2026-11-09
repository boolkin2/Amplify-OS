#!/bin/bash
# ============================================================================
# scripts/test_stage47_live.sh — live TLS handshake test (этап 47.5).
# ----------------------------------------------------------------------------
# Запускает openssl s_server на host'е WSL с RSA cipher suite, затем стартует
# QEMU с -netdev user (NAT, host=10.0.2.2).  Ядро в boot path должно увидеть
# флаг и попробовать TLS-handshake к 10.0.2.2:4443.
#
# Это полноценный end-to-end smoke: примитивы + bignum + RSA + X.509 + TLS
# все вместе работают против реальной OpenSSL-стороны.  В отличие от
# scripts/test_stage47_crypto.sh, этот тест требует сетевой setup в QEMU
# (-netdev user) и время на handshake (~10 сек на TCG).
#
# Использование: bash scripts/test_stage47_live.sh
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
echo "  Stage 47 LIVE — TLS handshake e2e test"
echo "=========================================="

# 1. Generate self-signed cert for "10.0.2.2".
TMP=/tmp/amplify-tls-live
mkdir -p "$TMP"
if [ ! -f "$TMP/srv.crt" ]; then
    echo "[1/4] Generating self-signed RSA-2048 cert for CN=10.0.2.2..."
    openssl req -x509 -newkey rsa:2048 -keyout "$TMP/srv.key" -out "$TMP/srv.crt" \
        -days 30 -nodes -subj "/CN=10.0.2.2" 2>/dev/null
fi

# 2. Start openssl s_server in background.
echo "[2/4] Starting openssl s_server on 0.0.0.0:4443 (RSA suite)..."
# Force only TLS_RSA_WITH_AES_128_GCM_SHA256 (openssl name: AES128-GCM-SHA256).
openssl s_server -cert "$TMP/srv.crt" -key "$TMP/srv.key" \
    -accept 0.0.0.0:4443 \
    -tls1_2 -cipher "AES128-GCM-SHA256" \
    -no_ticket -www >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
# Wait for actual listening — probe via timeout 5.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if (echo > /dev/tcp/127.0.0.1/4443) 2>/dev/null; then
        echo "  server listening on 127.0.0.1:4443 (after ${i} probes)"
        break
    fi
    sleep 0.3
done

# 3. Build kernel with LIVE smoke enabled.
echo "[3/4] Building kernel with AMPLIFY_TLS_LIVE=1..."
make ARCH=x86_64 CFLAGS_EXTRA="-DAMPLIFY_TLS_LIVE=1" >/dev/null 2>&1 || true
# Если make не передал CFLAGS_EXTRA в наш Makefile — соберём напрямую.
# Простейший trick: добавим define в Makefile или используем env.
# Здесь полагаемся на pre-defined макрос в kernel64_main.c (если он есть).
make ARCH=x86_64 >/dev/null 2>&1

# 4. Run QEMU with NAT.
echo "[4/4] Running QEMU with NAT (host=10.0.2.2 accessible)..."
LOG=$(mktemp /tmp/amplify-live-XXXXXX.log)
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

# 5. Stop server.
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# 6. Check log.
echo ""
echo "[Parsing $LOG]"
check_grep "log non-empty"           "$LOG" "."
check_grep "TLS live: connecting"    "$LOG" "\\[smoke 47\\.5 live\\] connecting"
check_grep "TLS live: handshake OK"  "$LOG" "\\[tls\\] handshake complete"

echo ""
echo "  Last 30 lines:"
tail -n 30 "$LOG" | sed 's/^/    /'

echo "=========================================="
echo "  RESULT: $PASS/$EXPECTED PASS, $FAIL FAIL"
echo "=========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1
