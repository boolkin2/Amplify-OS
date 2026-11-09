// ============================================================================
// crypto_sha.c — SHA-1 / SHA-256 / SHA-384 / SHA-512 (этап 47.1+47.6, FIPS 180-4 / RFC 3174).
// ----------------------------------------------------------------------------
// Это полная software-реализация всех четырёх алгоритмов с одним и тем же
// интерфейсом (init → update → final → один-в-один FIPS 180-4 §6.2 / §6.4
// и RFC 3174 для SHA-1).
//
// Почему все четыре?
//   * SHA-1   нужен ТОЛЬКО для verify legacy CA-цепочек (этап 47.6).
//             В новых подписях НЕ используется — collision-resistance сломан.
//   * SHA-256 нужен для HMAC в TLS PRF (RFC 5246 §5), AES-128/GCM/SHA256
//             cipher-suites, и signature verification SHA256WithRSAEncryption.
//   * SHA-384 нужен для AES-256/GCM/SHA384 cipher-suites — это любимый
//             suite Google/Cloudflare на современных серверах.
//   * SHA-512 нужен для Ed25519 verify (RFC 8032).
//
// SHA-384 — это в точности SHA-512 с другим IV и truncation первых 48
// байтов. Поэтому мы не пишем отдельный update — переиспользуем sha512.
//
// Реализация:
//   * Чистый C99, никаких SIMD-зависимостей (всё через uint32_t/uint64_t,
//     GCC сам векторизует на современных CPU).
//   * SHA-extensions (sha256rnds2 и т.д.) НЕ используем — на TCG они
//     эмулируются дороже, чем обычная реализация; на реальном железе
//     прирост ~5-10×, но мы оставим до 47.5 как micro-opt.
//   * Const-time: SHA-2 и так не имеет ветвлений по данным.
// ============================================================================
#include "crypto.h"

// ---- SHA-256 ---------------------------------------------------------------

// SHA-256 round constants (FIPS 180-4 §4.2.2): фракционные части кубических
// корней первых 64 простых чисел.
static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

#define SHA256_S0(x)   (rotr32(x, 2)  ^ rotr32(x, 13) ^ rotr32(x, 22))
#define SHA256_S1(x)   (rotr32(x, 6)  ^ rotr32(x, 11) ^ rotr32(x, 25))
#define SHA256_s0(x)   (rotr32(x, 7)  ^ rotr32(x, 18) ^ ((x) >> 3))
#define SHA256_s1(x)   (rotr32(x, 17) ^ rotr32(x, 19) ^ ((x) >> 10))
#define CH(x,y,z)      (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)     (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    // Big-endian word extraction.
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4*i  ] << 24) |
               ((uint32_t)block[4*i+1] << 16) |
               ((uint32_t)block[4*i+2] << 8)  |
               ((uint32_t)block[4*i+3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_s1(w[i-2]) + w[i-7] + SHA256_s0(w[i-15]) + w[i-16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t T1 = h + SHA256_S1(e) + CH(e, f, g) + K256[i] + w[i];
        uint32_t T2 = SHA256_S0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void crypto_sha256_init(crypto_sha256_ctx_t* ctx) {
    // FIPS 180-4 §5.3.3: IV — фракционные части квадратных корней первых 8 простых.
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bytes_total = 0;
    ctx->buf_len     = 0;
}

void crypto_sha256_update(crypto_sha256_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->bytes_total += len;

    // Дотянем неполный буфер до 64 байт.
    if (ctx->buf_len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p   += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    // Жмём прямо из вход'а — 64-байтные блоки без копирования.
    while (len >= 64) {
        sha256_compress(ctx->state, p);
        p   += 64;
        len -= 64;
    }

    // Остаток — в буфер.  ВАЖНО: buf_len здесь МОЖЕТ быть уже не нулём,
    // если в первом if (buf_len > 0) мы взяли take = len, не дотянув до 64.
    // В этом случае len теперь = 0 и ctx->buf_len = старое+take.  Поэтому
    // НЕ перезаписываем buf_len, а инкрементируем (но при len=0 это no-op).
    for (size_t i = 0; i < len; i++) ctx->buf[ctx->buf_len + i] = p[i];
    ctx->buf_len += len;
}

void crypto_sha256_final(crypto_sha256_ctx_t* ctx, uint8_t out[CRYPTO_SHA256_LEN]) {
    // Padding (FIPS 180-4 §5.1.1): 0x80, ноль'ов до 56 mod 64, 8 байт length.
    uint64_t bits = ctx->bytes_total * 8;
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        // Не помещается — flush'аем и начинаем новый блок с нулей.
        while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
        sha256_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;

    // Big-endian 64-bit length.
    for (int i = 7; i >= 0; i--) ctx->buf[ctx->buf_len++] = (uint8_t)(bits >> (i * 8));
    sha256_compress(ctx->state, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[4*i  ] = (uint8_t)(ctx->state[i] >> 24);
        out[4*i+1] = (uint8_t)(ctx->state[i] >> 16);
        out[4*i+2] = (uint8_t)(ctx->state[i] >> 8);
        out[4*i+3] = (uint8_t)(ctx->state[i]);
    }
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_sha256_buf(const void* data, size_t len, uint8_t out[CRYPTO_SHA256_LEN]) {
    crypto_sha256_ctx_t ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, data, len);
    crypto_sha256_final(&ctx, out);
}

// ---- SHA-512 ---------------------------------------------------------------

// SHA-512 round constants — фракционные части кубических корней первых 80 простых.
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL,
};

static inline uint64_t rotr64(uint64_t x, unsigned n) { return (x >> n) | (x << (64u - n)); }

#define SHA512_S0(x)   (rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39))
#define SHA512_S1(x)   (rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41))
#define SHA512_s0(x)   (rotr64(x, 1)  ^ rotr64(x, 8)  ^ ((x) >> 7))
#define SHA512_s1(x)   (rotr64(x, 19) ^ rotr64(x, 61) ^ ((x) >> 6))

static void sha512_compress(uint64_t state[8], const uint8_t block[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint64_t)block[8*i  ] << 56) | ((uint64_t)block[8*i+1] << 48) |
               ((uint64_t)block[8*i+2] << 40) | ((uint64_t)block[8*i+3] << 32) |
               ((uint64_t)block[8*i+4] << 24) | ((uint64_t)block[8*i+5] << 16) |
               ((uint64_t)block[8*i+6] <<  8) | ((uint64_t)block[8*i+7]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA512_s1(w[i-2]) + w[i-7] + SHA512_s0(w[i-15]) + w[i-16];
    }

    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t T1 = h + SHA512_S1(e) + CH(e, f, g) + K512[i] + w[i];
        uint64_t T2 = SHA512_S0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void crypto_sha512_init(crypto_sha512_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667f3bcc908ULL; ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL; ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL; ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL; ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->bytes_total_lo = ctx->bytes_total_hi = 0;
    ctx->buf_len        = 0;
}

void crypto_sha384_init(crypto_sha512_ctx_t* ctx) {
    // FIPS 180-4 §5.3.4: SHA-384 — это SHA-512 с другим IV (фракционные части
    // sqrt'ов 9-го..16-го простых) и truncation первых 48 байтов.
    ctx->state[0] = 0xcbbb9d5dc1059ed8ULL; ctx->state[1] = 0x629a292a367cd507ULL;
    ctx->state[2] = 0x9159015a3070dd17ULL; ctx->state[3] = 0x152fecd8f70e5939ULL;
    ctx->state[4] = 0x67332667ffc00b31ULL; ctx->state[5] = 0x8eb44a8768581511ULL;
    ctx->state[6] = 0xdb0c2e0d64f98fa7ULL; ctx->state[7] = 0x47b5481dbefa4fa4ULL;
    ctx->bytes_total_lo = ctx->bytes_total_hi = 0;
    ctx->buf_len        = 0;
}

void crypto_sha512_update(crypto_sha512_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    // 128-битный счётчик байтов.
    uint64_t prev_lo = ctx->bytes_total_lo;
    ctx->bytes_total_lo += len;
    if (ctx->bytes_total_lo < prev_lo) ctx->bytes_total_hi++;

    if (ctx->buf_len > 0) {
        size_t take = 128 - ctx->buf_len;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p   += take;
        len -= take;
        if (ctx->buf_len == 128) {
            sha512_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    while (len >= 128) {
        sha512_compress(ctx->state, p);
        p   += 128;
        len -= 128;
    }
    // Тот же fix, что и для SHA-256: после первого if buf_len уже может
    // содержать данные, не дотянутые до 128.  Не обнулять.
    for (size_t i = 0; i < len; i++) ctx->buf[ctx->buf_len + i] = p[i];
    ctx->buf_len += len;
}

static void sha512_finalize(crypto_sha512_ctx_t* ctx, uint8_t out[64]) {
    // bits = bytes_total * 8 (128-bit).
    uint64_t bits_lo = ctx->bytes_total_lo << 3;
    uint64_t bits_hi = (ctx->bytes_total_hi << 3) | (ctx->bytes_total_lo >> 61);

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < 128) ctx->buf[ctx->buf_len++] = 0;
        sha512_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 112) ctx->buf[ctx->buf_len++] = 0;

    // Big-endian 128-bit length.
    for (int i = 7; i >= 0; i--) ctx->buf[ctx->buf_len++] = (uint8_t)(bits_hi >> (i * 8));
    for (int i = 7; i >= 0; i--) ctx->buf[ctx->buf_len++] = (uint8_t)(bits_lo >> (i * 8));
    sha512_compress(ctx->state, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[8*i  ] = (uint8_t)(ctx->state[i] >> 56);
        out[8*i+1] = (uint8_t)(ctx->state[i] >> 48);
        out[8*i+2] = (uint8_t)(ctx->state[i] >> 40);
        out[8*i+3] = (uint8_t)(ctx->state[i] >> 32);
        out[8*i+4] = (uint8_t)(ctx->state[i] >> 24);
        out[8*i+5] = (uint8_t)(ctx->state[i] >> 16);
        out[8*i+6] = (uint8_t)(ctx->state[i] >> 8);
        out[8*i+7] = (uint8_t)(ctx->state[i]);
    }
}

void crypto_sha512_final(crypto_sha512_ctx_t* ctx, uint8_t out[CRYPTO_SHA512_LEN]) {
    sha512_finalize(ctx, out);
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_sha384_final(crypto_sha512_ctx_t* ctx, uint8_t out[CRYPTO_SHA384_LEN]) {
    uint8_t full[64];
    sha512_finalize(ctx, full);
    for (int i = 0; i < CRYPTO_SHA384_LEN; i++) out[i] = full[i];
    crypto_wipe(full, sizeof(full));
    crypto_wipe(ctx,  sizeof(*ctx));
}

void crypto_sha512_buf(const void* data, size_t len, uint8_t out[CRYPTO_SHA512_LEN]) {
    crypto_sha512_ctx_t ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, data, len);
    crypto_sha512_final(&ctx, out);
}

void crypto_sha384_buf(const void* data, size_t len, uint8_t out[CRYPTO_SHA384_LEN]) {
    crypto_sha512_ctx_t ctx;
    crypto_sha384_init(&ctx);
    crypto_sha512_update(&ctx, data, len);
    crypto_sha384_final(&ctx, out);
}

// ============================================================================
// SHA-1 (RFC 3174) — этап 47.6.
// ----------------------------------------------------------------------------
// Алгоритм точно тот же, что у SHA-256 (Merkle-Damgård + 64-байтные блоки),
// только 5 word'ов state, 80 раундов, четыре магических константы K, и три
// не-линейных функции f0..f3.  Простой, проверенный, маленький.
//
// ВНИМАНИЕ: используется ТОЛЬКО для verify legacy CA-сертификатов и для
// X.509-extensions hashing (subject_key_identifier).  В НОВЫХ подписях
// никогда не применяем — на коллизиях сломан с 2017 (SHAttered).
// ============================================================================

#define SHA1_F0(b,c,d) (((b) & (c)) | ((~(b)) & (d)))   // 0..19  Choice
#define SHA1_F1(b,c,d) ((b) ^ (c) ^ (d))                // 20..39 Parity
#define SHA1_F2(b,c,d) (((b) & (c)) | ((b) & (d)) | ((c) & (d)))  // 40..59 Majority
#define SHA1_F3(b,c,d) ((b) ^ (c) ^ (d))                // 60..79 Parity

static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }

static void sha1_compress(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4*i  ] << 24) |
               ((uint32_t)block[4*i+1] << 16) |
               ((uint32_t)block[4*i+2] << 8)  |
               ((uint32_t)block[4*i+3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 20; i++) {
        uint32_t t = rotl32(a, 5) + SHA1_F0(b, c, d) + e + 0x5A827999u + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    for (int i = 20; i < 40; i++) {
        uint32_t t = rotl32(a, 5) + SHA1_F1(b, c, d) + e + 0x6ED9EBA1u + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    for (int i = 40; i < 60; i++) {
        uint32_t t = rotl32(a, 5) + SHA1_F2(b, c, d) + e + 0x8F1BBCDCu + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    for (int i = 60; i < 80; i++) {
        uint32_t t = rotl32(a, 5) + SHA1_F3(b, c, d) + e + 0xCA62C1D6u + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void crypto_sha1_init(crypto_sha1_ctx_t* ctx) {
    // RFC 3174 §6.1: фиксированные H0..H4
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->bytes_total = 0;
    ctx->buf_len     = 0;
}

void crypto_sha1_update(crypto_sha1_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->bytes_total += len;

    if (ctx->buf_len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p   += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha1_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len >= 64) {
        sha1_compress(ctx->state, p);
        p   += 64;
        len -= 64;
    }
    for (size_t i = 0; i < len; i++) ctx->buf[ctx->buf_len + i] = p[i];
    ctx->buf_len += len;
}

void crypto_sha1_final(crypto_sha1_ctx_t* ctx, uint8_t out[CRYPTO_SHA1_LEN]) {
    uint64_t bits = ctx->bytes_total * 8;
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
        sha1_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;
    for (int i = 7; i >= 0; i--) ctx->buf[ctx->buf_len++] = (uint8_t)(bits >> (i * 8));
    sha1_compress(ctx->state, ctx->buf);

    for (int i = 0; i < 5; i++) {
        out[4*i  ] = (uint8_t)(ctx->state[i] >> 24);
        out[4*i+1] = (uint8_t)(ctx->state[i] >> 16);
        out[4*i+2] = (uint8_t)(ctx->state[i] >> 8);
        out[4*i+3] = (uint8_t)(ctx->state[i]);
    }
    crypto_wipe(ctx, sizeof(*ctx));
}

void crypto_sha1_buf(const void* data, size_t len, uint8_t out[CRYPTO_SHA1_LEN]) {
    crypto_sha1_ctx_t ctx;
    crypto_sha1_init(&ctx);
    crypto_sha1_update(&ctx, data, len);
    crypto_sha1_final(&ctx, out);
}
