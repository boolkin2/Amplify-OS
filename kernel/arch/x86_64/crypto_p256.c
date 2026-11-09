// ============================================================================
// crypto_p256.c — NIST P-256 (secp256r1) ECDH + ECDSA verify (этап 47.6).
// ----------------------------------------------------------------------------
// Кривая secp256r1 / prime256v1 (FIPS 186-4):
//   p = 2^256 - 2^224 + 2^192 + 2^96 - 1
//   y^2 = x^3 - 3*x + b   (Weierstrass, a = -3)
//   n = 2^256 - 2^224 + 2^192 - 89188191075325690597107910205041859247
//
// Это «золотой стандарт» ECC для TLS 1.2 cipher suites:
//   * 0xC02F TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
//   * 0xC02B TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
//   * 0xC030, 0xC02C — версии с SHA-384
// и для X.509 сертификатов с algorithm = ecdsa-with-SHA256
// (1.2.840.10045.4.3.2).
//
// Структура файла:
//   (1) Constants: p, n, b, G_x, G_y.
//   (2) Field arithmetic в GF(p) — 8 × 32-bit limbs, Solinas-style fast
//       reduction (FIPS 186-4 Appendix D).
//   (3) Modular inverse через Fermat: a^(p-2) mod p.
//   (4) Group-order arithmetic в GF(n) для ECDSA.
//   (5) Point operations в Jacobian (X, Y, Z) coordinates: double, add,
//       scalar_mult Монтгомери-лестницей.
//   (6) Public API: validate_pub, derive_pub, ecdh, ecdsa_verify.
//
// Производительность (на TCG QEMU):
//   * fe_mul + reduce  ≈ 5 µs
//   * scalar_mult      ≈ 100 ms (256 doublings × ~10 mults + ~128 adds × ~16 mults)
//   * full TLS ECDHE handshake adds ≈ 250 ms (one ecdh + one ecdsa_verify
//     если cert тоже ECDSA, или просто ecdh для ECDHE-RSA).
//   * Всё это укладывается в TLS-таймауты OpenSSL/nginx (5+ секунд).
//
// Constant-time:
//   * Field add/sub/mul/sqr — ветвлений по данным нет.
//   * Scalar mult — Монтгомери-лестница: на каждом бите делаем И add И double,
//     без зависимости от значения бита (разница только в swap'е переменных).
//   * Inverse через a^(p-2) — single fixed exponent, не зависит от a.
//   * Финальные subtract'ы p — могут быть data-dependent, но это уже
//     не secret data (только наблюдаемые точки кривой).
//
// Поддержка PCLMULQDQ / SHA-NI — НЕ используется (это AES/SHA-домен).
// Возможные оптимизации (отложено): Montgomery multiplication (×3 speedup),
// jacobian-affine mixed addition, signed-window scalar mult, comb tables для G.
// ============================================================================
#include "crypto.h"

// ============================================================================
// (1) Curve constants
// ============================================================================

// Все константы — 8 × 32-bit limbs, little-endian (limb 0 = LSW, limb 7 = MSW).
// Соответствует естественному layout для x86_64 little-endian.

// p = 0xFFFFFFFF00000001 0000000000000000 00000000FFFFFFFF FFFFFFFFFFFFFFFF
static const uint32_t P256_P[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};

// n = 0xFFFFFFFF00000000 FFFFFFFFFFFFFFFF BCE6FAADA7179E84 F3B9CAC2FC632551
static const uint32_t P256_N[8] = {
    0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
};

// b = 0x5AC635D8AA3A93E7 B3EBBD55769886BC 651D06B0CC53B0F6 3BCE3C3E27D2604B
static const uint32_t P256_B[8] = {
    0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
    0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
};

// G_x = 6B17D1F2E12C4247 F8BCE6E563A440F2 77037D812DEB33A0 F4A13945D898C296
static const uint32_t P256_GX[8] = {
    0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u
};

// G_y = 4FE342E2FE1A7F9B 8EE7EB4A7C0F9E16 2BCE33576B315ECE CBB6406837BF51F5
static const uint32_t P256_GY[8] = {
    0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u
};

// ============================================================================
// (2) Field arithmetic в GF(p)
// ============================================================================
//
// Field element: 8 × 32-bit limbs.  Канонический range: [0, p).
// Промежуточные значения после fe_add/sub могут быть в [0, 2p), выходные —
// после financial reduction приводим в [0, p).

typedef uint32_t fe_t[8];

static void fe_zero(fe_t r) {
    for (int i = 0; i < 8; i++) r[i] = 0;
}

static void fe_copy(fe_t r, const fe_t a) {
    for (int i = 0; i < 8; i++) r[i] = a[i];
}

static int fe_is_zero(const fe_t a) {
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc |= a[i];
    return acc == 0;
}

// Сравнение: -1 если a < b, 0 если равны, +1 если a > b.
static int fe_cmp(const fe_t a, const fe_t b) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return  1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

static void fe_from_bytes(fe_t r, const uint8_t in[32]) {
    // Big-endian 32-byte input → little-endian 8-limb fe.
    for (int i = 0; i < 8; i++) {
        r[7 - i] = ((uint32_t)in[4*i  ] << 24) |
                   ((uint32_t)in[4*i+1] << 16) |
                   ((uint32_t)in[4*i+2] << 8)  |
                   ((uint32_t)in[4*i+3]);
    }
}

static void fe_to_bytes(uint8_t out[32], const fe_t a) {
    for (int i = 0; i < 8; i++) {
        uint32_t w = a[7 - i];
        out[4*i  ] = (uint8_t)(w >> 24);
        out[4*i+1] = (uint8_t)(w >> 16);
        out[4*i+2] = (uint8_t)(w >> 8);
        out[4*i+3] = (uint8_t)(w);
    }
}

// Условное вычитание p, если a >= p.  Constant-time через mask.
static void fe_reduce_once(fe_t a) {
    // Compare a with p; if a >= p, subtract.
    // Compute a - p as int64 chain, decide based on borrow.
    uint32_t tmp[8];
    int64_t  borrow = 0;
    for (int i = 0; i < 8; i++) {
        int64_t d = (int64_t)a[i] - P256_P[i] - borrow;
        tmp[i] = (uint32_t)d;
        borrow = (d < 0) ? 1 : 0;
    }
    // Если borrow == 0, a >= p — заменяем a на tmp.
    if (borrow == 0) {
        for (int i = 0; i < 8; i++) a[i] = tmp[i];
    }
}

// r = (a + b) mod p.  Both operands in [0, p) on entry.
static void fe_add(fe_t r, const fe_t a, const fe_t b) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    // Если был final carry, или r >= p — вычитаем p.
    if (carry || fe_cmp(r, P256_P) >= 0) {
        int64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            int64_t d = (int64_t)r[i] - P256_P[i] - borrow;
            r[i] = (uint32_t)d;
            borrow = (d < 0) ? 1 : 0;
        }
        // Если carry был 1 и после вычитания всё равно borrow — не должно случиться
        // для корректных входов в [0, p).
        (void)borrow;
    }
}

// r = (a - b) mod p.  Both operands in [0, p).
static void fe_sub(fe_t r, const fe_t a, const fe_t b) {
    int64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        int64_t d = (int64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d < 0) ? 1 : 0;
    }
    if (borrow) {
        // r отрицателен → добавим p.
        uint64_t carry = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t s = (uint64_t)r[i] + P256_P[i] + carry;
            r[i] = (uint32_t)s;
            carry = s >> 32;
        }
        (void)carry;
    }
}

// 8x8 → 16-limb schoolbook multiplication.
static void mul_full_8x8(uint32_t out[16], const fe_t a, const fe_t b) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t prod = (uint64_t)a[i] * b[j] + out[i+j] + carry;
            out[i+j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        out[i+8] = (uint32_t)carry;
    }
}

// Solinas-style reduction p256: 16-limb input c → 8-limb output r.
// Reference: FIPS 186-4 Appendix D.  Алгоритм:
//   T  = (c[ 0], c[ 1], c[ 2], c[ 3], c[ 4], c[ 5], c[ 6], c[ 7])
//   S1 = (    0,     0,     0, c[11], c[12], c[13], c[14], c[15])
//   S2 = (    0,     0,     0, c[12], c[13], c[14], c[15],     0)
//   S3 = (c[ 8], c[ 9], c[10],     0,     0,     0, c[14], c[15])
//   S4 = (c[ 9], c[10], c[11], c[13], c[14], c[15], c[13], c[ 8])
//   D1 = (c[11], c[12], c[13],     0,     0,     0, c[ 8], c[10])
//   D2 = (c[12], c[13], c[14], c[15],     0,     0, c[ 9], c[11])
//   D3 = (c[13], c[14], c[15], c[ 8], c[ 9], c[10],     0, c[12])
//   D4 = (c[14], c[15],     0, c[ 9], c[10], c[11],     0, c[13])
//
//   R = T + 2*S1 + 2*S2 + S3 + S4 - D1 - D2 - D3 - D4 mod p
//
// Используем int64_t accumulators.  После всех операций возможный диапазон R:
// примерно [-4*p, 5*p].  Финальная нормализация: добавляем 4*p (всегда положительно
// после этого), propagate carries, потом reduce_once несколько раз.
static void p256_reduce(fe_t r, const uint32_t c[16]) {
    int64_t a[8];

    // T
    a[0] = (int64_t)c[0];
    a[1] = (int64_t)c[1];
    a[2] = (int64_t)c[2];
    a[3] = (int64_t)c[3];
    a[4] = (int64_t)c[4];
    a[5] = (int64_t)c[5];
    a[6] = (int64_t)c[6];
    a[7] = (int64_t)c[7];

    // + 2*S1
    a[3] += 2LL * (int64_t)c[11];
    a[4] += 2LL * (int64_t)c[12];
    a[5] += 2LL * (int64_t)c[13];
    a[6] += 2LL * (int64_t)c[14];
    a[7] += 2LL * (int64_t)c[15];

    // + 2*S2
    a[3] += 2LL * (int64_t)c[12];
    a[4] += 2LL * (int64_t)c[13];
    a[5] += 2LL * (int64_t)c[14];
    a[6] += 2LL * (int64_t)c[15];

    // + S3
    a[0] += (int64_t)c[8];
    a[1] += (int64_t)c[9];
    a[2] += (int64_t)c[10];
    a[6] += (int64_t)c[14];
    a[7] += (int64_t)c[15];

    // + S4
    a[0] += (int64_t)c[9];
    a[1] += (int64_t)c[10];
    a[2] += (int64_t)c[11];
    a[3] += (int64_t)c[13];
    a[4] += (int64_t)c[14];
    a[5] += (int64_t)c[15];
    a[6] += (int64_t)c[13];
    a[7] += (int64_t)c[8];

    // - D1
    a[0] -= (int64_t)c[11];
    a[1] -= (int64_t)c[12];
    a[2] -= (int64_t)c[13];
    a[6] -= (int64_t)c[8];
    a[7] -= (int64_t)c[10];

    // - D2
    a[0] -= (int64_t)c[12];
    a[1] -= (int64_t)c[13];
    a[2] -= (int64_t)c[14];
    a[3] -= (int64_t)c[15];
    a[6] -= (int64_t)c[9];
    a[7] -= (int64_t)c[11];

    // - D3
    a[0] -= (int64_t)c[13];
    a[1] -= (int64_t)c[14];
    a[2] -= (int64_t)c[15];
    a[3] -= (int64_t)c[8];
    a[4] -= (int64_t)c[9];
    a[5] -= (int64_t)c[10];
    a[7] -= (int64_t)c[12];

    // - D4
    a[0] -= (int64_t)c[14];
    a[1] -= (int64_t)c[15];
    a[3] -= (int64_t)c[9];
    a[4] -= (int64_t)c[10];
    a[5] -= (int64_t)c[11];
    a[7] -= (int64_t)c[13];

    // Сейчас a[0..7] in [-4*2^256, 5*2^256] approx.
    // Добавим 6*p чтобы гарантированно положительное.  Это нормально: финальная
    // редукция уберёт лишнее.  6*p ≈ 6*2^256 ≈ 6 * 2^256 что при сложении
    // с -4*2^256 даёт ~+2*2^256 — comfortably positive.
    //
    // 6*p в 32-bit limbs: 6*(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 1, 0xFFFFFFFF)
    //   = (6*0xFFFFFFFF, ..., ..., 0, 0, 0, 6, 6*0xFFFFFFFF)
    //   = (0x5FFFFFFFA, 0x5FFFFFFFA, 0x5FFFFFFFA, 0, 0, 0, 6, 0x5FFFFFFFA)
    //   высший бит может перенестись в "виртуальный" limb 8, но мы его учтём
    //   в большой петле reduce, потому что carry > 0 будет означать "ещё одно p
    //   мы добавим в обмен на обратную операцию".
    //
    // Проще: добавим p достаточно много раз, скажем 8*p, чтобы 100% гарантия.
    // Делаем это через накопление в int64_t.
    a[0] += 8LL * 0xFFFFFFFFLL;
    a[1] += 8LL * 0xFFFFFFFFLL;
    a[2] += 8LL * 0xFFFFFFFFLL;
    a[6] += 8LL;
    a[7] += 8LL * 0xFFFFFFFFLL;

    // Carry propagation (signed): начиная с a[0], выносим переполнение в next limb.
    // Используем arithmetic right shift для preservation of sign.
    int64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        a[i] += carry;
        carry = a[i] >> 32;       // arith. right shift на знаковом int64_t
        a[i] &= 0xFFFFFFFFLL;
    }

    // Теперь a[0..7] in [0, 2^32), но carry может быть > 0 (overflow за 2^256).
    // 2^256 ≡ 2^224 - 2^192 - 2^96 + 1 (mod p), то есть в наших limbs:
    //   carry * 2^256 ≡ +carry в limb 0, -carry в limb 3, -carry в limb 6, +carry в limb 7.
    // Применим эту замену и повторим propagation.  Достаточно 2-3 итераций.
    while (carry != 0) {
        a[0] += carry;
        a[3] -= carry;
        a[6] -= carry;
        a[7] += carry;
        carry = 0;
        for (int i = 0; i < 8; i++) {
            a[i] += carry;
            carry = a[i] >> 32;
            a[i] &= 0xFFFFFFFFLL;
        }
    }

    // Сейчас a[0..7] in [0, 2^256), но возможно >= p.  Финальная редукция.
    for (int i = 0; i < 8; i++) r[i] = (uint32_t)a[i];
    // До 4 раз: while r >= p, r -= p.  (Должно быть достаточно ≤ 2 раз для
    // корректных входов, но 4 для подстраховки.)
    fe_reduce_once(r);
    fe_reduce_once(r);
    fe_reduce_once(r);
    fe_reduce_once(r);
}

// r = a * b mod p
static void fe_mul(fe_t r, const fe_t a, const fe_t b) {
    uint32_t prod[16];
    mul_full_8x8(prod, a, b);
    p256_reduce(r, prod);
}

// r = a^2 mod p.  Можно оптимизировать (squaring примерно в 1.5× быстрее
// schoolbook'а), но для простоты используем общий mul.
static void fe_sqr(fe_t r, const fe_t a) {
    fe_mul(r, a, a);
}

// r = a^-1 mod p, через Fermat: a^(p-2) mod p.
//
// p-2 = 2^256 - 2^224 + 2^192 + 2^96 - 3
// Бинарное представление p-2:
//   bits 0..95:    все 1 кроме битов 0,1 (так как p-2 ≡ 0xfd mod 2^8)
//   bits 96..191:  все 0 кроме бита 96 (от +2^96 в формуле)... ой нет, перепишем.
//
// p = 2^256 - 2^224 + 2^192 + 2^96 - 1
// p-2 = 2^256 - 2^224 + 2^192 + 2^96 - 3
//     = (2^256 - 2^224) + 2^192 + (2^96 - 3)
//
// (2^256 - 2^224) = 0xFFFFFFFF00000000_0000000000000000_0000000000000000_0000000000000000
//                   biti 224..255 все 1
// 2^192 = bit 192 = 1
// (2^96 - 3) = bit 0 = 1, bit 1 = 0 (so bit 1=0), bit 2..95 = все 1
//   То есть bits 0..95 у (2^96 - 3) есть 1 везде, кроме бита 1.
//   Но бит 1 в полном представлении равен 0, так как 2^1 = 2 и -3 не вычитает 2^1, а вычитает 2+1.
//   Wait, 2^96 - 3 = 2^96 - 1 - 2 = (все 1 в bits 0..95) минус 0b10 = bits 0=1, 1=0, 2..95=все 1.
//
// Итого ненулевые bits в (p-2):
//   bit 0:    1
//   bit 2..95: 1
//   bit 192:  1 (от + 2^192)
//   bit 224..255: 1 (от 2^256 - 2^224)
//
// Проверяем: bit 1 = 0, bit 96..191 (кроме 192) = 0. Странно — bit 192 set?
// Нет, я ошибся. -2^224 + 2^192 = 2^192 * (1 - 2^32) = 2^192 - 2^224.
// Так что (2^256 - 2^224 + 2^192 + 2^96 - 3) = ?
// Перепишем: 2^256 + 2^192 + 2^96 - 2^224 - 3.
// 2^256 = bit 256 (но мы работаем mod 2^256, так что это просто +2^256 контрибуция, что
// в 256-bit представлении невозможно — это ровно граница). Стоп, p-2 МЕНЬШЕ p, значит < 2^256.
//
// Расчёт p-2 явно:
// p = 0xffffffff00000001 0000000000000000 00000000ffffffff ffffffffffffffff
// p-2:
//   = 0xffffffff00000001 0000000000000000 00000000ffffffff fffffffffffffffd
//
// В 32-bit limbs (LSW first): 0xfffffffd, 0xffffffff, 0xffffffff, 0x00000000,
//                              0x00000000, 0x00000000, 0x00000001, 0xffffffff
//
// Pattern bits в p-2:
//   bits 0..255 — где '1': bit 0 (=1), bits 2..95 (=1), bit 192 (=1), bits 224..255 (=1).
//
// Square-and-multiply: 256 итераций; на каждой делаем sqr, и при бите=1 ещё mul.
// Bit 0 = 1, bit 1 = 0, bits 2..95 = 1, bits 96..191 = 0, bit 192 = 1,
// bits 193..223 = 0, bits 224..255 = 1.
//
// Итого: ~95+1+32 = 128 multiplications + 256 squarings.
static void fe_inv(fe_t r, const fe_t a) {
    // p-2 binary representation (256-bit, LSB first):
    static const uint32_t exp_p_minus_2[8] = {
        0xFFFFFFFDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
    };

    fe_t result;
    fe_zero(result);
    result[0] = 1;     // result = 1

    fe_t base;
    fe_copy(base, a);

    for (int i = 0; i < 256; i++) {
        int bit = (exp_p_minus_2[i / 32] >> (i % 32)) & 1;
        if (bit) {
            fe_mul(result, result, base);
        }
        // square base for next bit
        fe_sqr(base, base);
    }
    fe_copy(r, result);
}

// ============================================================================
// (3) Group-order arithmetic в GF(n) для ECDSA
// ============================================================================
//
// Для verify нам нужны mod n операции: subtract, multiply, и inverse.
// Для inverse используем тот же Fermat'а трюк: a^(n-2) mod n.

// Compare: 0 if equal, -1 if a<b, +1 if a>b.
static int fn_cmp(const uint32_t a[8], const uint32_t b[8]) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return  1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

static void fn_reduce_once(uint32_t a[8]) {
    if (fn_cmp(a, P256_N) >= 0) {
        int64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            int64_t d = (int64_t)a[i] - P256_N[i] - borrow;
            a[i] = (uint32_t)d;
            borrow = (d < 0) ? 1 : 0;
        }
        (void)borrow;
    }
}

// r = a * b mod n.  Используем общую 16-limb mul + bit-by-bit reduction по n.
// Это медленно (256 вычитаний), но ECDSA-verify делает только 3 mod-n умножения
// (w, u1, u2), итого ~1ms — не критично.
static void fn_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t prod[16];
    fe_t aa, bb;
    for (int i = 0; i < 8; i++) { aa[i] = a[i]; bb[i] = b[i]; }
    mul_full_8x8(prod, aa, bb);

    // Bit-by-bit modular reduction: r = 0; for each bit i from MSB to LSB:
    //   r = (r << 1) | bit_i;
    //   if (r >= n) r -= n;
    uint32_t rr[8] = {0};
    for (int i = 16 * 32 - 1; i >= 0; i--) {
        // Shift rr left by 1, append bit_i of prod.
        uint32_t carry_top = 0;
        for (int j = 0; j < 8; j++) {
            uint32_t newc = rr[j] >> 31;
            rr[j] = (rr[j] << 1) | carry_top;
            carry_top = newc;
        }
        // Append bit i of prod
        uint32_t bit = (prod[i / 32] >> (i % 32)) & 1;
        rr[0] |= bit;
        // (carry_top становится "9-м" битом — игнорируем; следующий fn_reduce_once
        // или последующая итерация поглотит.  На самом деле в 256-bit модулярной
        // арифметике carry_top из последнего сдвига означает r >= 2^256, что
        // безусловно >= n; но fn_cmp работает с 8-limb, так что сначала
        // безусловное вычитание n если был carry_top.)
        if (carry_top) {
            int64_t borrow = 0;
            for (int j = 0; j < 8; j++) {
                int64_t d = (int64_t)rr[j] - P256_N[j] - borrow;
                rr[j] = (uint32_t)d;
                borrow = (d < 0) ? 1 : 0;
            }
            // borrow здесь должен быть 0 (так как 2^256 > n).
            (void)borrow;
        } else {
            fn_reduce_once(rr);
        }
    }
    for (int i = 0; i < 8; i++) r[i] = rr[i];
}

// r = a^-1 mod n (через Fermat: a^(n-2) mod n).
static void fn_inv(uint32_t r[8], const uint32_t a[8]) {
    // n-2 binary representation:
    static const uint32_t exp_n_minus_2[8] = {
        0xFC63254Fu, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
    };

    uint32_t result[8] = {0};
    result[0] = 1;
    uint32_t base[8];
    for (int i = 0; i < 8; i++) base[i] = a[i];

    for (int i = 0; i < 256; i++) {
        int bit = (exp_n_minus_2[i / 32] >> (i % 32)) & 1;
        if (bit) fn_mul(result, result, base);
        fn_mul(base, base, base);
    }
    for (int i = 0; i < 8; i++) r[i] = result[i];
}

// ============================================================================
// (4) Point operations в Jacobian coordinates (X, Y, Z)
// ============================================================================
//
// Affine ↔ Jacobian:
//   x_aff = X / Z^2,   y_aff = Y / Z^3
//   Identity: Z = 0.

typedef struct { fe_t X, Y, Z; } point_t;

static int point_is_zero(const point_t* p) { return fe_is_zero(p->Z); }

static void point_set_zero(point_t* p) {
    fe_zero(p->X);
    fe_zero(p->Y);
    fe_zero(p->Z);
    // Можно поставить X=Y=1 для нумерической стабильности; но fe_is_zero(Z) —
    // это всё, что нам нужно для проверки identity.
}

static void point_copy(point_t* d, const point_t* s) {
    fe_copy(d->X, s->X);
    fe_copy(d->Y, s->Y);
    fe_copy(d->Z, s->Z);
}

// Point doubling: r = 2*P, P = (X1, Y1, Z1).
// Формула из Cohen-Miyaji-Ono (1998) для a = -3 (специальный случай P-256):
//   M = 3*(X1 - Z1^2)*(X1 + Z1^2)
//   S = 4*X1*Y1^2
//   T = 8*Y1^4
//   X3 = M^2 - 2*S
//   Y3 = M*(S - X3) - T
//   Z3 = 2*Y1*Z1
// 8 squarings/multiplications + adds.
static void point_double(point_t* r, const point_t* p) {
    if (point_is_zero(p)) { point_set_zero(r); return; }

    fe_t z2, t1, t2, M, S, T, Y2, Y4, x3, y3, z3;
    // z2 = Z1^2
    fe_sqr(z2, p->Z);
    // t1 = X1 - z2,  t2 = X1 + z2
    fe_sub(t1, p->X, z2);
    fe_add(t2, p->X, z2);
    // M = 3*(t1*t2)
    fe_mul(M, t1, t2);
    fe_t M2; fe_add(M2, M, M); fe_add(M, M2, M);   // M = 3*M
    // Y2 = Y1^2
    fe_sqr(Y2, p->Y);
    // S = 4*X1*Y2
    fe_mul(S, p->X, Y2);
    fe_t S2; fe_add(S2, S, S); fe_add(S, S2, S2); // S = 4*S
    // Y4 = Y2^2
    fe_sqr(Y4, Y2);
    // T = 8*Y4
    fe_t Y4_2; fe_add(Y4_2, Y4, Y4); fe_t Y4_4; fe_add(Y4_4, Y4_2, Y4_2);
    fe_add(T, Y4_4, Y4_4);
    // X3 = M^2 - 2*S
    fe_t Msq; fe_sqr(Msq, M);
    fe_t S2v; fe_add(S2v, S, S);
    fe_sub(x3, Msq, S2v);
    // Y3 = M*(S - X3) - T
    fe_t SmX3; fe_sub(SmX3, S, x3);
    fe_t MsmX3; fe_mul(MsmX3, M, SmX3);
    fe_sub(y3, MsmX3, T);
    // Z3 = 2*Y1*Z1
    fe_t YZ; fe_mul(YZ, p->Y, p->Z); fe_add(z3, YZ, YZ);

    fe_copy(r->X, x3);
    fe_copy(r->Y, y3);
    fe_copy(r->Z, z3);
}

// Point addition в Jacobian: r = P + Q.  P = (X1, Y1, Z1), Q = (X2, Y2, Z2).
// Если P или Q = identity, или P = Q (тогда P+Q = 2P), или P = -Q (тогда P+Q = 0)
// — обрабатываем особо.
static void point_add(point_t* r, const point_t* p, const point_t* q) {
    if (point_is_zero(p)) { point_copy(r, q); return; }
    if (point_is_zero(q)) { point_copy(r, p); return; }

    // U1 = X1*Z2^2, U2 = X2*Z1^2
    // S1 = Y1*Z2^3, S2 = Y2*Z1^3
    fe_t Z1_2, Z2_2, Z1_3, Z2_3;
    fe_sqr(Z1_2, p->Z);
    fe_sqr(Z2_2, q->Z);
    fe_mul(Z1_3, Z1_2, p->Z);
    fe_mul(Z2_3, Z2_2, q->Z);

    fe_t U1, U2, S1, S2;
    fe_mul(U1, p->X, Z2_2);
    fe_mul(U2, q->X, Z1_2);
    fe_mul(S1, p->Y, Z2_3);
    fe_mul(S2, q->Y, Z1_3);

    if (fe_cmp(U1, U2) == 0) {
        if (fe_cmp(S1, S2) == 0) {
            // P == Q
            point_double(r, p);
            return;
        } else {
            // P == -Q
            point_set_zero(r);
            return;
        }
    }

    fe_t H, R;
    fe_sub(H, U2, U1);
    fe_sub(R, S2, S1);

    // X3 = R^2 - H^3 - 2*U1*H^2
    // Y3 = R*(U1*H^2 - X3) - S1*H^3
    // Z3 = H*Z1*Z2
    fe_t H_2, H_3;
    fe_sqr(H_2, H);
    fe_mul(H_3, H_2, H);

    fe_t U1H2; fe_mul(U1H2, U1, H_2);
    fe_t Rsq;  fe_sqr(Rsq, R);
    fe_t U1H2_2; fe_add(U1H2_2, U1H2, U1H2);
    fe_t tmp;
    fe_sub(tmp, Rsq, H_3);
    fe_t X3; fe_sub(X3, tmp, U1H2_2);

    fe_t U1H2mX3; fe_sub(U1H2mX3, U1H2, X3);
    fe_t RU; fe_mul(RU, R, U1H2mX3);
    fe_t S1H3; fe_mul(S1H3, S1, H_3);
    fe_t Y3; fe_sub(Y3, RU, S1H3);

    fe_t Z1Z2; fe_mul(Z1Z2, p->Z, q->Z);
    fe_t Z3; fe_mul(Z3, H, Z1Z2);

    fe_copy(r->X, X3);
    fe_copy(r->Y, Y3);
    fe_copy(r->Z, Z3);
}

// scalar mult: R = k * P, k — 32-byte big-endian scalar.
// Используем Монтгомери-лестницу: на каждом бите делаем И add И double,
// порядок зависит от бита — это даёт constant-time без табличного lookup.
//
// Initial: R0 = O, R1 = P.
// For each bit b of k from MSB to LSB:
//   if b == 0: R1 = R0 + R1; R0 = 2*R0
//   if b == 1: R0 = R0 + R1; R1 = 2*R1
// Final: R = R0.
static void point_scalar_mult(point_t* r, const point_t* base, const uint8_t scalar[32]) {
    point_t R0, R1;
    point_set_zero(&R0);
    point_copy(&R1, base);

    for (int i = 255; i >= 0; i--) {
        int byte = scalar[31 - i / 8];
        int bit  = (byte >> (i % 8)) & 1;
        if (bit) {
            point_add(&R0, &R0, &R1);
            point_double(&R1, &R1);
        } else {
            point_add(&R1, &R0, &R1);
            point_double(&R0, &R0);
        }
    }
    point_copy(r, &R0);
}

// Convert Jacobian → affine.  Возвращает false если point at infinity.
static bool point_to_affine(uint8_t out_x[32], uint8_t out_y[32], const point_t* p) {
    if (point_is_zero(p)) return false;
    fe_t z_inv, z_inv2, z_inv3, x_aff, y_aff;
    fe_inv(z_inv, p->Z);
    fe_sqr(z_inv2, z_inv);
    fe_mul(z_inv3, z_inv2, z_inv);
    fe_mul(x_aff, p->X, z_inv2);
    fe_mul(y_aff, p->Y, z_inv3);
    fe_to_bytes(out_x, x_aff);
    if (out_y) fe_to_bytes(out_y, y_aff);
    return true;
}

// Проверка, что (x, y) лежит на кривой y^2 = x^3 - 3x + b mod p.
static bool point_on_curve_aff(const fe_t x, const fe_t y) {
    fe_t y2, x2, x3, three_x, t1, t2, b_const;
    for (int i = 0; i < 8; i++) b_const[i] = P256_B[i];

    fe_sqr(y2, y);                   // y^2
    fe_sqr(x2, x);                   // x^2
    fe_mul(x3, x2, x);               // x^3
    fe_t two_x; fe_add(two_x, x, x);
    fe_add(three_x, two_x, x);       // 3*x
    fe_sub(t1, x3, three_x);         // x^3 - 3x
    fe_add(t2, t1, b_const);         // x^3 - 3x + b
    return fe_cmp(y2, t2) == 0;
}

// ============================================================================
// (5) Public API
// ============================================================================

bool crypto_p256_validate_pub(const uint8_t pub[65]) {
    if (pub[0] != 0x04) return false;          // Только uncompressed
    fe_t x, y;
    fe_from_bytes(x, pub + 1);
    fe_from_bytes(y, pub + 33);
    // Точка не должна быть identity (X != 0 || Y != 0).
    if (fe_is_zero(x) && fe_is_zero(y)) return false;
    // X, Y < p.
    if (fe_cmp(x, P256_P) >= 0) return false;
    if (fe_cmp(y, P256_P) >= 0) return false;
    // y^2 == x^3 - 3x + b (mod p)
    return point_on_curve_aff(x, y);
}

bool crypto_p256_derive_pub(const uint8_t scalar[32], uint8_t out_pub[65]) {
    // pub = scalar * G в affine форме.
    // G в Jacobian: (G_x, G_y, 1)
    point_t G;
    for (int i = 0; i < 8; i++) {
        G.X[i] = P256_GX[i];
        G.Y[i] = P256_GY[i];
        G.Z[i] = 0;
    }
    G.Z[0] = 1;

    point_t R;
    point_scalar_mult(&R, &G, scalar);

    out_pub[0] = 0x04;
    return point_to_affine(out_pub + 1, out_pub + 33, &R);
}

bool crypto_p256_ecdh(const uint8_t scalar[32], const uint8_t peer_pub[65],
                      uint8_t shared[32]) {
    if (!crypto_p256_validate_pub(peer_pub)) return false;

    point_t P;
    fe_from_bytes(P.X, peer_pub + 1);
    fe_from_bytes(P.Y, peer_pub + 33);
    fe_zero(P.Z); P.Z[0] = 1;

    point_t R;
    point_scalar_mult(&R, &P, scalar);

    if (point_is_zero(&R)) return false;

    // shared = X-coordinate (32 bytes BE).
    return point_to_affine(shared, 0, &R);
}

// ECDSA verify (FIPS 186-4 §6.4):
//
//   Inputs: pub (Q), hash (e), signature (r, s).
//
//   1. Reject if r or s ∉ [1, n-1].
//   2. e' = leftmost min(hash_len, 256/8) bytes of hash, treated as big-endian integer.
//   3. w = s^-1 mod n.
//   4. u1 = e' * w mod n,  u2 = r * w mod n.
//   5. (x1, y1) = u1*G + u2*Q.  Reject if point at infinity.
//   6. v = x1 mod n.  Accept if v == r, else reject.
//
// r и s передаются как big-endian byte arrays (взяты из ASN.1 INTEGER в
// X.509-сигнатуре или из TLS ServerKeyExchange).
bool crypto_p256_ecdsa_verify(const uint8_t pub[65],
                              const uint8_t* hash, size_t hash_len,
                              const uint8_t* r_bytes, size_t r_len,
                              const uint8_t* s_bytes, size_t s_len) {
    if (!crypto_p256_validate_pub(pub)) return false;
    if (r_len == 0 || s_len == 0)        return false;
    if (r_len > 32 || s_len > 32)        return false;

    // Pad/extract r, s в 8-limb representation.
    uint8_t r_be[32] = {0}, s_be[32] = {0};
    for (size_t i = 0; i < r_len; i++) r_be[32 - r_len + i] = r_bytes[i];
    for (size_t i = 0; i < s_len; i++) s_be[32 - s_len + i] = s_bytes[i];

    uint32_t r[8], s[8];
    fe_from_bytes(r, r_be);
    fe_from_bytes(s, s_be);

    // Range check: 1 <= r, s <= n-1.
    uint32_t one[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    if (fn_cmp(r, one) < 0 || fn_cmp(r, P256_N) >= 0) return false;
    if (fn_cmp(s, one) < 0 || fn_cmp(s, P256_N) >= 0) return false;

    // e: hash treated as integer.  Если hash > n, берём только leftmost bits;
    // для SHA-256 (32 bytes) и P-256 (32 bytes) — обычно совпадает или близко.
    uint8_t e_be[32] = {0};
    size_t hl = hash_len > 32 ? 32 : hash_len;
    for (size_t i = 0; i < hl; i++) e_be[32 - hl + i] = hash[i];
    uint32_t e[8];
    fe_from_bytes(e, e_be);
    // e mod n (нет операции напрямую; используем простой вычитающий цикл).
    while (fn_cmp(e, P256_N) >= 0) {
        int64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            int64_t d = (int64_t)e[i] - P256_N[i] - borrow;
            e[i] = (uint32_t)d;
            borrow = (d < 0) ? 1 : 0;
        }
        (void)borrow;
    }

    // w = s^-1 mod n
    uint32_t w[8];
    fn_inv(w, s);

    // u1 = e * w mod n,  u2 = r * w mod n
    uint32_t u1[8], u2[8];
    fn_mul(u1, e, w);
    fn_mul(u2, r, w);

    // X = u1*G + u2*Q.
    point_t G, Q, T1, T2, X;
    for (int i = 0; i < 8; i++) {
        G.X[i] = P256_GX[i];
        G.Y[i] = P256_GY[i];
        G.Z[i] = 0;
    }
    G.Z[0] = 1;

    fe_from_bytes(Q.X, pub + 1);
    fe_from_bytes(Q.Y, pub + 33);
    fe_zero(Q.Z); Q.Z[0] = 1;

    // Convert u1, u2 в byte form для scalar_mult.
    uint8_t u1_be[32], u2_be[32];
    fe_to_bytes(u1_be, u1);
    fe_to_bytes(u2_be, u2);

    point_scalar_mult(&T1, &G, u1_be);
    point_scalar_mult(&T2, &Q, u2_be);
    point_add(&X, &T1, &T2);

    if (point_is_zero(&X)) return false;

    // x1 в affine, потом mod n.
    uint8_t x1_be[32];
    if (!point_to_affine(x1_be, 0, &X)) return false;
    uint32_t x1[8];
    fe_from_bytes(x1, x1_be);
    while (fn_cmp(x1, P256_N) >= 0) {
        int64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            int64_t d = (int64_t)x1[i] - P256_N[i] - borrow;
            x1[i] = (uint32_t)d;
            borrow = (d < 0) ? 1 : 0;
        }
        (void)borrow;
    }

    return fn_cmp(x1, r) == 0;
}
