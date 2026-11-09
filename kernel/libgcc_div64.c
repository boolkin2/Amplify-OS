// ============================================================================
// libgcc_div64.c - быстрые реализации 64-битного деления для freestanding.
// ----------------------------------------------------------------------------
// На 32-битном i386 GCC превращает (uint64_t / uint32_t) и тому подобное в
// вызов __udivdi3 / __divdi3 / __umoddi3 / __moddi3. Без libgcc.a (мы
// линкуемся nostdlib) этих символов нет — линковщик ругается.
//
// Реализация трёхуровневая:
//   1) hi == 0           -> одно 32-bit деление (lo / den)
//   2) hi < den          -> один inline `divl` (hi:lo / den), тривиально.
//   3) hi >= den         -> два `divl`'а: сначала hi/den, потом (hi_rem):lo/den.
// А если делитель сам 64-битный (редкость в нашем коде) — fallback на
// школьный long-division по 64 итерациям.
//
// Для аудио-ресемплинга и таймеров типичный кейс — den <= 32 бит, и обычно
// hi == 0 или hi маленький. То есть мы попадаем в путь 1/2 — один divl.
// ============================================================================

#include <stdint.h>

// ---- бит-за-битом fallback (только для 64-bit делителей) ----
static uint64_t udivmod64_slow(uint64_t num, uint64_t den, uint64_t* rem_out) {
    uint64_t quot = 0;
    uint64_t rem  = 0;
    if (den == 0) {
        if (rem_out) *rem_out = 0;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        rem <<= 1;
        rem |= (num >> i) & 1ULL;
        if (rem >= den) {
            rem  -= den;
            quot |= 1ULL << i;
        }
    }
    if (rem_out) *rem_out = rem;
    return quot;
}

// ---- быстрый путь: 64 / 32 -> 64, через инструкцию divl ----
static inline uint64_t udivmod64_by32(uint64_t num, uint32_t den, uint32_t* rem_out) {
    uint32_t hi = (uint32_t)(num >> 32);
    uint32_t lo = (uint32_t)num;
    uint32_t qhi, qlo, rem;

    if (den == 0) {
        if (rem_out) *rem_out = 0;
        return 0;
    }

    if (hi == 0) {
        qhi = 0;
        qlo = lo / den;
        rem = lo % den;
    } else if (hi < den) {
        qhi = 0;
        __asm__ volatile (
            "divl %4"
            : "=a"(qlo), "=d"(rem)
            : "a"(lo), "d"(hi), "r"(den)
            : "cc"
        );
    } else {
        uint32_t hi_rem;
        __asm__ volatile (
            "divl %4"
            : "=a"(qhi), "=d"(hi_rem)
            : "a"(hi), "d"(0u), "r"(den)
            : "cc"
        );
        __asm__ volatile (
            "divl %4"
            : "=a"(qlo), "=d"(rem)
            : "a"(lo), "d"(hi_rem), "r"(den)
            : "cc"
        );
    }

    if (rem_out) *rem_out = rem;
    return ((uint64_t)qhi << 32) | qlo;
}

uint64_t __udivdi3(uint64_t a, uint64_t b) {
    uint32_t b_hi = (uint32_t)(b >> 32);
    if (b_hi == 0) {
        return udivmod64_by32(a, (uint32_t)b, 0);
    }
    return udivmod64_slow(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b) {
    uint32_t b_hi = (uint32_t)(b >> 32);
    if (b_hi == 0) {
        uint32_t r = 0;
        udivmod64_by32(a, (uint32_t)b, &r);
        return (uint64_t)r;
    }
    uint64_t r = 0;
    udivmod64_slow(a, b, &r);
    return r;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = 0;
    uint64_t ua = (uint64_t)a;
    uint64_t ub = (uint64_t)b;
    if (a < 0) { ua = (uint64_t)(-a); neg ^= 1; }
    if (b < 0) { ub = (uint64_t)(-b); neg ^= 1; }

    uint64_t q;
    if ((uint32_t)(ub >> 32) == 0) {
        q = udivmod64_by32(ua, (uint32_t)ub, 0);
    } else {
        q = udivmod64_slow(ua, ub, 0);
    }
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b) {
    int neg = (a < 0);
    uint64_t ua = (uint64_t)(a < 0 ? -a : a);
    uint64_t ub = (uint64_t)(b < 0 ? -b : b);

    uint64_t r;
    if ((uint32_t)(ub >> 32) == 0) {
        uint32_t r32 = 0;
        udivmod64_by32(ua, (uint32_t)ub, &r32);
        r = (uint64_t)r32;
    } else {
        r = 0;
        udivmod64_slow(ua, ub, &r);
    }
    return neg ? -(int64_t)r : (int64_t)r;
}

// __divmoddi4 — комбинированное знаковое деление+остаток (rem* — out param).
int64_t __divmoddi4(int64_t a, int64_t b, int64_t* rem) {
    int64_t q = __divdi3(a, b);
    if (rem) *rem = a - q * b;
    return q;
}

// __udivmoddi4 — то же без знака.
uint64_t __udivmoddi4(uint64_t a, uint64_t b, uint64_t* rem) {
    uint64_t q = __udivdi3(a, b);
    if (rem) *rem = a - q * b;
    return q;
}
