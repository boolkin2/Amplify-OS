// ============================================================================
// crypto_x509.h — X.509 v3 certificate parsing + signature verify (этап 47.3).
// ----------------------------------------------------------------------------
// RFC 5280.  Минимум для TLS server-cert validation:
//   * Парсер TBSCertificate: serial, validity, issuer/subject DN,
//     SubjectPublicKeyInfo (RSA, Ed25519), SAN extension, BasicConstraints.
//   * Алгоритмы подписи: RSA-PKCS#1 v1.5 + SHA-256/384/512, Ed25519.
//   * Verify chain через root CA store (см. crypto_ca.h).
//
// ECDSA (P-256/P-384) — пока stub, добавим в 47.4 если ECDHE-сертификат
// попадётся при тестах HTTPS.
// ============================================================================
#ifndef AOS_X86_64_CRYPTO_X509_H
#define AOS_X86_64_CRYPTO_X509_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_SHA256,
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
    X509_SIG_ED25519,
    // ---- этап 47.6 ----
    X509_SIG_RSA_SHA1,         // legacy CA chains
    X509_SIG_ECDSA_SHA256,     // ECDSA-with-SHA-256 (1.2.840.10045.4.3.2)
    X509_SIG_ECDSA_SHA384,     // ECDSA-with-SHA-384 (1.2.840.10045.4.3.3)
    X509_SIG_ECDSA_SHA512,     // ECDSA-with-SHA-512 (1.2.840.10045.4.3.4), подэтап 48.2
} x509_sig_alg_t;

typedef enum {
    X509_PK_UNKNOWN = 0,
    X509_PK_RSA,
    X509_PK_ED25519,
    // ---- этап 47.6 ----
    X509_PK_EC_P256,           // SubjectPublicKeyInfo с id-ecPublicKey + secp256r1
    X509_PK_EC_P384,           // id-ecPublicKey + secp384r1 (подэтап 48.2, только проверка подписей)
} x509_pk_type_t;

// ---- этап 47.6: KeyUsage / BasicConstraints / Extended Key Usage ----------
// RFC 5280 §4.2.1.3 KeyUsage flags (bit positions в BIT STRING):
#define X509_KU_DIGITAL_SIGNATURE   (1u << 0)
#define X509_KU_NON_REPUDIATION     (1u << 1)
#define X509_KU_KEY_ENCIPHERMENT    (1u << 2)
#define X509_KU_DATA_ENCIPHERMENT   (1u << 3)
#define X509_KU_KEY_AGREEMENT       (1u << 4)
#define X509_KU_KEY_CERT_SIGN       (1u << 5)    // обязательно для CA
#define X509_KU_CRL_SIGN            (1u << 6)
#define X509_KU_ENCIPHER_ONLY       (1u << 7)
#define X509_KU_DECIPHER_ONLY       (1u << 8)

// Extended KeyUsage (RFC 5280 §4.2.1.12) — для leaf:
#define X509_EKU_SERVER_AUTH        (1u << 0)
#define X509_EKU_CLIENT_AUTH        (1u << 1)
#define X509_EKU_CODE_SIGNING       (1u << 2)
#define X509_EKU_EMAIL_PROTECTION   (1u << 3)

typedef struct {
    // ---- TBS-блок (raw bytes для signature verify) ----
    const uint8_t* tbs;
    size_t         tbs_len;

    // ---- Signature ----
    x509_sig_alg_t sig_alg;
    const uint8_t* signature;       // BIT STRING value минус leading "unused-bits" byte
    size_t         signature_len;

    // ---- Issuer / Subject (raw DER bytes for chain matching) ----
    const uint8_t* issuer_dn;
    size_t         issuer_dn_len;
    const uint8_t* subject_dn;
    size_t         subject_dn_len;

    // ---- Validity (Unix time) ----
    uint64_t       not_before;
    uint64_t       not_after;

    // ---- Subject Public Key ----
    x509_pk_type_t pk_type;
    // Для RSA:
    const uint8_t* rsa_modulus;     // указатель внутрь DER (без leading-0)
    size_t         rsa_modulus_len;
    uint32_t       rsa_exponent;    // обычно 65537
    // Для Ed25519:
    uint8_t        ed25519_pk[32];
    // Для EC P-256 (этап 47.6): uncompressed точка 65 байт (0x04 || X(32) || Y(32))
    uint8_t        ec_p256_pk[65];
    // Для EC P-384 (48.2): 0x04 || X(48) || Y(48) = 97 байт
    uint8_t        ec_p384_pk[97];

    // ---- Extensions ----
    bool           is_ca;           // BasicConstraints CA=TRUE
    bool           has_san;
    const uint8_t* san_value;       // raw OCTET STRING значение SAN (содержит SEQUENCE)
    size_t         san_value_len;

    // ---- этап 47.6: новые поля для strict validation ----
    bool           has_basic_constraints;
    int            path_len_constraint;  // -1 = no constraint, иначе максимум промежуточных
    bool           has_key_usage;
    uint32_t       key_usage;            // bitmask X509_KU_*
    bool           has_ext_key_usage;
    uint32_t       ext_key_usage;        // bitmask X509_EKU_*
} x509_cert_t;

// Парсит DER (cert_der, der_len) в *out.  Возвращает false при любой ошибке.
bool x509_parse(const uint8_t* der, size_t der_len, x509_cert_t* out);

// Проверяет signature cert'а под public-key issuer'а.  Для self-signed cert'а
// можно передать тот же cert как issuer.  Возвращает true только если подпись
// полностью валидна (algorithm + hash + cryptoverify).
bool x509_verify_signature(const x509_cert_t* cert, const x509_cert_t* issuer);

// Сравнивает Issuer DN cert'а с Subject DN issuer'а (raw DER bytewise).
bool x509_issuer_matches(const x509_cert_t* cert, const x509_cert_t* issuer);

// Проверяет, что cert'у есть SAN с DNS-именем, совпадающим с hostname
// (с поддержкой wildcard *.example.com).
bool x509_hostname_matches(const x509_cert_t* cert, const char* hostname);

// Проверяет, что unix_time в диапазоне [not_before, not_after].
bool x509_time_valid(const x509_cert_t* cert, uint64_t unix_time);

// ---- этап 47.6: strict chain validation ------------------------------------
// Результат полной проверки цепочки.
typedef enum {
    X509_CHAIN_OK                = 0,
    X509_CHAIN_ERR_NO_LEAF       = 1,
    X509_CHAIN_ERR_BAD_HOSTNAME  = 2,
    X509_CHAIN_ERR_EXPIRED       = 3,
    X509_CHAIN_ERR_NOT_YET_VALID = 4,
    X509_CHAIN_ERR_SIG_INVALID   = 5,
    X509_CHAIN_ERR_NO_TRUST      = 6,
    X509_CHAIN_ERR_BAD_CA_EXT    = 7,   // intermediate без BC.CA или KU.keyCertSign
    X509_CHAIN_ERR_PATH_TOO_LONG = 8,   // path_len_constraint нарушен
    X509_CHAIN_ERR_BAD_EKU       = 9,   // leaf без EKU serverAuth (если присутствует)
    X509_CHAIN_ERR_PARSE         = 10,
    X509_CHAIN_ERR_TOO_DEEP      = 11,  // chain длиннее, чем мы поддерживаем
} x509_chain_result_t;

#define X509_MAX_CHAIN  8

// Полная strict валидация цепочки сертификатов.
//
// Параметры:
//   chain_ders, chain_lens — массив DER-блобов сертификатов в порядке от leaf
//                           к корню (или хотя бы leaf + intermediates).
//   chain_count           — сколько сертификатов в chain.
//   hostname              — для match с SAN (NULL чтобы пропустить).
//   now_unix              — текущее unix-время (для time-based проверки;
//                           передайте 0 чтобы пропустить).
//
// Алгоритм:
//   1. Парсим все сертификаты.
//   2. Проверяем leaf:
//      * hostname в SAN (если задан).
//      * not_before <= now <= not_after (если now != 0).
//      * Если есть EKU, должен включать serverAuth.
//   3. Для каждого intermediate:
//      * BasicConstraints.CA = TRUE.
//      * KeyUsage (если присутствует) должен включать keyCertSign.
//      * Подписан следующим в chain или корнем из trust store.
//      * path_len_constraint не нарушен (счётчик заходящих cert'ов вверх).
//   4. Финальный cert chain должен заканчиваться сертификатом, подписанным
//      одним из root'ов в trust store (crypto_ca.c).
//
// Возвращает X509_CHAIN_OK при полной валидности, иначе код ошибки.
x509_chain_result_t x509_verify_chain(
    const uint8_t* const* chain_ders,
    const size_t*         chain_lens,
    size_t                chain_count,
    const char*           hostname,
    uint64_t              now_unix);

// Преобразовать код ошибки в человекочитаемую строку.
const char* x509_chain_result_str(x509_chain_result_t r);

#endif
