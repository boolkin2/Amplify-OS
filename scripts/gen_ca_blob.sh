#!/bin/bash
# ============================================================================
# Генератор crypto_ca_blob.c — хранилище доверенных корней AMPLIFY-OS.
#
# Было (47.6): семь RSA-корней, выбранных вручную. Этого хватало для
# Let's Encrypt/DigiCert/GTS по RSA, но всё остальное (ECDSA-цепочки на P-384,
# GlobalSign R46/E46, Sectigo, GoDaddy, Certum, SSL.com ...) кончалось
# значком «HTTPS ?» — сертификат не проверен.
#
# Стало (48.2): в ядро идёт ВЕСЬ набор Mozilla из пакета ca-certificates
# (/etc/ssl/certs/*.pem, ~150 корней, ~190 КБ DER). Проверку ECDSA P-384 даёт
# crypto_p384.c; корень с неизвестным нам типом ключа просто не разберётся
# и в поиске издателя будет пропущен (crypto_ca_find_by_dn).
#
# Запуск вручную (в WSL), когда обновился пакет ca-certificates:
#   bash scripts/gen_ca_blob.sh kernel/arch/x86_64/crypto_ca_blob.c
# Результат лежит в дереве исходников — сборка на узле его не пересоздаёт
# (иначе два места сборки давали бы два разных ядра).
# ============================================================================
set -e
OUT="$1"
[ -z "$OUT" ] && { echo "usage: $0 out.c"; exit 1; }
SRC_DIR="${CA_DIR:-/etc/ssl/certs}"

TMP=$(mktemp -d /tmp/amplify-ca-XXXXXX)
trap "rm -rf $TMP" EXIT

# Каждый *.pem (ссылки пакета ca-certificates) -> DER; одинаковые файлы
# (две ссылки на один корень) берём один раз.
N=0
declare -A SEEN
: > "$TMP/list"
for pem in "$SRC_DIR"/*.pem; do
    [ -f "$pem" ] || continue
    der="$TMP/c$N.der"
    openssl x509 -in "$pem" -outform DER -out "$der" 2>/dev/null || continue
    [ -s "$der" ] || continue
    sum=$(sha256sum < "$der" | cut -c1-64)
    [ -n "${SEEN[$sum]:-}" ] && continue
    SEEN[$sum]=1
    name=$(basename "$pem" .pem | tr '_' ' ' | tr -cd 'A-Za-z0-9 .()-' | cut -c1-60)
    echo "c$N|$name" >> "$TMP/list"
    N=$((N + 1))
done
[ "$N" -ge 20 ] || { echo "only $N roots found in $SRC_DIR — install ca-certificates"; exit 1; }

{
cat <<EOF
// ============================================================================
// crypto_ca_blob.c — встроенное хранилище доверенных корней AMPLIFY-OS.
// ----------------------------------------------------------------------------
// АВТОГЕНЕРИРОВАНО: scripts/gen_ca_blob.sh из $SRC_DIR (набор Mozilla,
// пакет ca-certificates). Корней: $N. Руками не править.
// ============================================================================
#include "crypto_ca.h"

EOF
while IFS='|' read -r sym name; do
    echo "static const uint8_t g_ca_$sym[] = {"
    od -An -tx1 -v "$TMP/$sym.der" | tr -s ' \n' ' ' | sed 's/^ //; s/ $//' | awk '{
        for (i = 1; i <= NF; i++) {
            if (i % 16 == 1) printf "    ";
            printf "0x%s,", $i;
            if (i % 16 == 0) printf "\n";
        }
        if (NF % 16 != 0) printf "\n";
    }'
    echo "};"
done < "$TMP/list"
echo ""
echo "const crypto_ca_entry_t g_crypto_ca_store[] = {"
while IFS='|' read -r sym name; do
    printf '    { "%s", g_ca_%s, sizeof(g_ca_%s) },\n' "$name" "$sym" "$sym"
done < "$TMP/list"
echo "};"
echo ""
echo "const size_t g_crypto_ca_count = sizeof(g_crypto_ca_store) / sizeof(g_crypto_ca_store[0]);"
} > "$OUT"

TOTAL=$(cat "$TMP"/c*.der | wc -c)
echo "Generated $OUT: $N roots, $TOTAL bytes DER"
