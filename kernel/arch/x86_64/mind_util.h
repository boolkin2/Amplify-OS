// ============================================================================
// mind_util.h — базовые утилиты AMPLIFY MIND (этап 49): память, строки,
// математика без libm, преобразования fp16.
// ----------------------------------------------------------------------------
// Ядро собрано freestanding: нет memcpy/expf/sinf. Для инференса нужны:
//   * expf — softmax и SiLU (float, ~1 ulp);
//   * sqrt — RMSNorm (инструкция sqrtss/sqrtsd);
//   * exp/log/sin/cos в double — таблицы RoPE строятся один раз при загрузке
//     модели, поэтому считаем их с запасом точности (углы pos*freq доходят
//     до тысяч радиан, float-редукция по 2*pi там уже заметно врёт);
//   * fp16 <-> fp32 — масштабы блоков Q8_0/Q4_0 и веса F16 хранятся в half.
// Всё static inline: функции зовутся и на AP (smp_pool), где нельзя ничего,
// кроме чистых вычислений.
// ============================================================================
#ifndef AOS_X86_64_MIND_UTIL_H
#define AOS_X86_64_MIND_UTIL_H

#include <stdint.h>
#include <stdbool.h>

// ---- память / строки --------------------------------------------------------
// rep movsb/stosb: на современных CPU (ERMSB) это самый быстрый способ для
// больших блоков и заодно гарантирует, что GCC не заменит цикл вызовом memcpy
// (в ядре такого символа нет — линковка бы упала).
static inline void m_memcpy(void* d, const void* s, uint64_t n) {
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline void m_memset(void* d, int c, uint64_t n) {
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
}
static inline uint64_t m_strlen(const char* s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}
static inline bool m_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static inline bool m_starts(const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return false; }
    return true;
}
// Сравнение строки «указатель+длина» (формат GGUF) с C-строкой.
static inline bool m_lstreq(const uint8_t* p, uint64_t len, const char* s) {
    for (uint64_t i = 0; i < len; i++) {
        if (!s[i] || (uint8_t)s[i] != p[i]) return false;
    }
    return s[len] == 0;
}
// Безопасная склейка в буфер фиксированного размера.
static inline void m_append(char* dst, uint64_t cap, const char* src) {
    uint64_t n = m_strlen(dst);
    while (*src && n + 1 < cap) dst[n++] = *src++;
    dst[n] = 0;
}
static inline void m_append_dec(char* dst, uint64_t cap, int64_t v) {
    char t[24]; int i = 0; bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u && i < 22);
    if (neg) t[i++] = '-';
    char o[24]; int k = 0;
    while (i) o[k++] = t[--i];
    o[k] = 0;
    m_append(dst, cap, o);
}
// Число с одним знаком после запятой (x10 уже умножено вызывающим).
static inline void m_append_dec1(char* dst, uint64_t cap, int64_t x10) {
    m_append_dec(dst, cap, x10 / 10);
    char t[3] = { '.', (char)('0' + (int)((x10 < 0 ? -x10 : x10) % 10)), 0 };
    m_append(dst, cap, t);
}

// ---- float ------------------------------------------------------------------
static inline float m_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline double m_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline float m_fabsf(float x) {
    union { float f; uint32_t u; } v = { x };
    v.u &= 0x7FFFFFFFu;
    return v.f;
}

// expf: x = n*ln2 + r, |r| <= ln2/2; e^r — ряд Тейлора до r^7 (ошибка ~5e-9
// относительная), 2^n — прямой сборкой экспоненты float.
static inline float m_expf(float x) {
    if (x > 88.0f) x = 88.0f;               // e^88 ~ 1.6e38 — ещё конечно
    if (x < -87.0f) return 0.0f;
    float fn = x * 1.44269504088896341f;
    int n = (int)(fn + (fn >= 0.0f ? 0.5f : -0.5f));
    float r = x - (float)n * 0.693145751953125f;
    r = r - (float)n * 1.428606765330187045e-06f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666666666f + r * (0.041666666666f +
              r * (0.0083333333333f + r * (0.0013888888889f + r * 0.00019841269841f))))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}

// ---- double (таблицы RoPE) ---------------------------------------------------
static inline double m_exp(double x) {
    if (x > 700.0) x = 700.0;
    if (x < -700.0) return 0.0;
    double fn = x * 1.4426950408889634;
    int64_t n = (int64_t)(fn + (fn >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)n * 6.93147180369123816490e-01;
    r = r - (double)n * 1.90821492927058770002e-10;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 16; k++) { term *= r / (double)k; sum += term; }
    union { uint64_t u; double d; } s;
    s.u = (uint64_t)(n + 1023) << 52;
    return sum * s.d;
}
static inline double m_log(double x) {
    if (x <= 0.0) return -1e300;
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;   // m в [1,2)
    double m = v.d;
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    // log(m) = 2*atanh(s), s = (m-1)/(m+1), |s| < 0.172
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, t = s, sum = 0.0;
    for (int k = 1; k < 40; k += 2) { sum += t / (double)k; t *= s2; }
    return 2.0 * sum + (double)e * 0.69314718055994530942;
}
// sin/cos с редукцией по pi/2 (Cody–Waite, две части константы).
static inline void m_sincos(double x, double* so, double* co) {
    const double pio2_hi = 1.57079632679489655800e+00;
    const double pio2_lo = 6.12323399573676603587e-17;
    double kf = x * 0.63661977236758134308;
    int64_t k = (int64_t)(kf + (kf >= 0.0 ? 0.5 : -0.5));
    double r = (x - (double)k * pio2_hi) - (double)k * pio2_lo;
    double r2 = r * r;
    double s = r, c = 1.0, ts = r, tc = 1.0;
    for (int i = 1; i <= 10; i++) {
        ts *= -r2 / (double)((2 * i) * (2 * i + 1));
        tc *= -r2 / (double)((2 * i - 1) * (2 * i));
        s += ts; c += tc;
    }
    switch ((int)(k & 3)) {
        case 0:  *so =  s; *co =  c; break;
        case 1:  *so =  c; *co = -s; break;
        case 2:  *so = -s; *co = -c; break;
        default: *so = -c; *co =  s; break;
    }
}

// ---- half precision ----------------------------------------------------------
static inline float m_fp16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    union { uint32_t u; float f; } o;
    if (exp == 0) {
        if (man == 0) { o.u = sign; return o.f; }
        // денормал: нормализуем мантиссу
        int e = -1;
        do { man <<= 1; e++; } while (!(man & 0x400u));
        man &= 0x3FFu;
        o.u = sign | ((uint32_t)(127 - 15 - e) << 23) | (man << 13);
        return o.f;
    }
    if (exp == 31) { o.u = sign | 0x7F800000u | (man << 13); return o.f; }
    o.u = sign | ((exp + 112u) << 23) | (man << 13);
    return o.f;
}
// fp32 -> fp16 с округлением к ближайшему чётному (как F16C/numpy).
static inline uint16_t m_f32_to_fp16(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t sign = (v.u >> 16) & 0x8000u;
    int32_t exp = (int32_t)((v.u >> 23) & 0xFF) - 127 + 15;
    uint32_t man = v.u & 0x7FFFFFu;
    if (((v.u >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00u | (man ? 0x200u : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        int shift = 14 - exp;
        uint32_t half = man >> shift;
        uint32_t rem = man & ((1u << shift) - 1u);
        uint32_t mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u))) half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = sign | ((uint32_t)exp << 10) | (man >> 13);
    uint32_t rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;
    return (uint16_t)half;
}
static inline float m_bf16_to_f32(uint16_t b) {
    union { uint32_t u; float f; } o;
    o.u = (uint32_t)b << 16;
    return o.f;
}

#endif
