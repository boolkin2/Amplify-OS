// ============================================================================
// stb_config.h - Конфигурация STB-библиотек для ядра MyOS
// ============================================================================
// Переопределения стандартных функций (math, stdlib, string, assert)
// чтобы STB-заголовки компилировались в ядре без libc/libm
// ============================================================================

#ifndef STB_CONFIG_H
#define STB_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Подключаем kernel heap для malloc/free
// ============================================================================

#include "../memory/heap.h"
#include "../memory/memory.h"

// ============================================================================
// Переопределение malloc/free для STB
// ============================================================================

#define STBTT_malloc(x, u)  ((void)(u), kmalloc(x))
#define STBTT_free(x, u)    ((void)(u), kfree(x))

// ============================================================================
// Переопределение assert (заглушка — в ядре нет assert.h)
// ============================================================================

#define STBTT_assert(x)  ((void)0)

// ============================================================================
// Переопределение string-функций (уже есть в ядре)
// ============================================================================

#define STBTT_strlen(x)    strlen_impl(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset

// Простая реализация strlen для STB
static inline size_t strlen_impl(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

// ============================================================================
// Математические функции для ядра (замена libm)
// ============================================================================

// floor — округление вниз
static inline double stbtt_floor(double x) {
    if (x >= 0.0) return (double)(int64_t)x;
    int64_t i = (int64_t)x;
    return (x == (double)i) ? x : (double)(i - 1);
}

// ceil — округление вверх
static inline double stbtt_ceil(double x) {
    if (x >= 0.0) {
        int64_t i = (int64_t)x;
        return (x == (double)i) ? x : (double)(i + 1);
    }
    return (double)(int64_t)x;
}

// fabs — абсолютное значение
static inline double stbtt_fabs(double x) {
    return x < 0.0 ? -x : x;
}

// sqrt — квадратный корень (метод Ньютона)
static inline double stbtt_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double z = x;
    for (int i = 0; i < 16; i++) {
        z = 0.5 * (z + x / z);
    }
    return z;
}

// pow — возведение в степень (для целых показателей)
static inline double stbtt_pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (base <= 0.0) return 0.0;
    // Простой алгоритм для целых и дробных показателей
    double result = 1.0;
    int neg = 0;
    if (exp < 0.0) { neg = 1; exp = -exp; }
    // Целая часть
    int64_t int_exp = (int64_t)exp;
    double frac_exp = exp - (double)int_exp;
    for (int64_t i = 0; i < int_exp; i++) result *= base;
    // Дробная часть (приближение через ряд Тейлора)
    if (frac_exp > 0.001) {
        // ln(base) * frac_exp через e^(ln(base)*frac_exp)
        double ln = 0.0;
        double term = (base - 1.0) / (base + 1.0);
        double term2 = term * term;
        double acc = term;
        for (int i = 0; i < 20; i++) {
            ln += acc / (2 * i + 1);
            acc *= term2;
        }
        ln *= 2.0;
        double x2 = ln * frac_exp;
        // e^x через ряд Тейлора
        double ex = 1.0;
        double fac = 1.0;
        for (int i = 1; i <= 20; i++) {
            fac *= x2 / (double)i;
            ex += fac;
        }
        result *= ex;
    }
    return neg ? 1.0 / result : result;
}

// fmod — остаток от деления
static inline double stbtt_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double r = x / y;
    return x - (double)(int64_t)r * y;
}

// cos — косинус (ряд Тейлора)
static inline double stbtt_cos(double x) {
    // Нормализация в [-pi, pi]
    const double PI = 3.14159265358979323846;
    x = stbtt_fmod(x + PI, 2.0 * PI);
    if (x < 0.0) x += 2.0 * PI;
    x -= PI;
    // Ряд Тейлора: cos(x) = 1 - x^2/2! + x^4/4! - ...
    double result = 1.0;
    double term = 1.0;
    for (int i = 1; i <= 12; i++) {
        term *= -x * x / ((double)(2 * i - 1) * (double)(2 * i));
        result += term;
    }
    return result;
}

// acos — арккосинус (приближение)
static inline double stbtt_acos(double x) {
    if (x <= -1.0) return 3.14159265358979323846;
    if (x >= 1.0)  return 0.0;
    // acos(x) = atan2(sqrt(1-x^2), x)
    // Упрощённое приближение через ряд
    double sq = stbtt_sqrt(1.0 - x * x);
    // atan(sq/x) через приближение
    double a = sq / (1.0 + stbtt_fabs(x) + sq * 0.2855);
    if (x < 0.0) a = 3.14159265358979323846 - a;
    return a;
}

// Переопределения для STB
#define STBTT_ifloor(x)   ((int) stbtt_floor(x))
#define STBTT_iceil(x)    ((int) stbtt_ceil(x))
#define STBTT_sqrt(x)     stbtt_sqrt(x)
#define STBTT_pow(x, y)   stbtt_pow(x, y)
#define STBTT_fmod(x, y)  stbtt_fmod(x, y)
#define STBTT_cos(x)      stbtt_cos(x)
#define STBTT_acos(x)     stbtt_acos(x)
#define STBTT_fabs(x)     stbtt_fabs(x)

// ============================================================================
// Конфигурация для STB Image (PNG / JPEG декодеры)
// ============================================================================
// Отключаем всё, что требует libc/libm/threads
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_BMP
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS

// Перенаправляем аллокатор в ядерную кучу
#define STBI_MALLOC(sz)           kmalloc(sz)
#define STBI_FREE(p)              kfree(p)
#define STBI_REALLOC(p, newsz)    krealloc(p, newsz)

// Заглушка assert
#define STBI_ASSERT(x)            ((void)0)

#endif // STB_CONFIG_H