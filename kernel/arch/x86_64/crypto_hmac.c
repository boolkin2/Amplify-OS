// ============================================================================
// crypto_hmac.c — HMAC, HKDF, TLS 1.2 PRF (этап 47.1).
// ----------------------------------------------------------------------------
// RFC 2104 (HMAC), RFC 5869 (HKDF), RFC 5246 §5 (TLS 1.2 PRF).
//
// HMAC-SHA256, HMAC-SHA384, HMAC-SHA512.
// TLS 1.2 PRF используется для деривации master_secret, key_block, и
// finished/verify_data. Алгоритм:
//   PRF(secret, label, seed) = P_<hash>(secret, label || seed)
//   P_<hash>(secret, seed) = HMAC(secret, A(1) || seed) ||
//                            HMAC(secret, A(2) || seed) || ...
//   A(0) = seed, A(i) = HMAC(secret, A(i-1))
//
// Для GCM/SHA256 cipher-suites используется P_SHA256.
// (Для AES-256/GCM/SHA384 нужен P_SHA384 — добавим в crypto_tls12_prf_sha384
// если потребуется на 47.4; в большинстве серверов SHA256-suites хватает.)
// ============================================================================
#include "crypto.h"

// Минимальная замена memcpy/memset, чтобы не зависеть от libc.
static void mc(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void mz(void* d, size_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (size_t i = 0; i < n; i++) dd[i] = 0;
}

// ============================================================================
// HMAC-SHA256
// ============================================================================
void crypto_hmac_sha256_init(crypto_hmac_sha256_ctx_t* ctx,
                             const void* key, size_t keylen) {
    uint8_t k[64];
    mz(k, 64);

    // Если ключ длиннее 64 байт — заменяем на его SHA-256.
    if (keylen > 64) {
        crypto_sha256_buf(key, keylen, k);
        // hash длиной 32 байта — остальные 32 уже нули.
    } else {
        mc(k, key, keylen);
    }

    for (int i = 0; i < 64; i++) {
        ctx->k_ipad[i] = (uint8_t)(k[i] ^ 0x36);
        ctx->k_opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    crypto_wipe(k, sizeof(k));

    crypto_sha256_init(&ctx->inner);
    crypto_sha256_update(&ctx->inner, ctx->k_ipad, 64);
}

void crypto_hmac_sha256_update(crypto_hmac_sha256_ctx_t* ctx,
                               const void* data, size_t len) {
    crypto_sha256_update(&ctx->inner, data, len);
}

void crypto_hmac_sha256_final(crypto_hmac_sha256_ctx_t* ctx,
                              uint8_t out[CRYPTO_SHA256_LEN]) {
    uint8_t inner_hash[CRYPTO_SHA256_LEN];
    crypto_sha256_final(&ctx->inner, inner_hash);

    crypto_sha256_ctx_t outer;
    crypto_sha256_init(&outer);
    crypto_sha256_update(&outer, ctx->k_opad, 64);
    crypto_sha256_update(&outer, inner_hash, CRYPTO_SHA256_LEN);
    crypto_sha256_final(&outer, out);

    crypto_wipe(inner_hash, sizeof(inner_hash));
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_hmac_sha256_buf(const void* key, size_t keylen,
                            const void* data, size_t len,
                            uint8_t out[CRYPTO_SHA256_LEN]) {
    crypto_hmac_sha256_ctx_t ctx;
    crypto_hmac_sha256_init(&ctx, key, keylen);
    crypto_hmac_sha256_update(&ctx, data, len);
    crypto_hmac_sha256_final(&ctx, out);
}

// ============================================================================
// HMAC-SHA384 / HMAC-SHA512 (block size 128 байт)
// ============================================================================
static void hmac_sha512_init_common(crypto_hmac_sha512_ctx_t* ctx,
                                    const void* key, size_t keylen, int is_384) {
    uint8_t k[128];
    mz(k, 128);

    if (keylen > 128) {
        if (is_384) {
            uint8_t tmp[CRYPTO_SHA384_LEN];
            crypto_sha384_buf(key, keylen, tmp);
            mc(k, tmp, CRYPTO_SHA384_LEN);
            crypto_wipe(tmp, sizeof(tmp));
        } else {
            uint8_t tmp[CRYPTO_SHA512_LEN];
            crypto_sha512_buf(key, keylen, tmp);
            mc(k, tmp, CRYPTO_SHA512_LEN);
            crypto_wipe(tmp, sizeof(tmp));
        }
    } else {
        mc(k, key, keylen);
    }

    for (int i = 0; i < 128; i++) {
        ctx->k_ipad[i] = (uint8_t)(k[i] ^ 0x36);
        ctx->k_opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    crypto_wipe(k, sizeof(k));

    if (is_384) crypto_sha384_init(&ctx->inner);
    else        crypto_sha512_init(&ctx->inner);
    crypto_sha512_update(&ctx->inner, ctx->k_ipad, 128);
    ctx->is_384 = is_384;
}

void crypto_hmac_sha384_init(crypto_hmac_sha512_ctx_t* ctx,
                             const void* key, size_t keylen) {
    hmac_sha512_init_common(ctx, key, keylen, 1);
}

void crypto_hmac_sha512_init(crypto_hmac_sha512_ctx_t* ctx,
                             const void* key, size_t keylen) {
    hmac_sha512_init_common(ctx, key, keylen, 0);
}

void crypto_hmac_sha512_update(crypto_hmac_sha512_ctx_t* ctx,
                               const void* data, size_t len) {
    crypto_sha512_update(&ctx->inner, data, len);
}

void crypto_hmac_sha384_final(crypto_hmac_sha512_ctx_t* ctx,
                              uint8_t out[CRYPTO_SHA384_LEN]) {
    uint8_t inner_hash[CRYPTO_SHA384_LEN];
    crypto_sha384_final(&ctx->inner, inner_hash);

    crypto_sha512_ctx_t outer;
    crypto_sha384_init(&outer);
    crypto_sha512_update(&outer, ctx->k_opad, 128);
    crypto_sha512_update(&outer, inner_hash, CRYPTO_SHA384_LEN);
    crypto_sha384_final(&outer, out);

    crypto_wipe(inner_hash, sizeof(inner_hash));
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_hmac_sha512_final(crypto_hmac_sha512_ctx_t* ctx,
                              uint8_t out[CRYPTO_SHA512_LEN]) {
    uint8_t inner_hash[CRYPTO_SHA512_LEN];
    crypto_sha512_final(&ctx->inner, inner_hash);

    crypto_sha512_ctx_t outer;
    crypto_sha512_init(&outer);
    crypto_sha512_update(&outer, ctx->k_opad, 128);
    crypto_sha512_update(&outer, inner_hash, CRYPTO_SHA512_LEN);
    crypto_sha512_final(&outer, out);

    crypto_wipe(inner_hash, sizeof(inner_hash));
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_hmac_sha384_buf(const void* key, size_t keylen,
                            const void* data, size_t len,
                            uint8_t out[CRYPTO_SHA384_LEN]) {
    crypto_hmac_sha512_ctx_t ctx;
    crypto_hmac_sha384_init(&ctx, key, keylen);
    crypto_hmac_sha512_update(&ctx, data, len);
    crypto_hmac_sha384_final(&ctx, out);
}

// ============================================================================
// HKDF-SHA256 (RFC 5869)
// ============================================================================
void crypto_hkdf_sha256_extract(const void* salt, size_t salt_len,
                                const void* ikm,  size_t ikm_len,
                                uint8_t prk[CRYPTO_SHA256_LEN]) {
    // Если salt пустой — используем 32 нуля.
    uint8_t zero_salt[CRYPTO_SHA256_LEN];
    mz(zero_salt, CRYPTO_SHA256_LEN);
    const void* s = salt ? salt : zero_salt;
    size_t      slen = salt ? salt_len : CRYPTO_SHA256_LEN;
    crypto_hmac_sha256_buf(s, slen, ikm, ikm_len, prk);
}

void crypto_hkdf_sha256_expand(const uint8_t prk[CRYPTO_SHA256_LEN],
                               const void* info, size_t info_len,
                               uint8_t* out, size_t out_len) {
    uint8_t T[CRYPTO_SHA256_LEN];
    size_t  t_len = 0;
    size_t  done  = 0;
    uint8_t counter = 1;

    while (done < out_len) {
        crypto_hmac_sha256_ctx_t ctx;
        crypto_hmac_sha256_init(&ctx, prk, CRYPTO_SHA256_LEN);
        if (t_len > 0) crypto_hmac_sha256_update(&ctx, T, t_len);
        if (info_len > 0) crypto_hmac_sha256_update(&ctx, info, info_len);
        crypto_hmac_sha256_update(&ctx, &counter, 1);
        crypto_hmac_sha256_final(&ctx, T);
        t_len = CRYPTO_SHA256_LEN;

        size_t take = out_len - done;
        if (take > CRYPTO_SHA256_LEN) take = CRYPTO_SHA256_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = T[i];
        done += take;
        counter++;
    }
    crypto_wipe(T, sizeof(T));
}

// ============================================================================
// TLS 1.2 PRF (RFC 5246 §5)
// ============================================================================
// P_SHA256(secret, seed) = HMAC(secret, A(1)|seed) || HMAC(secret, A(2)|seed)|| ...
// PRF(secret, label, seed) = P_SHA256(secret, label || seed).
void crypto_tls12_prf_sha256(const void* secret, size_t secret_len,
                             const char* label,
                             const void* seed, size_t seed_len,
                             uint8_t* out, size_t out_len) {
    // Считаем длину label.
    size_t label_len = 0;
    while (label[label_len]) label_len++;

    // A(0) = label || seed. A(i) = HMAC(secret, A(i-1)).
    // A — это просто промежуточный вход HMAC'а размером SHA-256.
    uint8_t A[CRYPTO_SHA256_LEN];

    // A(1) = HMAC(secret, label || seed).
    {
        crypto_hmac_sha256_ctx_t ctx;
        crypto_hmac_sha256_init(&ctx, secret, secret_len);
        crypto_hmac_sha256_update(&ctx, label, label_len);
        crypto_hmac_sha256_update(&ctx, seed, seed_len);
        crypto_hmac_sha256_final(&ctx, A);
    }

    size_t done = 0;
    while (done < out_len) {
        // chunk = HMAC(secret, A || label || seed).
        uint8_t chunk[CRYPTO_SHA256_LEN];
        crypto_hmac_sha256_ctx_t ctx;
        crypto_hmac_sha256_init(&ctx, secret, secret_len);
        crypto_hmac_sha256_update(&ctx, A, CRYPTO_SHA256_LEN);
        crypto_hmac_sha256_update(&ctx, label, label_len);
        crypto_hmac_sha256_update(&ctx, seed, seed_len);
        crypto_hmac_sha256_final(&ctx, chunk);

        size_t take = out_len - done;
        if (take > CRYPTO_SHA256_LEN) take = CRYPTO_SHA256_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = chunk[i];
        done += take;

        // A(i+1) = HMAC(secret, A(i)).
        if (done < out_len) {
            crypto_hmac_sha256_buf(secret, secret_len, A, CRYPTO_SHA256_LEN, A);
        }
        crypto_wipe(chunk, sizeof(chunk));
    }
    crypto_wipe(A, sizeof(A));
}

// То же на SHA-384 — для наборов шифров *_SHA384 (AES-256-GCM): у них и PRF,
// и хеш рукопожатия считаются на SHA-384 (RFC 5289 §3.2).
void crypto_tls12_prf_sha384(const void* secret, size_t secret_len,
                             const char* label,
                             const void* seed, size_t seed_len,
                             uint8_t* out, size_t out_len) {
    size_t label_len = 0;
    while (label[label_len]) label_len++;

    uint8_t A[CRYPTO_SHA384_LEN];
    {
        crypto_hmac_sha512_ctx_t ctx;
        crypto_hmac_sha384_init(&ctx, secret, secret_len);
        crypto_hmac_sha512_update(&ctx, label, label_len);
        crypto_hmac_sha512_update(&ctx, seed, seed_len);
        crypto_hmac_sha384_final(&ctx, A);
    }

    size_t done = 0;
    while (done < out_len) {
        uint8_t chunk[CRYPTO_SHA384_LEN];
        crypto_hmac_sha512_ctx_t ctx;
        crypto_hmac_sha384_init(&ctx, secret, secret_len);
        crypto_hmac_sha512_update(&ctx, A, CRYPTO_SHA384_LEN);
        crypto_hmac_sha512_update(&ctx, label, label_len);
        crypto_hmac_sha512_update(&ctx, seed, seed_len);
        crypto_hmac_sha384_final(&ctx, chunk);

        size_t take = out_len - done;
        if (take > CRYPTO_SHA384_LEN) take = CRYPTO_SHA384_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = chunk[i];
        done += take;

        if (done < out_len) {
            uint8_t next[CRYPTO_SHA384_LEN];
            crypto_hmac_sha384_buf(secret, secret_len, A, CRYPTO_SHA384_LEN, next);
            for (size_t i = 0; i < CRYPTO_SHA384_LEN; i++) A[i] = next[i];
        }
        crypto_wipe(chunk, sizeof(chunk));
    }
    crypto_wipe(A, sizeof(A));
}

// ============================================================================
// [этап 48] PBKDF2-HMAC-SHA256 (RFC 2898 / RFC 8018 §5.2)
// ----------------------------------------------------------------------------
// Парольная деривация ключа для FDE (KEK из passphrase).
//   F(P, S, c, i) = U1 XOR U2 XOR ... XOR Uc
//   U1 = HMAC(P, S || INT_BE32(i)),  Uj = HMAC(P, U(j-1))
//   DK = F(..., 1) || F(..., 2) || ...
//
// KAT в crypto_self_test.c: стандартные векторы ("password","salt",c=1 и
// c=4096), кросс-проверены через `openssl kdf ... PBKDF2`.
// ============================================================================
//
// Оптимизация: состояния SHA-256 после поглощения K^ipad и K^opad не зависят
// от данных, поэтому считаются ОДИН раз (два 64-байтных блока), а каждая
// итерация копирует их и досчитывает по одному блоку на внутренний и внешний
// хеш.  Было 4 компрессии на итерацию, стало 2 — при фиксированном времени
// разблокировки FDE это вдвое больше итераций, т.е. вдвое дороже перебор.
void crypto_pbkdf2_hmac_sha256(const void* password, size_t password_len,
                               const void* salt, size_t salt_len,
                               uint32_t iters,
                               uint8_t* out, size_t out_len) {
    if (iters == 0) iters = 1;
    uint32_t block = 1;
    size_t   done  = 0;

    uint8_t U[CRYPTO_SHA256_LEN];
    uint8_t F[CRYPTO_SHA256_LEN];
    uint8_t inner[CRYPTO_SHA256_LEN];

    // Предвычисленные префиксы HMAC: H(K^ipad || ...) и H(K^opad || ...).
    crypto_hmac_sha256_ctx_t key;
    crypto_hmac_sha256_init(&key, password, password_len);
    crypto_sha256_ctx_t ipad_state = key.inner;
    crypto_sha256_ctx_t opad_state;
    crypto_sha256_init(&opad_state);
    crypto_sha256_update(&opad_state, key.k_opad, 64);
    crypto_wipe(&key, sizeof(key));

    while (done < out_len) {
        // U1 = HMAC(P, salt || INT_BE32(block))
        uint8_t be[4] = {
            (uint8_t)(block >> 24), (uint8_t)(block >> 16),
            (uint8_t)(block >> 8),  (uint8_t)(block),
        };
        crypto_sha256_ctx_t h = ipad_state;
        crypto_sha256_update(&h, salt, salt_len);
        crypto_sha256_update(&h, be, 4);
        crypto_sha256_final(&h, inner);
        h = opad_state;
        crypto_sha256_update(&h, inner, CRYPTO_SHA256_LEN);
        crypto_sha256_final(&h, U);
        for (size_t i = 0; i < CRYPTO_SHA256_LEN; i++) F[i] = U[i];

        // U2..Uc
        for (uint32_t j = 1; j < iters; j++) {
            h = ipad_state;
            crypto_sha256_update(&h, U, CRYPTO_SHA256_LEN);
            crypto_sha256_final(&h, inner);
            h = opad_state;
            crypto_sha256_update(&h, inner, CRYPTO_SHA256_LEN);
            crypto_sha256_final(&h, U);
            for (size_t i = 0; i < CRYPTO_SHA256_LEN; i++) F[i] ^= U[i];
        }

        size_t take = out_len - done;
        if (take > CRYPTO_SHA256_LEN) take = CRYPTO_SHA256_LEN;
        for (size_t i = 0; i < take; i++) out[done + i] = F[i];
        done += take;
        block++;
    }
    crypto_wipe(U, sizeof(U));
    crypto_wipe(F, sizeof(F));
    crypto_wipe(inner, sizeof(inner));
    crypto_wipe(&ipad_state, sizeof(ipad_state));
    crypto_wipe(&opad_state, sizeof(opad_state));
}
