// ============================================================================
// crypto_rsa.c — RSA-PKCS#1 v1.5 signature verify (этап 47.2).
// ----------------------------------------------------------------------------
// Алгоритм verify (PKCS#1 §8.2.2):
//   1. m = signature^e mod n            (RSAVP1)
//   2. EM = I2OSP(m, k)                  (BE-encode в k байт, k = размер n)
//   3. EM должен иметь форму:
//        0x00 0x01 PS 0x00 T
//      где PS — ≥8 байт 0xFF padding;
//          T — DigestInfo = ASN.1 SEQUENCE { OID(hash) NULL, OCTET STRING(hash) }
//   4. Проверяем, что наш hash совпадает с hash'ем в T.
//
// DigestInfo prefix'ы (ASN.1 DER hardcoded) для трёх хешей.  Так делает
// большинство embedded TLS-стек (mbedTLS, BearSSL): полный ASN.1 parser
// для DigestInfo избыточен — это всегда фиксированная последовательность.
// ============================================================================
#include "crypto.h"

// DigestInfo OID-prefix:
//   SHA-1  : 30 21 30 09 06 05 2b 0e 03 02 1a 05 00 04 14
//   SHA-256: 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
//   SHA-384: 30 41 30 0d 06 09 60 86 48 01 65 03 04 02 02 05 00 04 30
//   SHA-512: 30 51 30 0d 06 09 60 86 48 01 65 03 04 02 03 05 00 04 40
// За prefix'ом следует сам hash (20/32/48/64 байт).
//
// SHA-1 prefix добавлен в этапе 47.6 — нужен для verify legacy CA chain'ов
// (несколько Mozilla root'ов, например DigiCert Global Root CA, до сих пор
// на SHA-1, и intermediate certs от старых CA тоже).

static const uint8_t DI_SHA1[] = {
    0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,
    0x05,0x00,0x04,0x14
};
static const uint8_t DI_SHA256[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
    0x05,0x00,0x04,0x20
};
static const uint8_t DI_SHA384[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,
    0x05,0x00,0x04,0x30
};
static const uint8_t DI_SHA512[] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,
    0x05,0x00,0x04,0x40
};

bool crypto_rsa_pkcs1_verify(const uint8_t* n_bytes, size_t n_len,
                             uint32_t e,
                             crypto_hash_id_t hash_id,
                             const uint8_t* hash, size_t hash_len,
                             const uint8_t* signature, size_t sig_len) {
    if (sig_len != n_len) return false;            // signature должен быть длиной с modulus
    if (n_len < 64 || n_len > CRYPTO_BN_BYTES) return false;

    const uint8_t* di_prefix = 0;
    size_t         di_len    = 0;
    size_t         h_expect  = 0;
    switch (hash_id) {
        case CRYPTO_HASH_SHA1:   di_prefix = DI_SHA1;   di_len = sizeof(DI_SHA1);   h_expect = 20; break;
        case CRYPTO_HASH_SHA256: di_prefix = DI_SHA256; di_len = sizeof(DI_SHA256); h_expect = 32; break;
        case CRYPTO_HASH_SHA384: di_prefix = DI_SHA384; di_len = sizeof(DI_SHA384); h_expect = 48; break;
        case CRYPTO_HASH_SHA512: di_prefix = DI_SHA512; di_len = sizeof(DI_SHA512); h_expect = 64; break;
        default: return false;
    }
    if (hash_len != h_expect) return false;

    // RSAVP1: m = sig^e mod n.
    crypto_bn_t n, sig, m;
    crypto_bn_from_bytes_be(&n,   n_bytes,    n_len);
    crypto_bn_from_bytes_be(&sig, signature,  sig_len);

    // sig должен быть < n.
    if (crypto_bn_cmp(&sig, &n) >= 0) return false;

    crypto_bn_modexp(&m, &sig, e, &n);

    // I2OSP: BE-encode m в n_len байт.
    uint8_t EM[CRYPTO_BN_BYTES];
    crypto_bn_to_bytes_be(&m, EM, n_len);

    // Проверка структуры PKCS#1 v1.5 type 1:
    //   EM[0] = 0x00
    //   EM[1] = 0x01
    //   EM[2..2+PS_len-1] = 0xFF (минимум 8 байт)
    //   EM[2+PS_len] = 0x00
    //   EM[2+PS_len+1 .. ] = DigestInfo || hash
    if (EM[0] != 0x00 || EM[1] != 0x01) return false;
    size_t i = 2;
    while (i < n_len && EM[i] == 0xFF) i++;
    if (i < 10) return false;                       // PS должен быть ≥8 байт (10 = 2 + 8)
    if (i >= n_len) return false;
    if (EM[i] != 0x00) return false;
    i++;

    // Остаток должен равно совпасть с (DigestInfo || hash).
    size_t expected_tail = di_len + hash_len;
    if (n_len - i != expected_tail) return false;
    for (size_t j = 0; j < di_len; j++) {
        if (EM[i + j] != di_prefix[j]) return false;
    }
    // Сам hash в EM сравниваем константно-по-времени.
    return crypto_eq_const(EM + i + di_len, hash, hash_len);
}
