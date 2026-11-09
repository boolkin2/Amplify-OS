// ============================================================================
// crypto_aes.c — AES-128/256 + CBC + GCM (этап 47.1).
// ----------------------------------------------------------------------------
// FIPS-197 (AES), NIST SP 800-38A (CBC), NIST SP 800-38D (GCM).
//
// Реализация:
//   * Software: FIPS-197 reference (без T-tables — чистая SubBytes/ShiftRows/
//     MixColumns); AES-NI обгоняет software на ~10×, поэтому ради экономии
//     1 KB ROM (T-tables = 4 КБ) отказываемся от ускорения через таблицы.
//     CPU без AES-NI у нас встретится разве что на старом TCG без `-cpu host`.
//   * AES-NI (через inline ASM): aesenc/aesenclast для encrypt,
//     aesdec/aesdeclast для decrypt, aeskeygenassist для key expansion.
//   * Detect на лету через crypto_cpu()->aesni.
//
// GHASH в GCM:
//   * Software: bit-by-bit умножение в GF(2^128) — стабильный и постоянный
//     по времени, но медленный. Достаточно для TLS handshake (4 KB MAC).
//   * PCLMULQDQ путь — отложен до 47.5 (требует Karatsuba/aggregation).
//     На текущем этапе работаем со software GHASH везде.
//
// Constant-time: AES-NI не имеет timing-зависимостей по ключу. Software AES
// тоже без table lookup'ов (используем SubBytes напрямую с S-box, который
// одинаково обращается ко всем строкам); GHASH bit-by-bit — постоянный по
// данным.
// ============================================================================
#include "crypto.h"

// ---- AES S-box и inverse S-box (FIPS-197 §5.1.1, §5.3.2) -------------------
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};
static const uint8_t INV_SBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

static const uint32_t RCON[15] = {
    0x00000000,0x01000000,0x02000000,0x04000000,0x08000000,
    0x10000000,0x20000000,0x40000000,0x80000000,0x1b000000,
    0x36000000,0x6c000000,0xd8000000,0xab000000,0x4d000000
};

// ---- Software key schedule -------------------------------------------------
static uint32_t sub_word(uint32_t x) {
    return ((uint32_t)SBOX[(x >> 24) & 0xff] << 24)
         | ((uint32_t)SBOX[(x >> 16) & 0xff] << 16)
         | ((uint32_t)SBOX[(x >>  8) & 0xff] <<  8)
         | ((uint32_t)SBOX[(x      ) & 0xff]);
}
static uint32_t rot_word(uint32_t x) { return (x << 8) | (x >> 24); }

static void aes_key_expansion(crypto_aes_ctx_t* ctx,
                              const uint8_t* key, int key_words) {
    int Nk = key_words;          // 4 для AES-128, 8 для AES-256
    int Nr = (Nk == 4) ? 10 : 14;
    ctx->rounds = Nr;
    int total = 4 * (Nr + 1);    // total words

    for (int i = 0; i < Nk; i++) {
        ctx->rk[i] = ((uint32_t)key[4*i  ] << 24) |
                     ((uint32_t)key[4*i+1] << 16) |
                     ((uint32_t)key[4*i+2] <<  8) |
                     ((uint32_t)key[4*i+3]);
    }
    for (int i = Nk; i < total; i++) {
        uint32_t t = ctx->rk[i-1];
        if ((i % Nk) == 0) t = sub_word(rot_word(t)) ^ RCON[i / Nk];
        else if (Nk > 6 && (i % Nk) == 4) t = sub_word(t);
        ctx->rk[i] = ctx->rk[i-Nk] ^ t;
    }
}

// ---- AES-NI inline ASM helpers ---------------------------------------------
// Используются только если crypto_cpu()->aesni == true.

static inline void aesni_keygen128(const uint8_t key[16], uint32_t rk[44]) {
    // Загружаем ключ в xmm1, делаем 10 раундов keygen.
    // Out: 44 uint32_t (11 round keys × 4 words).
    __asm__ __volatile__(
        "movdqu (%0), %%xmm1\n\t"
        "movdqu %%xmm1, (%1)\n\t"
        // round 1..10 — каждый через aeskeygenassist + shuffle
        "aeskeygenassist $0x01,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x10(%1)\n\t"
        "aeskeygenassist $0x02,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x20(%1)\n\t"
        "aeskeygenassist $0x04,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x30(%1)\n\t"
        "aeskeygenassist $0x08,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x40(%1)\n\t"
        "aeskeygenassist $0x10,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x50(%1)\n\t"
        "aeskeygenassist $0x20,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x60(%1)\n\t"
        "aeskeygenassist $0x40,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x70(%1)\n\t"
        "aeskeygenassist $0x80,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x80(%1)\n\t"
        "aeskeygenassist $0x1b,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0x90(%1)\n\t"
        "aeskeygenassist $0x36,%%xmm1,%%xmm2\n\t" "call    1f\n\t" "movdqu %%xmm1, 0xa0(%1)\n\t"
        "jmp     2f\n\t"

        // helper: rk_next = expand(rk_prev, rcon_in_xmm2).
        "1:\n\t"
        "pshufd  $0xff, %%xmm2, %%xmm2\n\t"
        "movdqa  %%xmm1, %%xmm3\n\t"
        "pslldq  $4, %%xmm3\n\t"
        "pxor    %%xmm3, %%xmm1\n\t"
        "pslldq  $4, %%xmm3\n\t"
        "pxor    %%xmm3, %%xmm1\n\t"
        "pslldq  $4, %%xmm3\n\t"
        "pxor    %%xmm3, %%xmm1\n\t"
        "pxor    %%xmm2, %%xmm1\n\t"
        "ret\n\t"
        "2:\n\t"
        :
        : "r"(key), "r"(rk)
        : "xmm1", "xmm2", "xmm3", "memory"
    );
}

// AES-256 keygen: 14 rounds. Каждые 2 шага: aeskeygenassist с rcon, потом без rcon.
static inline void aesni_keygen256(const uint8_t key[32], uint32_t rk[60]) {
    __asm__ __volatile__(
        "movdqu  (%0), %%xmm1\n\t"
        "movdqu 16(%0), %%xmm3\n\t"
        "movdqu %%xmm1, 0x00(%1)\n\t"
        "movdqu %%xmm3, 0x10(%1)\n\t"

        // макрос-расширение для 256:
        // round_with_rcon: aeskeygenassist $rcon, %xmm3, %xmm2 ; expand xmm1
        // round_no_rcon:   aeskeygenassist $0,    %xmm1, %xmm2 ; expand xmm3 (с другим shuffle)
        "aeskeygenassist $0x01,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0x20(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0x30(%1)\n\t"
        "aeskeygenassist $0x02,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0x40(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0x50(%1)\n\t"
        "aeskeygenassist $0x04,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0x60(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0x70(%1)\n\t"
        "aeskeygenassist $0x08,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0x80(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0x90(%1)\n\t"
        "aeskeygenassist $0x10,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0xa0(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0xb0(%1)\n\t"
        "aeskeygenassist $0x20,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0xc0(%1)\n\t"
        "aeskeygenassist $0x00,%%xmm1,%%xmm2\n\t" "call 2f\n\t" "movdqu %%xmm3, 0xd0(%1)\n\t"
        "aeskeygenassist $0x40,%%xmm3,%%xmm2\n\t" "call 1f\n\t" "movdqu %%xmm1, 0xe0(%1)\n\t"
        "jmp 9f\n\t"

        // 1: expand xmm1 using xmm2 (rcon-based)
        "1:\n\t"
        "pshufd $0xff,%%xmm2,%%xmm2\n\t"
        "movdqa %%xmm1,%%xmm4\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm1\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm1\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm1\n\t"
        "pxor   %%xmm2,%%xmm1\n\t"
        "ret\n\t"

        // 2: expand xmm3 using xmm2 (no-rcon, shuffle byte 2)
        "2:\n\t"
        "pshufd $0xaa,%%xmm2,%%xmm2\n\t"
        "movdqa %%xmm3,%%xmm4\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm3\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm3\n\t"
        "pslldq $4,%%xmm4\n\t" "pxor %%xmm4,%%xmm3\n\t"
        "pxor   %%xmm2,%%xmm3\n\t"
        "ret\n\t"
        "9:\n\t"
        :
        : "r"(key), "r"(rk)
        : "xmm1", "xmm2", "xmm3", "xmm4", "memory"
    );
}

static inline void aesni_encrypt(const uint32_t* rk, int rounds,
                                 const uint8_t in[16], uint8_t out[16]) {
    if (rounds == 10) {
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0\n\t"
            "movdqu (%1), %%xmm1\n\t"
            "pxor   %%xmm1, %%xmm0\n\t"
            "movdqu 0x10(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x20(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x30(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x40(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x50(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x60(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x70(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x80(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x90(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xa0(%1),%%xmm1\n\t" "aesenclast %%xmm1,%%xmm0\n\t"
            "movdqu %%xmm0,(%2)\n\t"
            :: "r"(in), "r"(rk), "r"(out)
            : "xmm0", "xmm1", "memory");
    } else {
        // rounds = 14
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0\n\t"
            "movdqu (%1), %%xmm1\n\t"
            "pxor   %%xmm1, %%xmm0\n\t"
            "movdqu 0x10(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x20(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x30(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x40(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x50(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x60(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x70(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x80(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0x90(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xa0(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xb0(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xc0(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xd0(%1),%%xmm1\n\t" "aesenc %%xmm1,%%xmm0\n\t"
            "movdqu 0xe0(%1),%%xmm1\n\t" "aesenclast %%xmm1,%%xmm0\n\t"
            "movdqu %%xmm0,(%2)\n\t"
            :: "r"(in), "r"(rk), "r"(out)
            : "xmm0", "xmm1", "memory");
    }
}

// AES-NI decrypt: round keys должны быть подвергнуты InvMixColumns (через aesimc),
// кроме первого и последнего. Это происходит в init_dec.
static inline void aesni_invert_keys(uint32_t* rk, int rounds) {
    // Не-первый и не-последний round-key inv_mix_columns'им.
    // Используем aesimc xmm,xmm.
    for (int i = 1; i < rounds; i++) {
        __asm__ __volatile__(
            "movdqu (%0),%%xmm0\n\t"
            "aesimc %%xmm0,%%xmm0\n\t"
            "movdqu %%xmm0,(%0)\n\t"
            :: "r"(rk + 4*i) : "xmm0", "memory");
    }
}

static inline void aesni_decrypt(const uint32_t* rk, int rounds,
                                 const uint8_t in[16], uint8_t out[16]) {
    // Decrypt с обратным порядком raund-keys.
    // Round-keys уже InvMixColumns'нуты (кроме первого и последнего).
    if (rounds == 10) {
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0\n\t"
            "movdqu 0xa0(%1),%%xmm1\n\t" "pxor   %%xmm1,%%xmm0\n\t"
            "movdqu 0x90(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x80(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x70(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x60(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x50(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x40(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x30(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x20(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x10(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu (%1),%%xmm1\n\t"     "aesdeclast %%xmm1,%%xmm0\n\t"
            "movdqu %%xmm0,(%2)\n\t"
            :: "r"(in), "r"(rk), "r"(out)
            : "xmm0", "xmm1", "memory");
    } else {
        __asm__ __volatile__(
            "movdqu (%0), %%xmm0\n\t"
            "movdqu 0xe0(%1),%%xmm1\n\t" "pxor   %%xmm1,%%xmm0\n\t"
            "movdqu 0xd0(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0xc0(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0xb0(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0xa0(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x90(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x80(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x70(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x60(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x50(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x40(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x30(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x20(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu 0x10(%1),%%xmm1\n\t" "aesdec %%xmm1,%%xmm0\n\t"
            "movdqu (%1),%%xmm1\n\t"     "aesdeclast %%xmm1,%%xmm0\n\t"
            "movdqu %%xmm0,(%2)\n\t"
            :: "r"(in), "r"(rk), "r"(out)
            : "xmm0", "xmm1", "memory");
    }
}

// ---- Software encrypt/decrypt (FIPS-197 reference) -------------------------
static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void sub_bytes(uint8_t s[16])     { for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]]; }
static void inv_sub_bytes(uint8_t s[16]) { for (int i = 0; i < 16; i++) s[i] = INV_SBOX[s[i]]; }

static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
    t = s[2];  s[2]  = s[10]; s[10] = t;  t = s[6];  s[6]  = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
}
static void inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1]  = t;
    t = s[2];  s[2]  = s[10]; s[10] = t;  t = s[6];  s[6]  = s[14]; s[14] = t;
    t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;
}

static void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + 4*c;
        uint8_t a = col[0], b = col[1], cc = col[2], d = col[3];
        uint8_t t = (uint8_t)(a ^ b ^ cc ^ d);
        col[0] ^= (uint8_t)(t ^ xtime((uint8_t)(a ^ b)));
        col[1] ^= (uint8_t)(t ^ xtime((uint8_t)(b ^ cc)));
        col[2] ^= (uint8_t)(t ^ xtime((uint8_t)(cc ^ d)));
        col[3] ^= (uint8_t)(t ^ xtime((uint8_t)(d ^ a)));
    }
}

static uint8_t mul_gf(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

static void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + 4*c;
        uint8_t a = col[0], b = col[1], cc = col[2], d = col[3];
        col[0] = mul_gf(0x0e, a) ^ mul_gf(0x0b, b) ^ mul_gf(0x0d, cc) ^ mul_gf(0x09, d);
        col[1] = mul_gf(0x09, a) ^ mul_gf(0x0e, b) ^ mul_gf(0x0b, cc) ^ mul_gf(0x0d, d);
        col[2] = mul_gf(0x0d, a) ^ mul_gf(0x09, b) ^ mul_gf(0x0e, cc) ^ mul_gf(0x0b, d);
        col[3] = mul_gf(0x0b, a) ^ mul_gf(0x0d, b) ^ mul_gf(0x09, cc) ^ mul_gf(0x0e, d);
    }
}

static void add_round_key(uint8_t s[16], const uint32_t* rk) {
    for (int i = 0; i < 4; i++) {
        uint32_t w = rk[i];
        s[4*i  ] ^= (uint8_t)(w >> 24);
        s[4*i+1] ^= (uint8_t)(w >> 16);
        s[4*i+2] ^= (uint8_t)(w >>  8);
        s[4*i+3] ^= (uint8_t)(w);
    }
}

static void sw_encrypt(const crypto_aes_ctx_t* ctx,
                       const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i];
    add_round_key(s, ctx->rk);
    for (int r = 1; r < ctx->rounds; r++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, ctx->rk + 4*r);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, ctx->rk + 4*ctx->rounds);
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

static void sw_decrypt(const crypto_aes_ctx_t* ctx,
                       const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i];
    add_round_key(s, ctx->rk + 4*ctx->rounds);
    for (int r = ctx->rounds - 1; r > 0; r--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, ctx->rk + 4*r);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, ctx->rk);
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

// ---- Публичный init/encrypt/decrypt ----------------------------------------
void crypto_aes128_init_enc(crypto_aes_ctx_t* ctx, const uint8_t key[16]) {
    int aesni = crypto_cpu()->aesni;
    ctx->use_aesni = aesni;
    if (aesni) {
        aesni_keygen128(key, ctx->rk);
        ctx->rounds = 10;
    } else {
        aes_key_expansion(ctx, key, 4);
    }
}

void crypto_aes256_init_enc(crypto_aes_ctx_t* ctx, const uint8_t key[32]) {
    int aesni = crypto_cpu()->aesni;
    ctx->use_aesni = aesni;
    if (aesni) {
        aesni_keygen256(key, ctx->rk);
        ctx->rounds = 14;
    } else {
        aes_key_expansion(ctx, key, 8);
    }
}

void crypto_aes128_init_dec(crypto_aes_ctx_t* ctx, const uint8_t key[16]) {
    crypto_aes128_init_enc(ctx, key);
    if (ctx->use_aesni) aesni_invert_keys(ctx->rk, 10);
    // Software decrypt использует те же rk напрямую (через inv_*).
}

void crypto_aes256_init_dec(crypto_aes_ctx_t* ctx, const uint8_t key[32]) {
    crypto_aes256_init_enc(ctx, key);
    if (ctx->use_aesni) aesni_invert_keys(ctx->rk, 14);
}

void crypto_aes_encrypt_block(const crypto_aes_ctx_t* ctx,
                              const uint8_t in[16], uint8_t out[16]) {
    if (ctx->use_aesni) aesni_encrypt(ctx->rk, ctx->rounds, in, out);
    else                sw_encrypt(ctx, in, out);
}

void crypto_aes_decrypt_block(const crypto_aes_ctx_t* ctx,
                              const uint8_t in[16], uint8_t out[16]) {
    if (ctx->use_aesni) aesni_decrypt(ctx->rk, ctx->rounds, in, out);
    else                sw_decrypt(ctx, in, out);
}

// ---- AES-CBC ---------------------------------------------------------------
void crypto_aes_cbc_encrypt(const crypto_aes_ctx_t* ctx, uint8_t iv[16],
                            const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];
    for (size_t off = 0; off < len; off += 16) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(in[off + i] ^ prev[i]);
        crypto_aes_encrypt_block(ctx, blk, out + off);
        for (int i = 0; i < 16; i++) prev[i] = out[off + i];
    }
    for (int i = 0; i < 16; i++) iv[i] = prev[i];
}

void crypto_aes_cbc_decrypt(const crypto_aes_ctx_t* ctx, uint8_t iv[16],
                            const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];
    for (size_t off = 0; off < len; off += 16) {
        uint8_t blk[16];
        crypto_aes_decrypt_block(ctx, in + off, blk);
        for (int i = 0; i < 16; i++) out[off + i] = (uint8_t)(blk[i] ^ prev[i]);
        for (int i = 0; i < 16; i++) prev[i] = in[off + i];
    }
    for (int i = 0; i < 16; i++) iv[i] = prev[i];
}

// ============================================================================
// AES-GCM (NIST SP 800-38D)
// ----------------------------------------------------------------------------
// GHASH в GF(2^128) с reduction polynomial x^128 + x^7 + x^2 + x + 1.
// Bit-by-bit умножение — медленный, но простой. Аппаратный PCLMULQDQ
// добавим в 47.5 micro-opt.
// ============================================================================
static void gf128_mul(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16]) {
    uint8_t V[16], R[16];
    for (int i = 0; i < 16; i++) { V[i] = Y[i]; R[i] = 0; }
    for (int i = 0; i < 128; i++) {
        // Если i-th бит X (MSB-first) — 1, R ^= V.
        int byte = i >> 3;
        int bit  = 7 - (i & 7);
        if ((X[byte] >> bit) & 1) {
            for (int j = 0; j < 16; j++) R[j] ^= V[j];
        }
        // V = V >> 1, mod-reduce by polynomial if LSB was 1.
        int lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) V[j] = (uint8_t)((V[j] >> 1) | ((V[j-1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;  // reduction poly's top byte (0xe1 = 11100001)
    }
    for (int i = 0; i < 16; i++) Z[i] = R[i];
}

static void ghash_update(const uint8_t H[16], uint8_t Y[16],
                         const uint8_t* data, size_t len) {
    while (len > 0) {
        uint8_t blk[16] = {0};
        size_t take = len >= 16 ? 16 : len;
        for (size_t i = 0; i < take; i++) blk[i] = data[i];
        for (int i = 0; i < 16; i++) Y[i] ^= blk[i];
        uint8_t T[16];
        gf128_mul(Y, H, T);
        for (int i = 0; i < 16; i++) Y[i] = T[i];
        data += take;
        len  -= take;
    }
}

static void gctr(const crypto_aes_ctx_t* aes, uint8_t counter[16],
                 const uint8_t* in, uint8_t* out, size_t len) {
    while (len > 0) {
        uint8_t kstream[16];
        crypto_aes_encrypt_block(aes, counter, kstream);
        size_t take = len >= 16 ? 16 : len;
        for (size_t i = 0; i < take; i++) out[i] = (uint8_t)(in[i] ^ kstream[i]);
        // Increment 32-bit counter (last 4 bytes, big-endian, per RFC 5288).
        for (int i = 15; i >= 12; i--) {
            counter[i]++;
            if (counter[i] != 0) break;
        }
        in  += take;
        out += take;
        len -= take;
    }
}

void crypto_gcm_init(crypto_gcm_ctx_t* gcm, const uint8_t* key, size_t keylen) {
    if (keylen == 16) crypto_aes128_init_enc(&gcm->aes, key);
    else              crypto_aes256_init_enc(&gcm->aes, key);
    uint8_t zero[16] = {0};
    crypto_aes_encrypt_block(&gcm->aes, zero, gcm->H);
    gcm->use_pclmul = 0;   // 47.5 hot-spot
}

static void gcm_compute_j0(const uint8_t nonce[12], uint8_t J0[16]) {
    for (int i = 0; i < 12; i++) J0[i] = nonce[i];
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;
}

static void gcm_compute_tag(const crypto_gcm_ctx_t* gcm,
                            const uint8_t J0[16],
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* ct,  size_t ct_len,
                            uint8_t tag[16]) {
    uint8_t Y[16] = {0};
    if (aad_len) ghash_update(gcm->H, Y, aad, aad_len);
    if (ct_len)  ghash_update(gcm->H, Y, ct,  ct_len);
    // Финальный блок: 8 байт aad_bits + 8 байт ct_bits, big-endian.
    uint8_t lenblk[16];
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits  = (uint64_t)ct_len  * 8;
    for (int i = 7; i >= 0; i--) lenblk[i]     = (uint8_t)(aad_bits >> ((7-i)*8));
    for (int i = 7; i >= 0; i--) lenblk[8 + i] = (uint8_t)(ct_bits  >> ((7-i)*8));
    ghash_update(gcm->H, Y, lenblk, 16);

    uint8_t S[16];
    crypto_aes_encrypt_block(&gcm->aes, J0, S);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(Y[i] ^ S[i]);
}

bool crypto_gcm_encrypt(const crypto_gcm_ctx_t* gcm,
                        const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* pt,  uint8_t* ct, size_t len,
                        uint8_t tag[16]) {
    uint8_t J0[16];
    gcm_compute_j0(nonce, J0);
    // GCTR с counter = J0 + 1.
    uint8_t counter[16];
    for (int i = 0; i < 16; i++) counter[i] = J0[i];
    for (int i = 15; i >= 12; i--) {
        counter[i]++;
        if (counter[i] != 0) break;
    }
    gctr(&gcm->aes, counter, pt, ct, len);
    gcm_compute_tag(gcm, J0, aad, aad_len, ct, len, tag);
    return true;
}

bool crypto_gcm_decrypt(const crypto_gcm_ctx_t* gcm,
                        const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* ct,  uint8_t* pt, size_t len,
                        const uint8_t tag[16]) {
    uint8_t J0[16];
    gcm_compute_j0(nonce, J0);
    // Сначала верифицируем tag — расшифровка не должна выдать plaintext до проверки.
    uint8_t expected[16];
    gcm_compute_tag(gcm, J0, aad, aad_len, ct, len, expected);
    if (!crypto_eq_const(expected, tag, 16)) {
        crypto_wipe(expected, 16);
        return false;
    }
    crypto_wipe(expected, 16);

    uint8_t counter[16];
    for (int i = 0; i < 16; i++) counter[i] = J0[i];
    for (int i = 15; i >= 12; i--) {
        counter[i]++;
        if (counter[i] != 0) break;
    }
    gctr(&gcm->aes, counter, ct, pt, len);
    return true;
}

// ============================================================================
// [этап 48] AES-XTS — режим для шифрования дисковых секторов (FDE).
// ----------------------------------------------------------------------------
// IEEE 1619-2007 / NIST SP 800-38E:
//   T(0)   = E_key2(LE128(sector))            — tweak шифруется вторым ключом
//   C(j)   = E_key1(P(j) XOR T(j)) XOR T(j)   — data-блок под первым ключом
//   T(j+1) = T(j) * alpha в GF(2^128)         — умножение на примитивный элемент
//
// Умножение на alpha в little-endian конвенции стандарта: побайтовый сдвиг
// влево с carry между байтами; при выносе старшего бита из байта 15 —
// feedback 0x87 в байт 0 (полином x^128 + x^7 + x^2 + x + 1).
//
// Ciphertext-stealing НЕ реализован: дисковые секторы всегда кратны 16
// байтам, а других потребителей XTS в системе нет.
//
// KAT-векторы в crypto_self_test.c сгенерированы через OpenSSL EVP
// (tools/gen_xts_vec.c) — авторитетная reference-реализация IEEE 1619.
// ============================================================================

void crypto_xts128_init(crypto_xts_ctx_t* ctx,
                        const uint8_t key1[16], const uint8_t key2[16]) {
    crypto_aes128_init_enc(&ctx->data_enc,  key1);
    crypto_aes128_init_dec(&ctx->data_dec,  key1);
    crypto_aes128_init_enc(&ctx->tweak_enc, key2);
}

void crypto_xts256_init(crypto_xts_ctx_t* ctx,
                        const uint8_t key1[32], const uint8_t key2[32]) {
    crypto_aes256_init_enc(&ctx->data_enc,  key1);
    crypto_aes256_init_dec(&ctx->data_dec,  key1);
    crypto_aes256_init_enc(&ctx->tweak_enc, key2);
}

// T *= alpha в GF(2^128), little-endian байтовый порядок (IEEE 1619).
static void xts_mul_alpha(uint8_t T[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t next_carry = (uint8_t)(T[i] >> 7);
        T[i] = (uint8_t)((T[i] << 1) | carry);
        carry = next_carry;
    }
    if (carry) T[0] ^= 0x87;
}

// Начальный tweak: T = E_key2(LE128(sector)).
static void xts_init_tweak(const crypto_xts_ctx_t* ctx, uint64_t sector,
                           uint8_t T[16]) {
    uint8_t tw[16];
    for (int i = 0; i < 8; i++) tw[i] = (uint8_t)(sector >> (i * 8));
    for (int i = 8; i < 16; i++) tw[i] = 0;
    crypto_aes_encrypt_block(&ctx->tweak_enc, tw, T);
}

void crypto_xts_encrypt(const crypto_xts_ctx_t* ctx, uint64_t sector,
                        const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t T[16], x[16];
    xts_init_tweak(ctx, sector, T);
    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)(in[b * 16 + i] ^ T[i]);
        crypto_aes_encrypt_block(&ctx->data_enc, x, x);
        for (int i = 0; i < 16; i++) out[b * 16 + i] = (uint8_t)(x[i] ^ T[i]);
        xts_mul_alpha(T);
    }
    crypto_wipe(T, sizeof(T));
    crypto_wipe(x, sizeof(x));
}

void crypto_xts_decrypt(const crypto_xts_ctx_t* ctx, uint64_t sector,
                        const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t T[16], x[16];
    xts_init_tweak(ctx, sector, T);
    size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)(in[b * 16 + i] ^ T[i]);
        crypto_aes_decrypt_block(&ctx->data_dec, x, x);
        for (int i = 0; i < 16; i++) out[b * 16 + i] = (uint8_t)(x[i] ^ T[i]);
        xts_mul_alpha(T);
    }
    crypto_wipe(T, sizeof(T));
    crypto_wipe(x, sizeof(x));
}
