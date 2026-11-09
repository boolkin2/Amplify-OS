#!/bin/bash
# ============================================================================
# scripts/test_stage47_internet.sh — HTTPS GET к реальному интернет-сайту.
# ----------------------------------------------------------------------------
# Запускает QEMU с -netdev user (slirp NAT с встроенным DNS на 10.0.2.3) и
# собирает kernel с флагом AMPLIFY_TLS_INTERNET, который активирует smoke 47.7:
# наш kernel делает https_get("https://example.com/") и потом
# https_get("https://www.cloudflare.com/").
#
# Это финальный e2e тест "реальный сайт через AMPLIFY-OS".  Требует:
#   * Работающий интернет на хосте (WSL → Windows → провайдер).
#   * QEMU slirp NAT (стандартный -netdev user) — резолвит DNS через 10.0.2.3.
#   * Нашего стека: DNS → TCP → TLS(ECDHE-RSA или ECDHE-ECDSA) → HTTP parse.
#
# Использование: bash scripts/test_stage47_internet.sh
# ============================================================================

set -e
cd "$(dirname "$0")/.."

echo "=========================================="
echo "  Stage 47.7 — REAL INTERNET HTTPS test"
echo "=========================================="

# 1. Проверка интернета на хосте.
echo "[1/3] Verifying host connectivity to example.com..."
if ! timeout 5 bash -c '(echo > /dev/tcp/93.184.215.14/443) 2>/dev/null'; then
    # 93.184.215.14 — один из IP example.com.  Если не отвечает — DNS попробуем.
    if ! timeout 5 getent hosts example.com >/dev/null 2>&1; then
        echo "  WARN: host cannot reach internet — test will likely fail"
    fi
fi

# 2. Build kernel с флагом.
echo "[2/3] Building kernel with AMPLIFY_TLS_INTERNET=1..."
# Делаем clean rebuild для kernel64_main.o + initrd чтобы пересобрать smoke.
rm -f build/x86_64/kernel/kernel64_main.o build/initrd-x86_64.img build/esp.img
CFLAGS_EXTRA="-DAMPLIFY_TLS_INTERNET=1" make ARCH=x86_64 2>&1 | tail -5

# 3. Run QEMU with NAT.
echo "[3/3] Running QEMU with -netdev user (slirp DNS 10.0.2.3)..."
LOG=$(mktemp /tmp/amplify-inet-XXXXXX.log)
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    if [ -f "$f" ]; then OVMF="$f"; break; fi
done

if [ -n "$OVMF" ]; then
    # Дать побольше времени — HTTPS на реальный сайт через TCG может занять
    # 30-60 секунд из-за медленной RSA/P-256 математики.
    timeout 180 qemu-system-x86_64 \
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

echo ""
echo "[Parsing $LOG]"
# Поищем все важные строки.
PASS=0; FAIL=0
check() {
    local name="$1"; local pat="$2"
    if grep -aqE "$pat" "$LOG" 2>/dev/null; then
        echo "  PASS  $name"; PASS=$((PASS+1))
    else
        echo "  FAIL  $name (no '$pat')"; FAIL=$((FAIL+1))
    fi
}

check "boot OK"            "\\[crypto\\] self-test: PASS"
check "DNS resolved"       "\\[dns\\] resolved"
check "TLS handshake done" "\\[tls\\] handshake complete"
check "smoke 47.7 ran"     "\\[smoke 47\\.7\\] GET"
check "got HTTP status"    "\\[smoke 47\\.7\\] result: OK code="

echo ""
echo "  Last 60 lines:"
tail -n 60 "$LOG" | sed 's/^/    /'

echo "=========================================="
echo "  Detected: $PASS PASS, $FAIL FAIL"
echo "=========================================="
