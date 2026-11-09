#!/bin/bash
# ============================================================================
# scripts/test_stage47_send_recv.sh — TLS application data send/recv test.
# ----------------------------------------------------------------------------
# Поднимает openssl s_server -www (отвечает HTML с информацией о cipher),
# затем kernel делает https_get("https://10.0.2.2:4444/") в smoke 47.7.
# Проверяет что receive после handshake реально работает (decrypt OK).
# ============================================================================

set -e
cd "$(dirname "$0")/.."

PASS=0; FAIL=0
check_grep() {
    local name="$1"; local file="$2"; local pat="$3"
    if grep -aqE "$pat" "$file" 2>/dev/null; then
        echo "  PASS  $name"; PASS=$((PASS+1))
    else
        echo "  FAIL  $name (no '$pat')"; FAIL=$((FAIL+1))
    fi
}

echo "=========================================="
echo "  Stage 47.7 — TLS send/recv with openssl"
echo "=========================================="

TMP=/tmp/amplify-tls-sr
mkdir -p "$TMP"
if [ ! -f "$TMP/srv.crt" ]; then
    echo "[1/4] Generating RSA-2048 self-signed cert..."
    openssl req -x509 -newkey rsa:2048 -keyout "$TMP/srv.key" -out "$TMP/srv.crt" \
        -days 30 -nodes -subj "/CN=10.0.2.2" \
        -addext "subjectAltName=DNS:10.0.2.2,IP:10.0.2.2" 2>/dev/null
fi

echo "[2/4] Starting openssl s_server -www on 4444..."
openssl s_server -cert "$TMP/srv.crt" -key "$TMP/srv.key" \
    -accept 0.0.0.0:4444 \
    -tls1_2 -cipher "ECDHE-RSA-AES128-GCM-SHA256" \
    -named_curve prime256v1 \
    -no_ticket -www >"$TMP/server.log" 2>&1 &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null || true" EXIT

for i in 1 2 3 4 5 6 7 8 9 10; do
    if (echo > /dev/tcp/127.0.0.1/4444) 2>/dev/null; then
        echo "  server listening (after ${i} probes)"
        break
    fi
    sleep 0.3
done

# We need to write a custom test smoke that hits https://10.0.2.2:4444/
# Since we don't want to modify kernel for this — instead piggy back on smoke 47.7
# by setting AMPLIFY_TLS_INTERNET and overriding the test URL via define.
echo "[3/4] Building kernel with AMPLIFY_TLS_INTERNET=1 + custom URL..."
rm -f build/x86_64/kernel/kernel64_main.o build/initrd-x86_64.img build/esp.img
CFLAGS_EXTRA='-DAMPLIFY_TLS_INTERNET=1 -DAMPLIFY_TLS_LOCALHOST_ONLY=1' \
    make ARCH=x86_64 2>&1 | tail -3

echo "[4/4] Running QEMU..."
LOG=$(mktemp /tmp/amplify-sr-XXXXXX.log)
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    if [ -f "$f" ]; then OVMF="$f"; break; fi
done

if [ -n "$OVMF" ]; then
    timeout 120 qemu-system-x86_64 \
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

echo ""
echo "[Parsing $LOG]"
check_grep "boot OK"           "$LOG" "\\[crypto\\] self-test: PASS"
check_grep "TCP ESTABLISHED"   "$LOG" "\\[tcp\\] ESTABLISHED"
check_grep "TLS handshake OK"  "$LOG" "\\[tls\\] handshake complete"
check_grep "https_get ok"      "$LOG" "\\[smoke 47\\.7\\] result: OK"
check_grep "HTTP 200"          "$LOG" "code=200"
check_grep "body>0"            "$LOG" "body_len=[1-9]"

echo ""
echo "  Last 25 lines:"
tail -n 25 "$LOG" | sed 's/^/    /'

echo ""
echo "  Server log:"
tail -n 5 "$TMP/server.log" | sed 's/^/    /'
echo "=========================================="
echo "  RESULT: $PASS PASS, $FAIL FAIL"
echo "=========================================="
