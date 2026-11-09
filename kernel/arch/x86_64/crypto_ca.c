// ============================================================================
// crypto_ca.c — lookup в trust store (этап 47.3).
// ----------------------------------------------------------------------------
// DER blobs самих CA лежат в crypto_ca_blob.c (автогенерируется скриптом
// scripts/gen_ca_blob.sh из /etc/ssl/certs/).
// ============================================================================
#include "crypto_ca.h"

bool crypto_ca_find_by_dn(const uint8_t* issuer_dn, size_t issuer_dn_len,
                          x509_cert_t* out) {
    for (size_t i = 0; i < g_crypto_ca_count; i++) {
        x509_cert_t cand;
        if (!x509_parse(g_crypto_ca_store[i].der,
                        g_crypto_ca_store[i].der_len,
                        &cand)) {
            continue;
        }
        if (cand.subject_dn_len != issuer_dn_len) continue;
        bool match = true;
        for (size_t j = 0; j < issuer_dn_len; j++) {
            if (cand.subject_dn[j] != issuer_dn[j]) { match = false; break; }
        }
        if (match) {
            *out = cand;
            return true;
        }
    }
    return false;
}
