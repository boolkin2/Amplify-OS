// ============================================================================
// crypto_x509.c — X.509 v3 parser + signature verify (этап 47.3).
// ----------------------------------------------------------------------------
// Парсер написан с нуля по RFC 5280 §4.1.  Поддерживает достаточно для:
//   * Self-signed root CA (DER form, ~1.3 KB).
//   * Цепочка root → intermediate → leaf (Let's Encrypt, DigiCert, etc.).
//   * RSA-PKCS#1 v1.5 SHA-256/384/512 + Ed25519 signatures.
//   * SAN/dNSName + BasicConstraints/CA для матчинга hostname'а и
//     ограничения leaf'ов.
//
// Не поддерживаем (отложено на 47.4 / 47.5):
//   * ECDSA P-256 (нет в 47.2 crypto stack'е).
//   * CRL и OCSP revocation checks.
//   * Name Constraints, Policy Constraints.
//   * RSASSA-PSS, RSA-OAEP.
// ============================================================================
#include "crypto_x509.h"
#include "crypto_asn1.h"
#include "crypto.h"
#include "crypto_ca.h"

// ---- OID bytes (без tag/length, только value) ------------------------------
// RSA-PKCS#1 v1.5 sig OIDs:
static const uint8_t OID_RSA_SHA1[]     = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x05 };  // sha1WithRSAEncryption (этап 47.6)
static const uint8_t OID_RSA_SHA256[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const uint8_t OID_RSA_SHA384[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const uint8_t OID_RSA_SHA512[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
static const uint8_t OID_RSA_ENC[]      = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const uint8_t OID_ED25519[]      = { 0x2B,0x65,0x70 };

// ECDSA sig OIDs (этап 47.6):
//   ecdsa-with-SHA256 = 1.2.840.10045.4.3.2 = 06 08 2A 86 48 CE 3D 04 03 02
//   ecdsa-with-SHA384 = 1.2.840.10045.4.3.3 = 06 08 2A 86 48 CE 3D 04 03 03
//   id-ecPublicKey   = 1.2.840.10045.2.1   = 06 07 2A 86 48 CE 3D 02 01
//   prime256v1       = 1.2.840.10045.3.1.7 = 06 08 2A 86 48 CE 3D 03 01 07
static const uint8_t OID_ECDSA_SHA256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const uint8_t OID_ECDSA_SHA384[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
static const uint8_t OID_EC_PUBKEY[]    = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const uint8_t OID_SECP256R1[]    = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
// [48.2] ecdsa-with-SHA512 = 1.2.840.10045.4.3.4, secp384r1 = 1.3.132.0.34
static const uint8_t OID_ECDSA_SHA512[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04 };
static const uint8_t OID_SECP384R1[]    = { 0x2B,0x81,0x04,0x00,0x22 };

// X.509 v3 extension OIDs:
static const uint8_t OID_EXT_SAN[]      = { 0x55,0x1D,0x11 };
static const uint8_t OID_EXT_BC[]       = { 0x55,0x1D,0x13 };
static const uint8_t OID_EXT_KU[]       = { 0x55,0x1D,0x0F };  // KeyUsage (этап 47.6)
static const uint8_t OID_EXT_EKU[]      = { 0x55,0x1D,0x25 };  // ExtKeyUsage (этап 47.6)

// Extended Key Usage values (RFC 5280 §4.2.1.12):
static const uint8_t OID_EKU_SERVER_AUTH[]      = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x01 };
static const uint8_t OID_EKU_CLIENT_AUTH[]      = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x02 };
static const uint8_t OID_EKU_CODE_SIGNING[]     = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x03 };
static const uint8_t OID_EKU_EMAIL_PROTECTION[] = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x04 };

static x509_sig_alg_t map_sig_oid(const asn1_t* oid) {
    if (asn1_oid_eq(oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))) return X509_SIG_RSA_SHA256;
    if (asn1_oid_eq(oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384))) return X509_SIG_RSA_SHA384;
    if (asn1_oid_eq(oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512))) return X509_SIG_RSA_SHA512;
    if (asn1_oid_eq(oid, OID_RSA_SHA1,   sizeof(OID_RSA_SHA1)))   return X509_SIG_RSA_SHA1;
    if (asn1_oid_eq(oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) return X509_SIG_ECDSA_SHA256;
    if (asn1_oid_eq(oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))) return X509_SIG_ECDSA_SHA384;
    if (asn1_oid_eq(oid, OID_ECDSA_SHA512, sizeof(OID_ECDSA_SHA512))) return X509_SIG_ECDSA_SHA512;
    if (asn1_oid_eq(oid, OID_ED25519,    sizeof(OID_ED25519)))   return X509_SIG_ED25519;
    return X509_SIG_UNKNOWN;
}

// Парсит SubjectPublicKeyInfo и записывает соответствующие поля в out.
//
// SubjectPublicKeyInfo ::= SEQUENCE {
//     algorithm           AlgorithmIdentifier,
//     subjectPublicKey    BIT STRING
// }
//
// AlgorithmIdentifier ::= SEQUENCE {
//     algorithm   OBJECT IDENTIFIER,
//     parameters  ANY DEFINED BY algorithm OPTIONAL
// }
//
// Для EC: parameters = OID secp256r1; subjectPublicKey = uncompressed EC point.
static bool parse_spki(const uint8_t* der, size_t len, x509_cert_t* out) {
    asn1_t spki;
    if (!asn1_parse(der, len, &spki))         return false;
    if (spki.tag != ASN1_TAG_SEQUENCE)        return false;

    const uint8_t* p = spki.val;
    asn1_t alg_seq, alg_oid, key_bits;
    if (!asn1_parse_at(&p, spki.end, &alg_seq)) return false;
    if (alg_seq.tag != ASN1_TAG_SEQUENCE)       return false;

    // Парсим алгоритм-OID + (опционально) parameters.
    const uint8_t* alg_q = alg_seq.val;
    if (!asn1_parse_at(&alg_q, alg_seq.end, &alg_oid)) return false;
    if (alg_oid.tag != ASN1_TAG_OID)                   return false;

    // Optional parameters — для EC нужен curve OID.
    asn1_t alg_params;
    bool has_params = false;
    if (alg_q < alg_seq.end) {
        if (asn1_parse_at(&alg_q, alg_seq.end, &alg_params)) {
            has_params = true;
        }
    }

    if (!asn1_parse_at(&p, spki.end, &key_bits)) return false;
    if (key_bits.tag != ASN1_TAG_BITSTRING)      return false;
    if (key_bits.len < 1)                        return false;
    // Первый байт BIT STRING — number of unused bits (для X.509 keys всегда 0).
    if (key_bits.val[0] != 0x00)                 return false;
    const uint8_t* key_data = key_bits.val + 1;
    size_t         key_len  = key_bits.len   - 1;

    if (asn1_oid_eq(&alg_oid, OID_RSA_ENC, sizeof(OID_RSA_ENC))) {
        // key_data = SEQUENCE { modulus INTEGER, exponent INTEGER }
        asn1_t rsa_seq;
        if (!asn1_parse(key_data, key_len, &rsa_seq)) return false;
        if (rsa_seq.tag != ASN1_TAG_SEQUENCE)         return false;
        const uint8_t* q = rsa_seq.val;
        asn1_t mod, exp;
        if (!asn1_parse_at(&q, rsa_seq.end, &mod))    return false;
        if (!asn1_parse_at(&q, rsa_seq.end, &exp))    return false;
        if (!asn1_int_as_bytes(&mod, &out->rsa_modulus, &out->rsa_modulus_len)) return false;
        if (!asn1_int_to_u32(&exp, &out->rsa_exponent)) return false;
        out->pk_type = X509_PK_RSA;
        return true;
    }
    if (asn1_oid_eq(&alg_oid, OID_ED25519, sizeof(OID_ED25519))) {
        if (key_len != 32) return false;
        for (int i = 0; i < 32; i++) out->ed25519_pk[i] = key_data[i];
        out->pk_type = X509_PK_ED25519;
        return true;
    }
    // EC P-256 (этап 47.6): id-ecPublicKey + secp256r1 parameter.
    if (asn1_oid_eq(&alg_oid, OID_EC_PUBKEY, sizeof(OID_EC_PUBKEY))) {
        if (!has_params || alg_params.tag != ASN1_TAG_OID)               return false;
        // [48.2] P-384: такими ключами подписаны почти все ECDSA-цепочки.
        if (asn1_oid_eq(&alg_params, OID_SECP384R1, sizeof(OID_SECP384R1))) {
            if (key_len != 97 || key_data[0] != 0x04) return false;
            for (int i = 0; i < 97; i++) out->ec_p384_pk[i] = key_data[i];
            out->pk_type = X509_PK_EC_P384;
            return true;
        }
        if (!asn1_oid_eq(&alg_params, OID_SECP256R1, sizeof(OID_SECP256R1))) return false;
        // key_data = uncompressed EC point: 0x04 || X(32) || Y(32) = 65 байт.
        if (key_len != 65 || key_data[0] != 0x04) return false;
        for (int i = 0; i < 65; i++) out->ec_p256_pk[i] = key_data[i];
        out->pk_type = X509_PK_EC_P256;
        return true;
    }
    out->pk_type = X509_PK_UNKNOWN;
    return false;
}

// Парсит Extensions ([3] EXPLICIT SEQUENCE OF Extension).
// Заполняет is_ca, has_san и san_value/_len, has_basic_constraints+path_len,
// has_key_usage+key_usage, has_ext_key_usage+ext_key_usage (этап 47.6).
static bool parse_extensions(const uint8_t* der, size_t len, x509_cert_t* out) {
    asn1_t exts_outer;
    if (!asn1_parse(der, len, &exts_outer))    return false;
    // Should be SEQUENCE (after [3] EXPLICIT wrapper unwrapped by caller).
    if (exts_outer.tag != ASN1_TAG_SEQUENCE)   return false;

    out->path_len_constraint = -1;       // sentinel: не задан

    const uint8_t* p = exts_outer.val;
    while (p < exts_outer.end) {
        asn1_t ext;
        if (!asn1_parse_at(&p, exts_outer.end, &ext)) return false;
        if (ext.tag != ASN1_TAG_SEQUENCE)             return false;
        const uint8_t* q = ext.val;
        asn1_t oid, maybe_crit, octet;
        if (!asn1_parse_at(&q, ext.end, &oid))        return false;
        if (oid.tag != ASN1_TAG_OID)                  return false;
        if (!asn1_parse_at(&q, ext.end, &maybe_crit)) return false;
        if (maybe_crit.tag == ASN1_TAG_BOOLEAN) {
            // critical flag, skip и читаем дальше OCTET STRING.
            if (!asn1_parse_at(&q, ext.end, &octet))  return false;
        } else {
            octet = maybe_crit;
        }
        if (octet.tag != ASN1_TAG_OCTETSTRING)        return false;

        if (asn1_oid_eq(&oid, OID_EXT_SAN, sizeof(OID_EXT_SAN))) {
            out->has_san       = true;
            out->san_value     = octet.val;
            out->san_value_len = octet.len;
        } else if (asn1_oid_eq(&oid, OID_EXT_BC, sizeof(OID_EXT_BC))) {
            // BasicConstraints ::= SEQUENCE { cA BOOLEAN DEFAULT FALSE, pathLenConstraint INTEGER OPTIONAL }
            asn1_t bc_seq;
            if (!asn1_parse(octet.val, octet.len, &bc_seq)) continue;
            if (bc_seq.tag != ASN1_TAG_SEQUENCE)            continue;
            out->has_basic_constraints = true;
            const uint8_t* r = bc_seq.val;
            if (r < bc_seq.end) {
                asn1_t fld;
                if (asn1_parse_at(&r, bc_seq.end, &fld)) {
                    if (fld.tag == ASN1_TAG_BOOLEAN && fld.len == 1 && fld.val[0] != 0) {
                        out->is_ca = true;
                        // pathLenConstraint следует только если ca=TRUE
                        if (r < bc_seq.end) {
                            asn1_t plen;
                            if (asn1_parse_at(&r, bc_seq.end, &plen) &&
                                plen.tag == ASN1_TAG_INTEGER) {
                                uint32_t pv;
                                if (asn1_int_to_u32(&plen, &pv)) {
                                    out->path_len_constraint = (int)pv;
                                }
                            }
                        }
                    }
                }
            }
        } else if (asn1_oid_eq(&oid, OID_EXT_KU, sizeof(OID_EXT_KU))) {
            // KeyUsage ::= BIT STRING
            // Внутри octet — DER-encoded BIT STRING.
            asn1_t bit;
            if (!asn1_parse(octet.val, octet.len, &bit)) continue;
            if (bit.tag != ASN1_TAG_BITSTRING || bit.len < 1) continue;
            uint8_t unused = bit.val[0];
            // Формат BIT STRING: [unused bits][bytes]; биты идут MSB-first.
            // RFC 5280 §4.2.1.3: bits 0..8 → digitalSignature..decipherOnly.
            // Мы маппим bit i в bit i нашего uint32_t (little-endian convention).
            uint32_t ku = 0;
            for (size_t i = 1; i < bit.len; i++) {
                uint8_t b = bit.val[i];
                int base = (int)((i - 1) * 8);
                int max_bit = 7;
                if (i == bit.len - 1) max_bit = 7 - (int)unused;
                for (int j = 0; j <= max_bit; j++) {
                    if (b & (0x80 >> j)) {
                        ku |= (1u << (base + j));
                    }
                }
            }
            out->has_key_usage = true;
            out->key_usage     = ku;
        } else if (asn1_oid_eq(&oid, OID_EXT_EKU, sizeof(OID_EXT_EKU))) {
            // ExtKeyUsage ::= SEQUENCE OF KeyPurposeId (OID)
            asn1_t seq;
            if (!asn1_parse(octet.val, octet.len, &seq)) continue;
            if (seq.tag != ASN1_TAG_SEQUENCE) continue;
            uint32_t eku = 0;
            const uint8_t* r = seq.val;
            while (r < seq.end) {
                asn1_t purpose;
                if (!asn1_parse_at(&r, seq.end, &purpose)) break;
                if (purpose.tag != ASN1_TAG_OID) continue;
                if (asn1_oid_eq(&purpose, OID_EKU_SERVER_AUTH, sizeof(OID_EKU_SERVER_AUTH)))
                    eku |= X509_EKU_SERVER_AUTH;
                else if (asn1_oid_eq(&purpose, OID_EKU_CLIENT_AUTH, sizeof(OID_EKU_CLIENT_AUTH)))
                    eku |= X509_EKU_CLIENT_AUTH;
                else if (asn1_oid_eq(&purpose, OID_EKU_CODE_SIGNING, sizeof(OID_EKU_CODE_SIGNING)))
                    eku |= X509_EKU_CODE_SIGNING;
                else if (asn1_oid_eq(&purpose, OID_EKU_EMAIL_PROTECTION, sizeof(OID_EKU_EMAIL_PROTECTION)))
                    eku |= X509_EKU_EMAIL_PROTECTION;
            }
            out->has_ext_key_usage = true;
            out->ext_key_usage     = eku;
        }
        // другие extensions (CRL, AKI/SKI, ...) — игнорируем
    }
    return true;
}

bool x509_parse(const uint8_t* der, size_t der_len, x509_cert_t* out) {
    // Очищаем поля
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t*)out)[i] = 0;

    asn1_t outer;
    if (!asn1_parse(der, der_len, &outer))        return false;
    if (outer.tag != ASN1_TAG_SEQUENCE)           return false;

    const uint8_t* p = outer.val;

    // ---- TBSCertificate ----
    const uint8_t* tbs_start = p;
    asn1_t tbs;
    if (!asn1_parse_at(&p, outer.end, &tbs))      return false;
    if (tbs.tag != ASN1_TAG_SEQUENCE)             return false;
    out->tbs     = tbs_start;
    out->tbs_len = (size_t)(tbs.end - tbs_start);

    // ---- signatureAlgorithm ----
    asn1_t outer_sig_alg;
    if (!asn1_parse_at(&p, outer.end, &outer_sig_alg)) return false;
    if (outer_sig_alg.tag != ASN1_TAG_SEQUENCE)        return false;
    {
        const uint8_t* q = outer_sig_alg.val;
        asn1_t oid;
        if (!asn1_parse_at(&q, outer_sig_alg.end, &oid)) return false;
        out->sig_alg = map_sig_oid(&oid);
    }

    // ---- signatureValue ----
    asn1_t sig;
    if (!asn1_parse_at(&p, outer.end, &sig))     return false;
    if (sig.tag != ASN1_TAG_BITSTRING)           return false;
    if (sig.len < 1 || sig.val[0] != 0x00)       return false;
    out->signature     = sig.val + 1;
    out->signature_len = sig.len - 1;

    // ---- Дешифруем содержимое TBS ----
    const uint8_t* tp = tbs.val;
    // version [0] EXPLICIT INTEGER OPTIONAL (default v1=0)
    asn1_t maybe_ver;
    if (!asn1_parse_at(&tp, tbs.end, &maybe_ver)) return false;
    if (maybe_ver.tag == ASN1_TAG_CTX0_CONST) {
        // skip [0] wrapper, parse INTEGER inside
        // continue to next field
        if (!asn1_parse_at(&tp, tbs.end, &maybe_ver)) return false;
    }
    // maybe_ver сейчас = serialNumber (INTEGER)
    if (maybe_ver.tag != ASN1_TAG_INTEGER)            return false;

    // signature (inner) — должен совпасть с outer signatureAlgorithm
    asn1_t inner_sig_alg;
    if (!asn1_parse_at(&tp, tbs.end, &inner_sig_alg)) return false;
    if (inner_sig_alg.tag != ASN1_TAG_SEQUENCE)       return false;

    // issuer
    asn1_t issuer;
    const uint8_t* issuer_start = tp;
    if (!asn1_parse_at(&tp, tbs.end, &issuer))        return false;
    out->issuer_dn     = issuer_start;
    out->issuer_dn_len = (size_t)(issuer.end - issuer_start);

    // validity
    asn1_t validity;
    if (!asn1_parse_at(&tp, tbs.end, &validity))      return false;
    if (validity.tag != ASN1_TAG_SEQUENCE)            return false;
    {
        const uint8_t* q = validity.val;
        asn1_t nb, na;
        if (!asn1_parse_at(&q, validity.end, &nb))    return false;
        if (!asn1_parse_at(&q, validity.end, &na))    return false;
        if (!asn1_time_to_unix(&nb, &out->not_before)) return false;
        if (!asn1_time_to_unix(&na, &out->not_after))  return false;
    }

    // subject
    asn1_t subject;
    const uint8_t* subject_start = tp;
    if (!asn1_parse_at(&tp, tbs.end, &subject))       return false;
    out->subject_dn     = subject_start;
    out->subject_dn_len = (size_t)(subject.end - subject_start);

    // subjectPublicKeyInfo
    asn1_t spki;
    const uint8_t* spki_start = tp;
    if (!asn1_parse_at(&tp, tbs.end, &spki))          return false;
    size_t spki_total = (size_t)(spki.end - spki_start);
    if (!parse_spki(spki_start, spki_total, out))     return false;

    // extensions (optional, [3] EXPLICIT)
    while (tp < tbs.end) {
        asn1_t fld;
        const uint8_t* fld_start = tp;
        if (!asn1_parse_at(&tp, tbs.end, &fld))       return false;
        if (fld.tag == ASN1_TAG_CTX3_CONST) {
            size_t total = (size_t)(fld.end - fld_start);
            (void)total;
            // inside [3] EXPLICIT — SEQUENCE OF Extension
            if (!parse_extensions(fld.val, fld.len, out)) return false;
            break;
        }
        // unique IDs [1]/[2] — игнорируем
    }

    return true;
}

// ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER }
// X.509 заворачивает (r, s) в SEQUENCE внутри signature BIT STRING.  Распаковываем.
static bool extract_ecdsa_rs(const uint8_t* sig, size_t sig_len,
                             const uint8_t** r_bytes, size_t* r_len,
                             const uint8_t** s_bytes, size_t* s_len) {
    asn1_t seq;
    if (!asn1_parse(sig, sig_len, &seq))      return false;
    if (seq.tag != ASN1_TAG_SEQUENCE)         return false;
    const uint8_t* p = seq.val;
    asn1_t r, s;
    if (!asn1_parse_at(&p, seq.end, &r))      return false;
    if (r.tag != ASN1_TAG_INTEGER)            return false;
    if (!asn1_parse_at(&p, seq.end, &s))      return false;
    if (s.tag != ASN1_TAG_INTEGER)            return false;
    if (!asn1_int_as_bytes(&r, r_bytes, r_len)) return false;
    if (!asn1_int_as_bytes(&s, s_bytes, s_len)) return false;
    return true;
}

bool x509_verify_signature(const x509_cert_t* cert, const x509_cert_t* issuer) {
    if (cert->sig_alg == X509_SIG_UNKNOWN) return false;

    // 1. Compute hash of TBS.
    uint8_t hash[64];
    size_t  hash_len = 0;
    crypto_hash_id_t hid = CRYPTO_HASH_SHA256;
    switch (cert->sig_alg) {
        case X509_SIG_RSA_SHA1:
            crypto_sha1_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 20; hid = CRYPTO_HASH_SHA1; break;
        case X509_SIG_RSA_SHA256:
            crypto_sha256_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 32; hid = CRYPTO_HASH_SHA256; break;
        case X509_SIG_RSA_SHA384:
            crypto_sha384_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 48; hid = CRYPTO_HASH_SHA384; break;
        case X509_SIG_RSA_SHA512:
            crypto_sha512_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 64; hid = CRYPTO_HASH_SHA512; break;
        case X509_SIG_ECDSA_SHA256:
            crypto_sha256_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 32; break;
        case X509_SIG_ECDSA_SHA384:
            crypto_sha384_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 48; break;
        case X509_SIG_ECDSA_SHA512:
            crypto_sha512_buf(cert->tbs, cert->tbs_len, hash);
            hash_len = 64; break;
        case X509_SIG_ED25519:
            // Ed25519 хеширует сам внутри verify (RFC 8032).
            break;
        default: return false;
    }

    if (cert->sig_alg == X509_SIG_ED25519) {
        if (issuer->pk_type != X509_PK_ED25519) return false;
        return crypto_ed25519_verify(issuer->ed25519_pk,
                                     cert->tbs, cert->tbs_len,
                                     cert->signature);
    }
    // ECDSA paths (этап 47.6).
    if (cert->sig_alg == X509_SIG_ECDSA_SHA256 ||
        cert->sig_alg == X509_SIG_ECDSA_SHA384 ||
        cert->sig_alg == X509_SIG_ECDSA_SHA512) {
        if (issuer->pk_type != X509_PK_EC_P256 &&
            issuer->pk_type != X509_PK_EC_P384) return false;
        // Sig — это ASN.1 SEQUENCE { r INTEGER, s INTEGER }.
        const uint8_t* r_b; size_t r_l;
        const uint8_t* s_b; size_t s_l;
        if (!extract_ecdsa_rs(cert->signature, cert->signature_len,
                              &r_b, &r_l, &s_b, &s_l)) return false;
        // Кривая — та, на которой ключ издателя; хеш — из алгоритма подписи
        // (длиннее порядка кривой — берутся левые биты, это делает verify).
        if (issuer->pk_type == X509_PK_EC_P384)
            return crypto_p384_ecdsa_verify(issuer->ec_p384_pk,
                                            hash, hash_len,
                                            r_b, r_l, s_b, s_l);
        return crypto_p256_ecdsa_verify(issuer->ec_p256_pk,
                                        hash, hash_len,
                                        r_b, r_l, s_b, s_l);
    }
    // RSA paths (включая SHA-1):
    if (issuer->pk_type != X509_PK_RSA) return false;
    return crypto_rsa_pkcs1_verify(issuer->rsa_modulus, issuer->rsa_modulus_len,
                                   issuer->rsa_exponent,
                                   hid,
                                   hash, hash_len,
                                   cert->signature, cert->signature_len);
}

bool x509_issuer_matches(const x509_cert_t* cert, const x509_cert_t* issuer) {
    if (cert->issuer_dn_len != issuer->subject_dn_len) return false;
    for (size_t i = 0; i < cert->issuer_dn_len; i++) {
        if (cert->issuer_dn[i] != issuer->subject_dn[i]) return false;
    }
    return true;
}

bool x509_time_valid(const x509_cert_t* cert, uint64_t now) {
    return now >= cert->not_before && now <= cert->not_after;
}

// Простой hostname match с поддержкой leading wildcard ("*.example.com").
static bool dns_match(const char* pat, size_t plen, const char* host) {
    // Wildcard допустим только как первый компонент: "*.foo.bar".
    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        // Найти первую точку в host.
        size_t hi = 0;
        while (host[hi] && host[hi] != '.') hi++;
        if (host[hi] != '.') return false;
        // Сравним остаток pat[2..] с host[hi+1..].
        const char* tail = host + hi;  // ".rest"
        const char* p    = pat + 1;    // ".rest"
        size_t pl        = plen - 1;
        size_t i = 0;
        while (i < pl && tail[i]) {
            // case-insensitive
            char a = tail[i]; if (a >= 'A' && a <= 'Z') a += 32;
            char b = p[i];    if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
            i++;
        }
        return i == pl && tail[i] == 0;
    }
    // Exact compare, case-insensitive.
    size_t i = 0;
    while (i < plen && host[i]) {
        char a = pat[i];  if (a >= 'A' && a <= 'Z') a += 32;
        char b = host[i]; if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
        i++;
    }
    return i == plen && host[i] == 0;
}

bool x509_hostname_matches(const x509_cert_t* cert, const char* hostname) {
    if (!cert->has_san) return false;
    // san_value содержит SEQUENCE of GeneralName.
    asn1_t san;
    if (!asn1_parse(cert->san_value, cert->san_value_len, &san)) return false;
    if (san.tag != ASN1_TAG_SEQUENCE) return false;
    const uint8_t* p = san.val;
    while (p < san.end) {
        asn1_t gn;
        if (!asn1_parse_at(&p, san.end, &gn)) return false;
        // dNSName = [2] IA5String → tag = 0x82 (context-specific [2], primitive).
        if (gn.tag == 0x82) {
            if (dns_match((const char*)gn.val, gn.len, hostname)) return true;
        }
    }
    return false;
}

// ============================================================================
// (этап 47.6) — Полная strict chain validation.
// ============================================================================
//
// Алгоритм (RFC 5280 §6.1, упрощён):
//   1. Парсим все сертификаты; падаем при первой ошибке.
//   2. leaf = chain[0].  Проверяем:
//      * hostname в SAN (если задан).
//      * not_before <= now <= not_after (если now != 0).
//      * Если есть ExtendedKeyUsage — должен включать serverAuth.
//   3. Для каждого intermediate (i = 1..count-1):
//      * BasicConstraints.CA = TRUE.
//      * KeyUsage (если задан) — должен включать keyCertSign.
//      * Подписан issuer'ом = следующим в chain (если есть) или root'ом.
//      * pathLenConstraint: количество промежуточных СНИЗУ от меня (не считая
//        leaf и не считая меня) <= path_len_constraint.
//      * not_before <= now <= not_after.
//   4. Финальный cert chain (top-level) должен быть подписан root'ом из
//      trust store, чей Subject DN == Issuer DN top-level cert'а.
//
// Возвращаемая ошибка соответствует первому failed-условию.

const char* x509_chain_result_str(x509_chain_result_t r) {
    switch (r) {
        case X509_CHAIN_OK:                return "OK";
        case X509_CHAIN_ERR_NO_LEAF:       return "no leaf";
        case X509_CHAIN_ERR_BAD_HOSTNAME:  return "hostname mismatch";
        case X509_CHAIN_ERR_EXPIRED:       return "expired";
        case X509_CHAIN_ERR_NOT_YET_VALID: return "not yet valid";
        case X509_CHAIN_ERR_SIG_INVALID:   return "signature invalid";
        case X509_CHAIN_ERR_NO_TRUST:      return "no chain to trusted root";
        case X509_CHAIN_ERR_BAD_CA_EXT:    return "bad CA extension";
        case X509_CHAIN_ERR_PATH_TOO_LONG: return "path too long";
        case X509_CHAIN_ERR_BAD_EKU:       return "bad ExtKeyUsage";
        case X509_CHAIN_ERR_PARSE:         return "parse error";
        case X509_CHAIN_ERR_TOO_DEEP:      return "chain too deep";
    }
    return "unknown";
}

x509_chain_result_t x509_verify_chain(
    const uint8_t* const* chain_ders,
    const size_t*         chain_lens,
    size_t                chain_count,
    const char*           hostname,
    uint64_t              now_unix)
{
    if (chain_count == 0) return X509_CHAIN_ERR_NO_LEAF;
    if (chain_count > X509_MAX_CHAIN) return X509_CHAIN_ERR_TOO_DEEP;

    // 1. Парсим все. Обязан разобраться только лист; сертификат выше по
    //    цепочке с неизвестным нам типом ключа — ошибка лишь тогда, когда без
    //    него не обойтись (серверы нередко шлют лишние сертификаты).
    x509_cert_t certs[X509_MAX_CHAIN];
    bool parsed[X509_MAX_CHAIN];
    for (size_t i = 0; i < chain_count; i++) {
        parsed[i] = x509_parse(chain_ders[i], chain_lens[i], &certs[i]);
    }
    if (!parsed[0]) return X509_CHAIN_ERR_PARSE;

    // 2. Leaf checks.
    const x509_cert_t* leaf = &certs[0];

    // hostname (если задан)
    if (hostname && hostname[0]) {
        if (!x509_hostname_matches(leaf, hostname)) {
            return X509_CHAIN_ERR_BAD_HOSTNAME;
        }
    }
    // time (если задан)
    if (now_unix) {
        if (now_unix < leaf->not_before) return X509_CHAIN_ERR_NOT_YET_VALID;
        if (now_unix > leaf->not_after)  return X509_CHAIN_ERR_EXPIRED;
    }
    // EKU: если присутствует, должен включать serverAuth (или anyExtendedKeyUsage,
    // но мы его не парсим — делаем strict).  RFC 5280: «If the extension is
    // present, then the certificate MUST only be used for one of the purposes
    // indicated.»
    if (leaf->has_ext_key_usage) {
        if (!(leaf->ext_key_usage & X509_EKU_SERVER_AUTH)) {
            return X509_CHAIN_ERR_BAD_EKU;
        }
    }

    // 3. Идём снизу вверх. На каждом шаге сначала спрашиваем хранилище: если
    //    издатель текущего сертификата — доверенный корень и подпись сходится,
    //    цепочка замкнулась, а всё, что сервер прислал выше (перекрёстные
    //    подписи на старые корни, сам корень), уже не важно. Иначе издателем
    //    должен быть следующий сертификат цепочки.
    for (size_t i = 0; i < chain_count; i++) {
        const x509_cert_t* sub = &certs[i];

        x509_cert_t root;
        if (crypto_ca_find_by_dn(sub->issuer_dn, sub->issuer_dn_len, &root) &&
            root.is_ca && root.has_basic_constraints &&
            (!now_unix || (now_unix >= root.not_before && now_unix <= root.not_after)) &&
            x509_verify_signature(sub, &root)) {
            return X509_CHAIN_OK;
        }

        if (i + 1 >= chain_count) return X509_CHAIN_ERR_NO_TRUST;
        if (!parsed[i + 1])       return X509_CHAIN_ERR_PARSE;
        const x509_cert_t* iss = &certs[i + 1];

        // Issuer DN match.
        if (!x509_issuer_matches(sub, iss)) {
            return X509_CHAIN_ERR_NO_TRUST;
        }
        // Issuer должен быть CA.
        if (!iss->is_ca || !iss->has_basic_constraints) {
            return X509_CHAIN_ERR_BAD_CA_EXT;
        }
        // Если у issuer'а есть KeyUsage, должно быть keyCertSign.
        if (iss->has_key_usage && !(iss->key_usage & X509_KU_KEY_CERT_SIGN)) {
            return X509_CHAIN_ERR_BAD_CA_EXT;
        }
        // pathLenConstraint: сколько промежуточных CA допустимо НИЖЕ издателя.
        // Издатель стоит на позиции i+1, лист — на 0, между ними i-1
        // промежуточных (лист в счёт не идёт).
        if (iss->path_len_constraint >= 0) {
            int below = (int)i - 1;
            if (below < 0) below = 0;
            if (below > iss->path_len_constraint) {
                return X509_CHAIN_ERR_PATH_TOO_LONG;
            }
        }
        // Time check.
        if (now_unix) {
            if (now_unix < iss->not_before) return X509_CHAIN_ERR_NOT_YET_VALID;
            if (now_unix > iss->not_after)  return X509_CHAIN_ERR_EXPIRED;
        }
        // Signature check: sub подписан issuer.
        if (!x509_verify_signature(sub, iss)) {
            return X509_CHAIN_ERR_SIG_INVALID;
        }
    }

    return X509_CHAIN_ERR_NO_TRUST;
}
