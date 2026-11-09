// ============================================================================
// crypto_bn.c — 2048-битная арифметика для RSA (этап 47.2).
// ----------------------------------------------------------------------------
// Простой layout: 64 × 32-bit little-endian limbs (v[0]=LSW).  Подход через
// 32-bit limbs (а не 64-bit) проще для портабильности и нагляднее в умножении
// (64x64 → 128-bit не нужен; есть только 32x32 → 64-bit, который GCC отлично
// эмитит).
//
// modexp — bit-by-bit модульная редукция: на каждый бит экспоненты делаем
// squaring + (если бит 1) mul, потом mod n bit-by-bit.  Это медленно
// (~50 ms на TCG для RSA-2048 с e=65537), но единственный путь без Montgomery
// reduction (которая на ~100 строк сложнее, но в ~5× быстрее).  Так как
// RSA-verify происходит 1-2 раза за TLS handshake, скорость не критична.
// ============================================================================
#include "crypto.h"

void crypto_bn_zero(crypto_bn_t* x) {
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) x->v[i] = 0;
}

void crypto_bn_copy(crypto_bn_t* d, const crypto_bn_t* s) {
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) d->v[i] = s->v[i];
}

bool crypto_bn_is_zero(const crypto_bn_t* x) {
    uint32_t acc = 0;
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) acc |= x->v[i];
    return acc == 0;
}

int crypto_bn_cmp(const crypto_bn_t* a, const crypto_bn_t* b) {
    // From MSW down.
    for (int i = CRYPTO_BN_LIMBS - 1; i >= 0; i--) {
        if (a->v[i] > b->v[i]) return  1;
        if (a->v[i] < b->v[i]) return -1;
    }
    return 0;
}

// Big-endian byte array → bignum.  Старшие байты могут быть нулевыми
// (len < BYTES OK).
void crypto_bn_from_bytes_be(crypto_bn_t* x, const uint8_t* in, size_t len) {
    crypto_bn_zero(x);
    if (len > CRYPTO_BN_BYTES) {
        // Игнорим старшие байты, не помещающиеся в 2048 бит.
        in += (len - CRYPTO_BN_BYTES);
        len = CRYPTO_BN_BYTES;
    }
    // Pad input до 2048 бит: размещаем последний байт в v[0]&0xff, и так далее.
    // То есть проходим with byte-index относительно LSB.
    for (size_t i = 0; i < len; i++) {
        size_t byte_pos_from_lsb = len - 1 - i;
        uint32_t shift = (byte_pos_from_lsb % 4) * 8;
        x->v[byte_pos_from_lsb / 4] |= ((uint32_t)in[i]) << shift;
    }
}

void crypto_bn_to_bytes_be(const crypto_bn_t* x, uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t byte_pos_from_lsb = len - 1 - i;
        if (byte_pos_from_lsb >= CRYPTO_BN_BYTES) {
            out[i] = 0;
        } else {
            uint32_t shift = (byte_pos_from_lsb % 4) * 8;
            out[i] = (uint8_t)(x->v[byte_pos_from_lsb / 4] >> shift);
        }
    }
}

uint32_t crypto_bn_add(crypto_bn_t* out, const crypto_bn_t* a, const crypto_bn_t* b) {
    uint64_t carry = 0;
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) {
        uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry;
        out->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}

uint32_t crypto_bn_sub(crypto_bn_t* out, const crypto_bn_t* a, const crypto_bn_t* b) {
    int64_t borrow = 0;
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) {
        int64_t d = (int64_t)a->v[i] - b->v[i] - borrow;
        out->v[i] = (uint32_t)d;
        borrow   = (d < 0) ? 1 : 0;
    }
    return (uint32_t)borrow;
}

// 2048×2048 → 4096-bit mul.  Школьное умножение, O(N²).
// Используется только внутри modexp; out — temp буфер 128 limbs (=2N).
typedef struct { uint32_t v[2 * CRYPTO_BN_LIMBS]; } bn_double_t;

static void bn_mul_full(bn_double_t* out, const crypto_bn_t* a, const crypto_bn_t* b) {
    for (int i = 0; i < 2 * CRYPTO_BN_LIMBS; i++) out->v[i] = 0;
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < CRYPTO_BN_LIMBS; j++) {
            uint64_t prod = (uint64_t)a->v[i] * b->v[j] + out->v[i + j] + carry;
            out->v[i + j] = (uint32_t)prod;
            carry         = prod >> 32;
        }
        out->v[i + CRYPTO_BN_LIMBS] = (uint32_t)carry;
    }
}

// Bit-i (i = 0..4095) of bn_double_t.
static inline uint32_t bn_double_bit(const bn_double_t* x, int i) {
    return (x->v[i / 32] >> (i % 32)) & 1;
}

// (r, carry) := (r * 2) + carry_in_lsb.  Возвращает carry-out из MSB.
static uint32_t bn_shl1_with_carry(crypto_bn_t* r, uint32_t carry_in) {
    uint32_t carry = carry_in & 1;
    for (int i = 0; i < CRYPTO_BN_LIMBS; i++) {
        uint32_t newc = r->v[i] >> 31;
        r->v[i] = (r->v[i] << 1) | carry;
        carry = newc;
    }
    return carry;
}

// out = a mod n, где a — 2N (= 4096-bit) bn_double_t, n — 2048-bit bn_t.
// Bitwise restoration division: r = 0; for each bit i from MSB to LSB:
//   r = r*2 + bit_i; if (r >= n) r -= n;
// Завершается за 4096 итераций.
static void bn_mod_2N(crypto_bn_t* out, const bn_double_t* a, const crypto_bn_t* n) {
    crypto_bn_t r; crypto_bn_zero(&r);
    for (int i = 2 * CRYPTO_BN_LIMBS * 32 - 1; i >= 0; i--) {
        uint32_t bit = bn_double_bit(a, i);
        uint32_t top_carry = bn_shl1_with_carry(&r, bit);
        // top_carry означает, что r >= 2^2048, что заведомо > n.  В этом случае
        // r фактически = (r' + 2^2048), и (r' + 2^2048) - n = r' + (2^2048 - n).
        // Простейший приём: вычислить r' - n; если top_carry — то r' - n
        // (без borrow) есть результат.  Но реализуем чуть иначе через if'ы.
        if (top_carry) {
            // r >= 2^2048 > n: безусловно вычитаем.
            crypto_bn_sub(&r, &r, n);
        } else if (crypto_bn_cmp(&r, n) >= 0) {
            crypto_bn_sub(&r, &r, n);
        }
    }
    crypto_bn_copy(out, &r);
}

// Modular multiplication: out = (a * b) mod n.
static void bn_mul_mod(crypto_bn_t* out, const crypto_bn_t* a,
                       const crypto_bn_t* b, const crypto_bn_t* n) {
    bn_double_t prod;
    bn_mul_full(&prod, a, b);
    bn_mod_2N(out, &prod, n);
}

// Modular exponentiation: out = base^exp mod n.
// exp — 32-bit (для RSA-verify хватает: e=65537=0x10001, или e=3).
// Square-and-multiply, MSB first.
void crypto_bn_modexp(crypto_bn_t* out, const crypto_bn_t* base,
                      uint32_t exp, const crypto_bn_t* n) {
    crypto_bn_t r;
    crypto_bn_zero(&r);
    r.v[0] = 1;          // r = 1

    crypto_bn_t b;       // working base
    crypto_bn_copy(&b, base);

    // Уже redused base ниже n (если случайно нет — bn_mod в loop справится сам).

    // Найдём MSB exp'а.
    if (exp == 0) {
        crypto_bn_copy(out, &r);
        return;
    }
    int msb = 31;
    while (msb > 0 && ((exp >> msb) & 1) == 0) msb--;

    for (int i = msb; i >= 0; i--) {
        // r = r * r mod n
        bn_mul_mod(&r, &r, &r, n);
        if ((exp >> i) & 1) {
            // r = r * b mod n
            bn_mul_mod(&r, &r, &b, n);
        }
    }
    crypto_bn_copy(out, &r);
}
