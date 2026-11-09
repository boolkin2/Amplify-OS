// ============================================================================
// crypto_ca.h — trust store (root CAs из системы ca-certificates).  Этап 47.3.
// ----------------------------------------------------------------------------
// Сам blob certификатов лежит в crypto_ca.c, который автогенерируется из
// /etc/ssl/certs/ скриптом scripts/gen_ca_blob.sh во время сборки.  Это та же
// схема, что используется во всех TLS-libraries (mbedTLS, BearSSL, GnuTLS):
// trust store берётся из публичного Mozilla NSS, перепакованного Debian/Ubuntu
// в пакете ca-certificates.
// ============================================================================
#ifndef AOS_X86_64_CRYPTO_CA_H
#define AOS_X86_64_CRYPTO_CA_H

#include <stdint.h>
#include <stddef.h>
#include "crypto_x509.h"

typedef struct {
    const char*    name;        // human-readable
    const uint8_t* der;
    size_t         der_len;
} crypto_ca_entry_t;

extern const crypto_ca_entry_t g_crypto_ca_store[];
extern const size_t            g_crypto_ca_count;

// Найти первый root CA, чей Subject DN совпадает с issuer_dn.  Парсит DER
// найденного CA в *out.  Возвращает true, если найдено.
bool crypto_ca_find_by_dn(const uint8_t* issuer_dn, size_t issuer_dn_len,
                          x509_cert_t* out);

#endif
