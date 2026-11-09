#!/bin/bash
# ============================================================================
# scripts/test_stage47_crypto.sh — автотест криптопримитивов этапа 47.1.
# ----------------------------------------------------------------------------
#   1) Проверяем, что исходники этапа 47.1 на месте.
#   2) Собираем ядро + initrd + ESP.
#   3) Проверяем, что новые символы попали в kernel.bin:
#        crypto_init, crypto_sha256_buf, crypto_aes_encrypt_block,
#        crypto_chacha20_encrypt, crypto_poly1305_mac, crypto_rand_bytes,
#        crypto_self_test_run, crypto_gcm_encrypt.
#   4) Headless boot через QEMU, парсим serial-лог:
#        * "[crypto] CPU features:"
#        * "[crypto] PRNG initialized"
#        * "[crypto] self-test: PASS (15/15)"
#        * "[smoke 47.1] crypto_init=PASS"
#        * "[smoke 47.1] GCM round-trip with PRNG-key: OK"
#   5) Регрессия 46: smoke 46 блок всё ещё проходит (всё PASS).
#
# Без X/SDL: -display none + -serial file:LOG.
# ============================================================================

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
EXPECTED=0

check() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local cmd="$2"
    if eval "$cmd" >/dev/null 2>&1; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        FAIL=$((FAIL + 1))
    fi
}

check_grep() {
    EXPECTED=$((EXPECTED + 1))
    local name="$1"; local file="$2"; local pat="$3"
    if grep -aqE "$pat" "$file" 2>/dev/null; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name (no '$pat' in $file)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "  Stage 47.1 — Crypto primitives (SHA, HMAC, AES, ChaCha20, RNG)"
echo "=========================================="

echo ""
echo "[1/5] Sources present"
check "kernel/arch/x86_64/crypto.h"             "test -f kernel/arch/x86_64/crypto.h"
check "kernel/arch/x86_64/crypto_cpu.c"         "test -f kernel/arch/x86_64/crypto_cpu.c"
check "kernel/arch/x86_64/crypto_sha.c"         "test -f kernel/arch/x86_64/crypto_sha.c"
check "kernel/arch/x86_64/crypto_hmac.c"        "test -f kernel/arch/x86_64/crypto_hmac.c"
check "kernel/arch/x86_64/crypto_aes.c"         "test -f kernel/arch/x86_64/crypto_aes.c"
check "kernel/arch/x86_64/crypto_chacha.c"      "test -f kernel/arch/x86_64/crypto_chacha.c"
check "kernel/arch/x86_64/crypto_rand.c"        "test -f kernel/arch/x86_64/crypto_rand.c"
check "kernel/arch/x86_64/crypto_self_test.c"   "test -f kernel/arch/x86_64/crypto_self_test.c"
check "kernel/arch/x86_64/crypto_bn.c"          "test -f kernel/arch/x86_64/crypto_bn.c"
check "kernel/arch/x86_64/crypto_rsa.c"         "test -f kernel/arch/x86_64/crypto_rsa.c"
check "kernel/arch/x86_64/crypto_curve.c"       "test -f kernel/arch/x86_64/crypto_curve.c"
check "kernel/arch/x86_64/crypto_p256.c"        "test -f kernel/arch/x86_64/crypto_p256.c"
check "kernel/arch/x86_64/crypto_asn1.c"        "test -f kernel/arch/x86_64/crypto_asn1.c"
check "kernel/arch/x86_64/crypto_x509.c"        "test -f kernel/arch/x86_64/crypto_x509.c"
check "kernel/arch/x86_64/crypto_ca.c"          "test -f kernel/arch/x86_64/crypto_ca.c"
check "kernel/arch/x86_64/crypto_ca_blob.c"     "test -f kernel/arch/x86_64/crypto_ca_blob.c"
check "kernel/arch/x86_64/tls.h"                "test -f kernel/arch/x86_64/tls.h"
check "kernel/arch/x86_64/tls.c"                "test -f kernel/arch/x86_64/tls.c"
check "kernel/arch/x86_64/https.h"              "test -f kernel/arch/x86_64/https.h"
check "kernel/arch/x86_64/https.c"              "test -f kernel/arch/x86_64/https.c"

echo ""
echo "[2/5] Build kernel + initrd + ESP"
make ARCH=x86_64 all >/dev/null 2>&1
check "kernel-x86_64.bin built"  "test -f build/kernel-x86_64.bin"
check "initrd-x86_64.img built"  "test -f build/initrd-x86_64.img"
check "esp.img built"            "test -f build/esp.img"

echo ""
echo "[3/5] Symbols in kernel.bin"
KERNEL=build/kernel-x86_64.bin
check_grep "crypto_init"             "$KERNEL" "crypto_init"
check_grep "crypto_sha256_buf"       "$KERNEL" "crypto_sha256_buf"
check_grep "crypto_aes_encrypt_block" "$KERNEL" "crypto_aes_encrypt_block"
check_grep "crypto_chacha20_encrypt" "$KERNEL" "crypto_chacha20_encrypt"
check_grep "crypto_poly1305_mac"     "$KERNEL" "crypto_poly1305_mac"
check_grep "crypto_rand_bytes"       "$KERNEL" "crypto_rand_bytes"
check_grep "crypto_self_test_run"    "$KERNEL" "crypto_self_test_run"
check_grep "crypto_bn_modexp"        "$KERNEL" "crypto_bn_modexp"
check_grep "crypto_rsa_pkcs1_verify" "$KERNEL" "crypto_rsa_pkcs1_verify"
check_grep "crypto_x25519"           "$KERNEL" "crypto_x25519"
check_grep "crypto_ed25519_verify"   "$KERNEL" "crypto_ed25519_verify"
check_grep "x509_parse"              "$KERNEL" "x509_parse"
check_grep "x509_verify_signature"   "$KERNEL" "x509_verify_signature"
check_grep "asn1_parse"              "$KERNEL" "asn1_parse"
check_grep "crypto_ca_find_by_dn"    "$KERNEL" "crypto_ca_find_by_dn"
check_grep "tls_client_handshake"    "$KERNEL" "tls_client_handshake"
check_grep "tls_send"                "$KERNEL" "tls_send"
check_grep "tls_recv"                "$KERNEL" "tls_recv"
check_grep "https_get"               "$KERNEL" "https_get"
check_grep "tcp_listen"              "$KERNEL" "tcp_listen"
check_grep "tcp_accept"              "$KERNEL" "tcp_accept"
# 47.6 — modern HTTPS (ECDHE-RSA P-256 + SHA-1 + strict chain + ALPN):
check_grep "crypto_sha1_buf"         "$KERNEL" "crypto_sha1_buf"
check_grep "crypto_p256_ecdh"        "$KERNEL" "crypto_p256_ecdh"
check_grep "crypto_p256_derive_pub"  "$KERNEL" "crypto_p256_derive_pub"
check_grep "crypto_p256_ecdsa_verify" "$KERNEL" "crypto_p256_ecdsa_verify"
check_grep "x509_verify_chain"       "$KERNEL" "x509_verify_chain"
check_grep "x509_chain_result_str"   "$KERNEL" "x509_chain_result_str"

echo ""
echo "[4/5] Headless QEMU boot — smoke 47.1"

# Найти OVMF firmware.
OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd \
         /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/qemu/OVMF.fd; do
    if [ -f "$f" ]; then OVMF="$f"; break; fi
done

LOG=$(mktemp /tmp/amplify-stage47-XXXXXX.log)
trap "rm -f $LOG" EXIT

if [ -n "$OVMF" ]; then
    echo "  Using OVMF: $OVMF"
    timeout 60 qemu-system-x86_64 \
        -bios "$OVMF" \
        -machine pc -cpu max \
        -drive format=raw,file=build/disk-x86_64.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/esp.img,if=ide,index=1 \
        -m 1G -smp cpus=2,cores=2,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -serial file:"$LOG" -display none \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
        -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 || true
else
    echo "  OVMF not found — fallback to BIOS/GRUB path"
    timeout 60 qemu-system-x86_64 \
        -cdrom build/myos-x86_64.iso -boot d \
        -cpu max -m 1G -smp cpus=2,cores=2,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0,media=disk \
        -serial file:"$LOG" -display none \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
        -audiodev none,id=audio0 -device AC97,audiodev=audio0 \
        -no-reboot -no-shutdown >/dev/null 2>&1 || true
fi

check "log produced"            "test -s $LOG"
check_grep "CPU features line"  "$LOG" "\\[crypto\\] CPU features:"
check_grep "PRNG initialized"   "$LOG" "\\[crypto\\] PRNG initialized"
check_grep "self-test PASS"     "$LOG" "\\[crypto\\] self-test: PASS \\(28/28\\)"
check_grep "smoke 47.1 init"    "$LOG" "\\[smoke 47\\.1\\] crypto_init=PASS"
check_grep "smoke 47.1 GCM RT"  "$LOG" "\\[smoke 47\\.1\\] GCM round-trip with PRNG-key: OK"
check_grep "smoke 47.2 X25519"  "$LOG" "\\[smoke 47\\.2\\] ss_alice == ss_bob: OK"
# Этап 47.7 — расширили trust store до 7 RSA-roots (см. scripts/gen_ca_blob.sh).
# Регекс по N/N, не по фиксированным "2 2".
check_grep "smoke 47.3 CA OK"   "$LOG" "\\[smoke 47\\.3\\] parsed=[0-9]+ self-signed-verified=([0-9]+)/\\1 \\(OK\\)"
check_grep "smoke 47.4 TLS"     "$LOG" "\\[smoke 47\\.4\\] ctx ready: OK"
check_grep "smoke 47.5 listen"  "$LOG" "\\[smoke 47\\.5\\] tcp_listen=ok state=LISTEN=yes \\(OK\\)"
# 47.6 — Modern HTTPS:
check_grep "smoke 47.6 SHA-1"   "$LOG" "\\[smoke 47\\.6\\] SHA-1\\(empty\\): OK"
check_grep "smoke 47.6 ECDH"    "$LOG" "\\[smoke 47\\.6\\] P-256 ECDH self-roundtrip: OK"
check_grep "smoke 47.6 chain"   "$LOG" "\\[smoke 47\\.6\\] chain\\(root only\\) result: OK \\(OK\\)"

echo ""
echo "[5/5] Regression — stage 46 smoke still passes"
check_grep "smoke 46 cgroup OK"  "$LOG" "\\[smoke 46\\] cgroup mem.*\\(OK\\)"
check_grep "smoke 46 seccomp OK" "$LOG" "\\[smoke 46\\] seccomp strict.*\\(OK\\)"
check_grep "smoke 46 ns OK"      "$LOG" "\\[smoke 46\\] ns_unshare.*\\(OK\\)"
check_grep "smoke 46 mnt OK"     "$LOG" "\\[smoke 46\\] mnt-ns allow=yes deny=yes \\(OK\\)"
check_grep "smoke 46 sandbox OK" "$LOG" "\\[smoke 46\\] sandbox initialized, cgroup_count="

echo ""
echo "=========================================="
echo "  RESULT: $PASS/$EXPECTED PASS, $FAIL FAIL"
echo "=========================================="

if [ $FAIL -ne 0 ]; then
    echo ""
    echo "  serial log tail:"
    tail -n 60 "$LOG" | sed 's/^/    /'
    exit 1
fi
exit 0
