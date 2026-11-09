// ============================================================================
// crypto.h — единый header криптографической подсистемы AMPLIFY-OS (этап 47.1).
// ----------------------------------------------------------------------------
// Этот header агрегирует ВЕСЬ публичный API крипто-стека:
//   * SHA-2 семейство   (crypto_sha.c)
//   * HMAC / HKDF / PRF (crypto_hmac.c)
//   * AES-128/256       (crypto_aes.c, software + AES-NI fast path)
//   * AES-CBC, AES-GCM  (crypto_aes.c)
//   * ChaCha20, Poly1305, AEAD ChaCha20-Poly1305 (crypto_chacha.c)
//   * PRNG (RDRAND/RDSEED + Fortuna-style entropy pool) (crypto_rand.c)
//   * CPU feature detection (crypto_cpu.c)
//   * Self-test (crypto_self_test.c)
//
// Конвенции:
//   * Большие числа (ключи, хеш-результаты, IV, tag) — массивы uint8_t в
//     big-endian (network byte order), как принято в стандартах TLS/IETF.
//     Исключение — внутренние слова SHA-256/512 в w[64]/state[8] (uint32_t/
//     uint64_t для удобства, big-endian подаются ТОЛЬКО на финальном выводе).
//   * Все контексты живут на стеке вызывающего. Heap не используется в
//     горячих путях TLS — это критично для kernel-mode (RIP-cooperative
//     allocation требует heap-locks).
//   * Все функции возвращают bool (true=OK) или void. Ошибки печатаются
//     через serial_puts(), как принято в остальном ядре.
//   * Чувствительные данные (private keys, traffic secrets) wipe'аются
//     через crypto_wipe() (volatile-указатель в цикле, чтобы compiler
//     не оптимизировал прочь memset(0)).
//
// Что НЕ В этом header'е (отложено на 47.2..47.5):
//   * Большие целые (bignum)    — crypto_bignum.h    (47.2)
//   * RSA-PKCS#1 v1.5            — crypto_rsa.h       (47.2)
//   * Curve25519, Ed25519        — crypto_curve.h     (47.2)
//   * ASN.1 DER parser           — crypto_asn1.h      (47.3)
//   * X.509 certificate          — crypto_x509.h      (47.3)
//   * CA bundle                  — crypto_ca.h        (47.3)
//   * TLS 1.2 client             — tls.h              (47.4)
//   * HTTPS                      — https.h            (47.5)
// ============================================================================
#ifndef AOS_X86_64_CRYPTO_H
#define AOS_X86_64_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- Утилиты ----------------------------------------------------------------
void crypto_wipe(void* p, size_t n);  // безопасная очистка памяти (volatile)
bool crypto_eq_const(const void* a, const void* b, size_t n);  // constant-time

// ---- CPU feature detection (crypto_cpu.c) -----------------------------------
typedef struct {
    bool aesni;       // CPUID.01H:ECX[25]  AES-NI
    bool pclmulqdq;   // CPUID.01H:ECX[1]   carry-less multiply (для GHASH)
    bool rdrand;      // CPUID.01H:ECX[30]  hardware RNG
    bool rdseed;      // CPUID.07H:EBX[18]  better hardware RNG
    bool sha_ext;     // CPUID.07H:EBX[29]  SHA extensions (Goldmont+, Zen)
} crypto_cpu_features_t;

const crypto_cpu_features_t* crypto_cpu(void);  // указатель на g_cpu (init'нутый)
void crypto_cpu_init(void);
void crypto_cpu_print(void);                    // вывод в serial для smoke

// ---- SHA-1 (crypto_sha.c, RFC 3174) -----------------------------------------
// SHA-1 нужен ТОЛЬКО для совместимости с legacy CA подписанными SHA-1
// (несколько Mozilla root CA до сих пор там, например DigiCert Global Root CA).
// Современные intermediate certs практически все на SHA-256, но при
// валидации chain'а можем встретить старый промежуточный.  ВНИМАНИЕ:
// SHA-1 collision-resistance сломан (SHAttered 2017) — в новой подписи
// никогда не используем; только verify legacy.
#define CRYPTO_SHA1_LEN    20

typedef struct {
    uint32_t state[5];
    uint64_t bytes_total;
    uint8_t  buf[64];
    size_t   buf_len;
} crypto_sha1_ctx_t;

void crypto_sha1_init  (crypto_sha1_ctx_t* ctx);
void crypto_sha1_update(crypto_sha1_ctx_t* ctx, const void* data, size_t len);
void crypto_sha1_final (crypto_sha1_ctx_t* ctx, uint8_t out[CRYPTO_SHA1_LEN]);
void crypto_sha1_buf   (const void* data, size_t len, uint8_t out[CRYPTO_SHA1_LEN]);

// ---- SHA-2 семейство (crypto_sha.c) -----------------------------------------
// FIPS 180-4: одинаковый интерфейс, разные digest_size.
//
// Размеры дайджестов:
#define CRYPTO_SHA256_LEN  32
#define CRYPTO_SHA384_LEN  48
#define CRYPTO_SHA512_LEN  64

typedef struct {
    uint32_t state[8];
    uint64_t bytes_total;          // общее число обработанных байтов
    uint8_t  buf[64];              // буфер для накопления неполного блока
    size_t   buf_len;
} crypto_sha256_ctx_t;

void crypto_sha256_init  (crypto_sha256_ctx_t* ctx);
void crypto_sha256_update(crypto_sha256_ctx_t* ctx, const void* data, size_t len);
void crypto_sha256_final (crypto_sha256_ctx_t* ctx, uint8_t out[CRYPTO_SHA256_LEN]);
void crypto_sha256_buf   (const void* data, size_t len,
                          uint8_t out[CRYPTO_SHA256_LEN]);   // one-shot

typedef struct {
    uint64_t state[8];
    uint64_t bytes_total_lo;
    uint64_t bytes_total_hi;       // 128-bit length по FIPS 180-4
    uint8_t  buf[128];
    size_t   buf_len;
} crypto_sha512_ctx_t;

void crypto_sha512_init  (crypto_sha512_ctx_t* ctx);
void crypto_sha384_init  (crypto_sha512_ctx_t* ctx);   // те же блоки, другой IV+truncation
void crypto_sha512_update(crypto_sha512_ctx_t* ctx, const void* data, size_t len);
void crypto_sha512_final (crypto_sha512_ctx_t* ctx, uint8_t out[CRYPTO_SHA512_LEN]);
void crypto_sha384_final (crypto_sha512_ctx_t* ctx, uint8_t out[CRYPTO_SHA384_LEN]);
void crypto_sha512_buf   (const void* data, size_t len,
                          uint8_t out[CRYPTO_SHA512_LEN]);
void crypto_sha384_buf   (const void* data, size_t len,
                          uint8_t out[CRYPTO_SHA384_LEN]);

// ---- HMAC / HKDF / TLS PRF (crypto_hmac.c) ----------------------------------
#define CRYPTO_HMAC_SHA256_MAX_KEY 128
#define CRYPTO_HMAC_SHA512_MAX_KEY 256   // для SHA-384/512 block = 128 bytes

typedef struct {
    uint8_t  k_ipad[64];
    uint8_t  k_opad[64];
    crypto_sha256_ctx_t inner;
} crypto_hmac_sha256_ctx_t;

void crypto_hmac_sha256_init  (crypto_hmac_sha256_ctx_t* ctx,
                               const void* key, size_t keylen);
void crypto_hmac_sha256_update(crypto_hmac_sha256_ctx_t* ctx,
                               const void* data, size_t len);
void crypto_hmac_sha256_final (crypto_hmac_sha256_ctx_t* ctx,
                               uint8_t out[CRYPTO_SHA256_LEN]);
void crypto_hmac_sha256_buf   (const void* key, size_t keylen,
                               const void* data, size_t len,
                               uint8_t out[CRYPTO_SHA256_LEN]);

typedef struct {
    uint8_t k_ipad[128];
    uint8_t k_opad[128];
    crypto_sha512_ctx_t inner;
    int     is_384;                // 0=512, 1=384
} crypto_hmac_sha512_ctx_t;

void crypto_hmac_sha384_init  (crypto_hmac_sha512_ctx_t* ctx,
                               const void* key, size_t keylen);
void crypto_hmac_sha512_init  (crypto_hmac_sha512_ctx_t* ctx,
                               const void* key, size_t keylen);
void crypto_hmac_sha512_update(crypto_hmac_sha512_ctx_t* ctx,
                               const void* data, size_t len);
void crypto_hmac_sha384_final (crypto_hmac_sha512_ctx_t* ctx,
                               uint8_t out[CRYPTO_SHA384_LEN]);
void crypto_hmac_sha512_final (crypto_hmac_sha512_ctx_t* ctx,
                               uint8_t out[CRYPTO_SHA512_LEN]);
void crypto_hmac_sha384_buf   (const void* key, size_t keylen,
                               const void* data, size_t len,
                               uint8_t out[CRYPTO_SHA384_LEN]);

// HKDF (RFC 5869) — Extract + Expand. Используется в TLS 1.3, но также
// удобен для собственных KDF в системе (key wrapping и т.д.).
void crypto_hkdf_sha256_extract(const void* salt, size_t salt_len,
                                const void* ikm,  size_t ikm_len,
                                uint8_t prk[CRYPTO_SHA256_LEN]);
void crypto_hkdf_sha256_expand (const uint8_t prk[CRYPTO_SHA256_LEN],
                                const void* info, size_t info_len,
                                uint8_t* out, size_t out_len);

// TLS 1.2 PRF (RFC 5246 §5). label — ASCII строка ("master secret",
// "key expansion", и т.д.). PRF(secret, label, seed) = P_<hash>(secret,
// label || seed). Для GCM/SHA256 cipher-suites используется P_SHA256.
void crypto_tls12_prf_sha256(const void* secret, size_t secret_len,
                             const char* label,
                             const void* seed, size_t seed_len,
                             uint8_t* out, size_t out_len);
// P_SHA384 — для наборов шифров *_SHA384 (AES-256-GCM).
void crypto_tls12_prf_sha384(const void* secret, size_t secret_len,
                             const char* label,
                             const void* seed, size_t seed_len,
                             uint8_t* out, size_t out_len);

// ---- AES (crypto_aes.c) -----------------------------------------------------
// Реализация двух-в-одном: software T-tables ИЛИ AES-NI (через CPUID).
// 240 байт expanded key для AES-256 (15 round-keys × 16), 176 для AES-128.
#define CRYPTO_AES_BLOCK 16

typedef struct {
    uint32_t rk[60];               // round keys (max — AES-256 = 60 words)
    int      rounds;               // 10 (AES-128) или 14 (AES-256)
    int      use_aesni;            // 0=software, 1=AES-NI fast path
} crypto_aes_ctx_t;

void crypto_aes128_init_enc(crypto_aes_ctx_t* ctx, const uint8_t key[16]);
void crypto_aes256_init_enc(crypto_aes_ctx_t* ctx, const uint8_t key[32]);
void crypto_aes128_init_dec(crypto_aes_ctx_t* ctx, const uint8_t key[16]);
void crypto_aes256_init_dec(crypto_aes_ctx_t* ctx, const uint8_t key[32]);

void crypto_aes_encrypt_block(const crypto_aes_ctx_t* ctx,
                              const uint8_t in[16], uint8_t out[16]);
void crypto_aes_decrypt_block(const crypto_aes_ctx_t* ctx,
                              const uint8_t in[16], uint8_t out[16]);

// AES-CBC: in/out длиной кратной 16. iv (16 байт) изменяется на финальный
// ciphertext-блок при encrypt'е (для chaining нескольких вызовов).
void crypto_aes_cbc_encrypt(const crypto_aes_ctx_t* ctx, uint8_t iv[16],
                            const uint8_t* in, uint8_t* out, size_t len);
void crypto_aes_cbc_decrypt(const crypto_aes_ctx_t* ctx, uint8_t iv[16],
                            const uint8_t* in, uint8_t* out, size_t len);

// AES-GCM (AEAD).
// nonce — 12 байт (рекомендуемая длина по NIST SP 800-38D и TLS 1.2).
// tag   — 16 байт (полный, не truncated).
// aad   — Additional Authenticated Data (необязательная).
// crypto_aes_gcm_encrypt: ciphertext.len = plaintext.len; tag вычисляется.
// crypto_aes_gcm_decrypt: проверяет tag, возвращает false при mismatch.
typedef struct {
    crypto_aes_ctx_t aes;
    uint8_t H[16];                 // E_K(0^128) — GHASH ключ
    int     use_pclmul;            // 1 = аппаратный PCLMULQDQ
} crypto_gcm_ctx_t;

void crypto_gcm_init    (crypto_gcm_ctx_t* gcm, const uint8_t* key, size_t keylen);
bool crypto_gcm_encrypt (const crypto_gcm_ctx_t* gcm,
                         const uint8_t nonce[12],
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* pt,  uint8_t* ct, size_t len,
                         uint8_t tag[16]);
bool crypto_gcm_decrypt (const crypto_gcm_ctx_t* gcm,
                         const uint8_t nonce[12],
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* ct,  uint8_t* pt, size_t len,
                         const uint8_t tag[16]);

// ---- AES-XTS (crypto_aes.c, этап 48 — FDE) ----------------------------------
// IEEE 1619-2007 / NIST SP 800-38E.  Режим для шифрования дисковых секторов:
// каждый сектор шифруется независимо, tweak = номер сектора (LE128), поэтому
// одинаковый plaintext в разных секторах даёт разный ciphertext, а random
// access к диску не требует chaining.  Ciphertext-stealing НЕ реализован —
// дисковые секторы всегда кратны 16 байтам (512/4096).
//
// XTS использует ДВА независимых ключа: key1 (data) + key2 (tweak).
// Для FDE применяется AES-256-XTS (суммарный ключ 64 байта).
typedef struct {
    crypto_aes_ctx_t data_enc;    // E_key1 (encrypt schedule)
    crypto_aes_ctx_t data_dec;    // D_key1 (decrypt schedule)
    crypto_aes_ctx_t tweak_enc;   // E_key2 (tweak всегда encrypt)
} crypto_xts_ctx_t;

void crypto_xts128_init(crypto_xts_ctx_t* ctx,
                        const uint8_t key1[16], const uint8_t key2[16]);
void crypto_xts256_init(crypto_xts_ctx_t* ctx,
                        const uint8_t key1[32], const uint8_t key2[32]);

// len должен быть кратен 16 (для секторов 512/4096 всегда так).
// sector — номер data unit (tweak, little-endian 128-bit).
void crypto_xts_encrypt(const crypto_xts_ctx_t* ctx, uint64_t sector,
                        const uint8_t* in, uint8_t* out, size_t len);
void crypto_xts_decrypt(const crypto_xts_ctx_t* ctx, uint64_t sector,
                        const uint8_t* in, uint8_t* out, size_t len);

// ---- PBKDF2-HMAC-SHA256 (crypto_hmac.c, RFC 2898 / RFC 8018, этап 48) -------
// KDF для парольной деривации KEK в FDE (LUKS-подобный header).
// iters >= 10000 рекомендуется; в AMPLIFY-OS FDE по умолчанию 20000.
void crypto_pbkdf2_hmac_sha256(const void* password, size_t password_len,
                               const void* salt, size_t salt_len,
                               uint32_t iters,
                               uint8_t* out, size_t out_len);

// ---- ChaCha20-Poly1305 (crypto_chacha.c) ------------------------------------
// RFC 8439. Один из основных cipher-suites TLS 1.2/1.3 для CPU без AES-NI.
//
// nonce — 12 байт (RFC 8439 §2.4).
// key   — 32 байта.
// tag   — 16 байт (Poly1305).
void crypto_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                             uint32_t counter,
                             const uint8_t* in, uint8_t* out, size_t len);

void crypto_poly1305_mac    (const uint8_t key[32],
                             const uint8_t* msg, size_t len,
                             uint8_t tag[16]);

bool crypto_chacha20_poly1305_seal(
        const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t* aad, size_t aad_len,
        const uint8_t* pt,  uint8_t* ct, size_t len,
        uint8_t tag[16]);
bool crypto_chacha20_poly1305_open(
        const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t* aad, size_t aad_len,
        const uint8_t* ct,  uint8_t* pt, size_t len,
        const uint8_t tag[16]);

// ---- PRNG (crypto_rand.c) ---------------------------------------------------
// Источник энтропии — RDRAND/RDSEED (если есть) + RDTSC + PIT ticks + IRQ count.
// Пул — ChaCha20 в счётчик-режиме с reseed-интервалом каждые 64 KB вывода.
//
// crypto_rand_init() ВЫЗЫВАЕТСЯ ОДИН РАЗ в kernel_main64 после timer_init.
// crypto_rand_bytes()  — основной API.
// crypto_rand_u32/u64  — удобные обёртки.
// crypto_rand_reseed() — принудительный rotation entropy.
void     crypto_rand_init  (void);
void     crypto_rand_bytes (void* out, size_t n);
uint32_t crypto_rand_u32   (void);
uint64_t crypto_rand_u64   (void);
void     crypto_rand_reseed(void);

// ============================================================================
// [подэтап 47.2] Public-key крипта.
// ============================================================================

// ---- Bignum (crypto_bn.c) ---------------------------------------------------
// Фиксированный 4096-битный bignum: 128 × 32-bit little-endian limbs.
// Достаточно для RSA-2048 и RSA-4096 verify (часто встречается у root CA,
// например ISRG Root X1 — RSA-4096).  Для X25519/Ed25519 используется
// отдельная 51-bit-limb field arithmetic (см. crypto_curve.c).
#define CRYPTO_BN_LIMBS  128   // 128 × 32 = 4096 bit
#define CRYPTO_BN_BYTES  (CRYPTO_BN_LIMBS * 4)

typedef struct {
    uint32_t v[CRYPTO_BN_LIMBS];   // v[0] = LSW
} crypto_bn_t;

void crypto_bn_zero          (crypto_bn_t* x);
void crypto_bn_copy          (crypto_bn_t* d, const crypto_bn_t* s);
int  crypto_bn_cmp           (const crypto_bn_t* a, const crypto_bn_t* b);  // -1/0/+1
bool crypto_bn_is_zero       (const crypto_bn_t* x);

void crypto_bn_from_bytes_be (crypto_bn_t* x, const uint8_t* in, size_t len);
void crypto_bn_to_bytes_be   (const crypto_bn_t* x, uint8_t* out, size_t len);

// out = a + b (mod 2^2048).  carry returned.
uint32_t crypto_bn_add(crypto_bn_t* out, const crypto_bn_t* a, const crypto_bn_t* b);
uint32_t crypto_bn_sub(crypto_bn_t* out, const crypto_bn_t* a, const crypto_bn_t* b);

// Modular exponentiation: out = base^exp mod n.
// Использует bit-by-bit reduction (медленно, но просто и достаточно для
// RSA-verify с e=65537: 17 mod_mul, ~50 мс на TCG).
void crypto_bn_modexp(crypto_bn_t* out, const crypto_bn_t* base,
                      uint32_t exp, const crypto_bn_t* n);

// ---- RSA-PKCS#1 v1.5 verify (crypto_rsa.c) ---------------------------------
// Алгоритмы хеша, поддерживаемые в DigestInfo (signature scheme).
// SHA-1 (этап 47.6) — только для legacy chain validation.
typedef enum {
    CRYPTO_HASH_SHA1   = 4,
    CRYPTO_HASH_SHA256 = 1,
    CRYPTO_HASH_SHA384 = 2,
    CRYPTO_HASH_SHA512 = 3,
} crypto_hash_id_t;

// n_bytes / n_len — RSA modulus (BE).  e — public exponent (обычно 65537).
// hash — digest сообщения; hash_len = размер digest'а.
// signature / sig_len — signature, длина = размер modulus в байтах (256 для RSA-2048).
// Возвращает true, если signature валидна для (hash, hash_id) под (n, e).
bool crypto_rsa_pkcs1_verify(const uint8_t* n_bytes, size_t n_len,
                             uint32_t e,
                             crypto_hash_id_t hash_id,
                             const uint8_t* hash, size_t hash_len,
                             const uint8_t* signature, size_t sig_len);

// ---- X25519 ECDH (crypto_curve.c, RFC 7748 §5) -----------------------------
// scalar — 32 байта (приватный ключ); u — 32 байта (public-key coordinate).
// Возвращает 32 байта shared secret в out.
void crypto_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);

// X25519 базовая точка u = 9.
extern const uint8_t crypto_x25519_basepoint[32];

// ---- Ed25519 verify (crypto_curve.c, RFC 8032 §5.1.7) ----------------------
// pub_key — 32 байта; signature — 64 байта (R || S); msg — произвольный.
bool crypto_ed25519_verify(const uint8_t pub_key[32],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t signature[64]);

// ---- NIST P-256 ECDH + ECDSA verify (crypto_p256.c, этап 47.6) -------------
// Curve secp256r1 / prime256v1 (NIST FIPS 186-4):
//   p = 2^256 - 2^224 + 2^192 + 2^96 - 1
//   y^2 = x^3 - 3x + b   over GF(p)
//
// Это ОСНОВНАЯ современная кривая для ECDHE-RSA TLS 1.2 cipher suites
// (0xC02F = TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, любимый Google/Cloudflare).
//
// Точки представлены в uncompressed форме (стандарт TLS / X.509):
//   65 байт: 0x04 || X(32) || Y(32)
// X и Y — большие числа в big-endian.
//
// Функции:
//   crypto_p256_validate_pub  — проверить, что точка на кривой.
//   crypto_p256_derive_pub    — public = scalar * G (используется при генерации
//                               клиентского ECDHE keypair'а).
//   crypto_p256_ecdh          — out = scalar * peer_pub.X (32-байтовый shared
//                               secret, X-coordinate в TLS RFC 8422).
//   crypto_p256_ecdsa_verify  — verify ECDSA-SHA256 сигнатуры (для X.509
//                               cert'ов с ECDSA-with-SHA256 algorithm).
#define CRYPTO_P256_SCALAR_LEN  32
#define CRYPTO_P256_POINT_LEN   65   // 0x04 || X(32) || Y(32)

bool crypto_p256_validate_pub(const uint8_t pub[65]);
bool crypto_p256_derive_pub  (const uint8_t scalar[32], uint8_t out_pub[65]);
bool crypto_p256_ecdh        (const uint8_t scalar[32], const uint8_t peer_pub[65],
                              uint8_t shared[32]);
bool crypto_p256_ecdsa_verify(const uint8_t pub[65],
                              const uint8_t* hash, size_t hash_len,
                              const uint8_t* r, size_t r_len,
                              const uint8_t* s, size_t s_len);

// ---- ECDSA P-384, только проверка (crypto_p384.c, подэтап 48.2) -------------
// Нужна для цепочек сертификатов: промежуточные и корневые CA с ключами
// P-384 (Let's Encrypt E5..E8, ISRG Root X2, GTS R4, GlobalSign ECC R5).
// pub — несжатая точка 0x04 || X(48) || Y(48); hash — SHA-256/384/512.
#define CRYPTO_P384_POINT_LEN   97
bool crypto_p384_validate_pub(const uint8_t pub[97]);
bool crypto_p384_ecdsa_verify(const uint8_t pub[97],
                              const uint8_t* hash, size_t hash_len,
                              const uint8_t* r, size_t r_len,
                              const uint8_t* s, size_t s_len);
// ECDH на P-384 (не за постоянное время — только для одноразовых ключей ECDHE).
// scalar — 48 байт BE из [1, n-1].
bool crypto_p384_derive_pub(const uint8_t scalar[48], uint8_t out_pub[97]);
bool crypto_p384_ecdh(const uint8_t scalar[48], const uint8_t peer_pub[97], uint8_t shared[48]);
bool crypto_p384_selftest(void);

// ---- Self-test (crypto_self_test.c) -----------------------------------------
// Возвращает 0 при успехе, иначе bitmask провалов (битами 0..N — какие
// тесты упали). Запускается из kernel_main64 на boot path.
typedef enum {
    CRYPTO_TEST_SHA256     = 1 << 0,
    CRYPTO_TEST_SHA384     = 1 << 1,
    CRYPTO_TEST_SHA512     = 1 << 2,
    CRYPTO_TEST_HMAC_SHA256= 1 << 3,
    CRYPTO_TEST_HMAC_SHA384= 1 << 4,
    CRYPTO_TEST_TLS_PRF    = 1 << 5,
    CRYPTO_TEST_HKDF       = 1 << 6,
    CRYPTO_TEST_AES128_ECB = 1 << 7,
    CRYPTO_TEST_AES256_ECB = 1 << 8,
    CRYPTO_TEST_AES_CBC    = 1 << 9,
    CRYPTO_TEST_AES_GCM    = 1 << 10,
    CRYPTO_TEST_CHACHA20   = 1 << 11,
    CRYPTO_TEST_POLY1305   = 1 << 12,
    CRYPTO_TEST_AEAD_CHA   = 1 << 13,
    CRYPTO_TEST_RAND       = 1 << 14,
    // 47.2:
    CRYPTO_TEST_BN_MODEXP  = 1 << 15,
    CRYPTO_TEST_RSA_VERIFY = 1 << 16,
    CRYPTO_TEST_X25519     = 1 << 17,
    CRYPTO_TEST_ED25519    = 1 << 18,
    // 47.3:
    CRYPTO_TEST_ASN1       = 1 << 19,
    CRYPTO_TEST_X509_PARSE = 1 << 20,
    CRYPTO_TEST_X509_VERIFY= 1 << 21,
    CRYPTO_TEST_CA_LOOKUP  = 1 << 22,
    // 47.6:
    CRYPTO_TEST_SHA1       = 1 << 23,
    CRYPTO_TEST_P256_ECDH  = 1 << 24,
    CRYPTO_TEST_P256_ECDSA = 1 << 25,
    // 48 (FDE):
    CRYPTO_TEST_PBKDF2     = 1 << 26,
    CRYPTO_TEST_AES_XTS    = 1 << 27,
    // 48.2:
    CRYPTO_TEST_P384_ECDSA = 1 << 28,
    CRYPTO_TEST_PRF_SHA384 = 1 << 29,
} crypto_test_bits_t;

uint32_t crypto_self_test_run(void);
void     crypto_self_test_print(uint32_t failed);

// ---- Главный init -----------------------------------------------------------
// Вызывается из kernel_main64. Делает: CPUID-detect, init RNG, прогоняет
// self_test, печатает результат. Возвращает true при полном PASS.
bool crypto_init(void);

#endif // AOS_X86_64_CRYPTO_H
