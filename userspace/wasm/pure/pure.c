// ============================================================================
// pure.c — проверочный модуль для интерпретатора WebAssembly (этап 51.1).
// ----------------------------------------------------------------------------
// «Чистые» функции: ничего не импортируют (окружения WASI ещё нет — оно в
// 51.2), получают числа и возвращают число. Каждая нагружает свою часть
// движка: вызовы и рекурсию, 64-битную арифметику, память и статические
// данные, косвенные вызовы, числа с плавающей точкой, преобразования.
//
// Один и тот же файл собирается дважды:
//   clang --target=wasm32 -nostdlib  -> модуль для движка (три варианта: -O0,
//                                       -O2, -O2 со всеми расширениями 2.0);
//   gcc (обычная программа хоста)    -> печатает «функция(аргументы) = значение».
// Вывод хоста — эталон: тест вызывает те же функции через `rune call` и
// сравнивает строки. Значит, в функциях нет ничего, что стандарт C оставляет
// на усмотрение компилятора: знаковое переполнение, сдвиг на ширину типа,
// преобразование дробного вне диапазона, порядок вычисления.
// Дробные результаты сравниваются ПО БИТАМ — операции + - * / sqrt в IEEE 754
// округляются однозначно, а «слитного умножения-сложения» нет ни там, ни там
// (хост собирается с -ffp-contract=off).
// ============================================================================
#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
typedef unsigned long size_t;
#else
#define EXPORT(name)
#include <stdio.h>
#include <string.h>
#endif

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned char      u8;

#ifdef __wasm__
// Без libc компилятор всё равно вправе позвать memcpy/memset/memmove (а в
// варианте с расширениями заменяет их командами memory.copy / memory.fill).
// no_builtin — чтобы он не «узнал» в этих циклах сами эти функции и не
// заменил тело вызовом самого себя.
__attribute__((no_builtin)) void* memcpy(void* d, const void* s, size_t n) {
    u8* a = (u8*)d; const u8* b = (const u8*)s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
__attribute__((no_builtin)) void* memset(void* d, int c, size_t n) {
    u8* a = (u8*)d;
    for (size_t i = 0; i < n; i++) a[i] = (u8)c;
    return d;
}
__attribute__((no_builtin)) void* memmove(void* d, const void* s, size_t n) {
    u8* a = (u8*)d; const u8* b = (const u8*)s;
    if (a < b) for (size_t i = 0; i < n; i++) a[i] = b[i];
    else for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1];
    return d;
}
#endif

// ---- вызовы и рекурсия ---------------------------------------------------------------
EXPORT(fib) int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// ---- 64-битная арифметика ------------------------------------------------------------
EXPORT(sum64) i64 sum64(int n) {
    u64 acc = 0x9E3779B97F4A7C15ull;
    for (u64 i = 1; i <= (u64)n; i++) {
        acc += i * i * i;
        acc ^= acc >> 29;
        acc = (acc << 13) | (acc >> 51);
    }
    return (i64)acc;
}

// Деление и остаток со знаком и без — у WebAssembly для каждого своя команда.
EXPORT(divmix) i64 divmix(i64 a, i64 b) {
    u64 ua = (u64)a, ub = (u64)b;
    i64 q = a / b, r = a % b;
    u64 uq = ua / ub, ur = ua % ub;
    return (i64)((u64)q * 1000003u + (u64)r * 10007u + uq * 101u + ur);
}

EXPORT(mul64) i64 mul64(i64 a, i64 b) {
    u64 x = (u64)a, y = (u64)b, acc = 0;
    for (int i = 0; i < 64; i++) {
        acc += x * y;
        x = (x << (i & 31)) | (x >> (64 - (i & 31) - 1) >> 1);      // сдвиг строго меньше 64
        y ^= (u64)((i64)y >> 3);                                    // арифметический сдвиг
        acc ^= acc >> 17;
    }
    return (i64)acc;
}

// ---- циклы и ветвления ---------------------------------------------------------------
EXPORT(collatz) int collatz(int n) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        u64 x = (u64)i;
        int steps = 0;
        while (x != 1) { x = (x & 1) ? 3 * x + 1 : x / 2; steps++; }
        if (steps > best) best = steps;
    }
    return best;
}

// ---- память и статические данные -----------------------------------------------------
static u8 sieve[1 << 16];

EXPORT(primes) int primes(int n) {
    if (n > (int)sizeof(sieve)) n = (int)sizeof(sieve);
    for (int i = 0; i < n; i++) sieve[i] = 1;
    int count = 0;
    for (int i = 2; i < n; i++) {
        if (!sieve[i]) continue;
        count++;
        for (int k = i * 2; k < n; k += i) sieve[k] = 0;
    }
    return count;
}

static u32 crc_table[256];
static u8  crc_buf[4096];

EXPORT(crc32) int crc32(int n) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    for (u32 i = 0; i < sizeof(crc_buf); i++) crc_buf[i] = (u8)(i * 7 + 3);
    u32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) crc = crc_table[(crc ^ crc_buf[i & 4095]) & 0xFF] ^ (crc >> 8);
    return (int)(crc ^ 0xFFFFFFFFu);
}

static int sort_buf[4096];

static void quick(int* a, int lo, int hi) {
    while (lo < hi) {
        int p = a[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quick(a, lo, j); lo = i; }
        else { quick(a, i, hi); hi = j; }
    }
}

EXPORT(sort) int sort(int n) {
    if (n > 4096) n = 4096;
    u32 seed = 12345;
    for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; sort_buf[i] = (int)(seed >> 8) - 4000000; }
    quick(sort_buf, 0, n - 1);
    u32 sum = 0;
    for (int i = 0; i < n; i++) {
        if (i && sort_buf[i - 1] > sort_buf[i]) return -1;         // не отсортировано
        sum = sum * 31u + (u32)sort_buf[i];
    }
    return (int)sum;
}

// Копирование и заполнение кусками переменной длины, в том числе внахлёст.
static u8 mem_a[2048], mem_b[2048];

EXPORT(memops) int memops(int n) {
    for (u32 i = 0; i < sizeof(mem_a); i++) { mem_a[i] = (u8)(i * 13 + 1); mem_b[i] = 0; }
    u32 sum = 0;
    for (int i = 1; i <= n; i++) {
        u32 len = (u32)(i * 7) % 900 + 1, from = (u32)(i * 31) % 1000, to = (u32)(i * 17) % 1000;
        __builtin_memcpy(mem_b + to, mem_a + from, len);
        __builtin_memmove(mem_a + to, mem_a + from, len);           // области пересекаются
        __builtin_memset(mem_b + from, i, len / 3);
        sum = sum * 33u + mem_a[(to + len / 2) & 2047] + mem_b[(from + len) & 2047];
    }
    for (u32 i = 0; i < sizeof(mem_a); i++) sum = sum * 31u + mem_a[i] + (u32)mem_b[i] * 3u;
    return (int)sum;
}

// Байтовая работа: числа в строку, строка в свёртку.
static char str_buf[32];

EXPORT(strwork) int strwork(int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) {
        u32 v = (u32)i * 2654435761u;
        int len = 0;
        do { str_buf[len++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (int k = 0; k < len / 2; k++) { char t = str_buf[k]; str_buf[k] = str_buf[len - 1 - k]; str_buf[len - 1 - k] = t; }
        for (int k = 0; k < len; k++) h = (h ^ (u8)str_buf[k]) * 16777619u;
    }
    return (int)h;
}

// Изменяемые глобальные с начальными значениями — они лежат в сегменте данных.
static int g_counter = 12345;
static i64 g_wide = 0x0123456789ABCDEFll;

EXPORT(globals) int globals(int n) {
    for (int i = 0; i < n; i++) { g_counter += i; g_wide = (i64)((u64)g_wide * 3u + (u64)g_counter); }
    return g_counter ^ (int)(u32)((u64)g_wide >> 20);
}

// ---- косвенные вызовы ----------------------------------------------------------------
static int op_add(int a, int b) { return (int)((u32)a + (u32)b); }
static int op_xor(int a, int b) { return a ^ b; }
static int op_mul(int a, int b) { return (int)((u32)a * (u32)b + 1u); }
static int op_rot(int a, int b) { u32 x = (u32)a; int s = b & 31; return (int)((x << s) | (x >> ((32 - s) & 31))); }
static int (*const ops[4])(int, int) = { op_add, op_xor, op_mul, op_rot };

EXPORT(dispatch) int dispatch(int n) {
    int acc = 7;
    for (int i = 0; i < n; i++) acc = ops[(u32)(acc ^ i) & 3](acc, i + 1);
    return acc;
}

// ---- биты ----------------------------------------------------------------------------
EXPORT(bits) int bits(int n) {
    u32 acc = 0;
    for (int i = 1; i <= n; i++) {
        u32 v = (u32)i * 2246822519u;
        u64 w = (u64)v * 0x100000001B3ull + (u64)i;
        acc += (u32)__builtin_clz(v | 1) + (u32)__builtin_ctz(v | 0x80000000u) + (u32)__builtin_popcount(v);
        acc += (u32)__builtin_clzll(w | 1) + (u32)__builtin_ctzll(w | (1ull << 63)) + (u32)__builtin_popcountll(w);
        acc = (acc << 5) | (acc >> 27);
        acc ^= (u32)(signed char)(v >> 3) + (u32)(short)(v >> 7);  // расширение знака 8 и 16 бит
    }
    return (int)acc;
}

// ---- числа с плавающей точкой --------------------------------------------------------
EXPORT(mandel) int mandel(int n) {
    int total = 0;
    for (int py = 0; py < n; py++) {
        for (int px = 0; px < n; px++) {
            double cx = -2.0 + 3.0 * px / n, cy = -1.5 + 3.0 * py / n;
            double x = 0, y = 0;
            int it = 0;
            while (it < 200 && x * x + y * y <= 4.0) { double t = x * x - y * y + cx; y = 2.0 * x * y + cy; x = t; it++; }
            total += it;
        }
    }
    return total;
}

EXPORT(series) double series(int n) {
    double s = 0;
    for (int i = 1; i <= n; i++) s += 1.0 / ((double)i * (double)i) + __builtin_sqrt((double)i) * 1e-9;
    return s;
}

EXPORT(fmix) float fmix(int n) {
    float x = 1.0f, acc = 0.0f;
    for (int i = 0; i < n; i++) {
        x = x * 1.0001f + 0.25f / (float)(i + 1);
        acc += __builtin_sqrtf(x) - (float)(i & 7) * 0.125f;
        if (acc > 1000.0f) acc = -acc * 0.5f;
    }
    return acc;
}

// Преобразования между целыми и дробными (всегда в пределах целого типа).
EXPORT(conv) int conv(int n) {
    u32 acc = 0;
    for (int i = 0; i < n; i++) {
        double d = (double)i * 1234.5678 - 3000000.0;
        float  f = (float)i * 0.37f + 0.5f;
        acc += (u32)(int)d;
        acc ^= (u32)(i64)(d * 1000.0);
        acc += (u32)f + (u32)(u64)((double)f * 4096.0);
        acc = acc * 31u + (u32)(int)((float)(int)(acc >> 8) * 0.5f);
        acc += (u32)(i64)(double)(u64)acc + (u32)(float)(u32)i;
    }
    return (int)acc;
}

// ---- то, что должно быть остановлено движком (эталона на хосте у них нет) ------------
static volatile int g_spin;
EXPORT(forever) int forever(int n) { for (;;) g_spin += n; return 0; }
EXPORT(crash)   int crash(int n)   { if (n) __builtin_trap(); return 7; }
EXPORT(poke)    int poke(int addr) { *(volatile int*)(unsigned long)(u32)addr = 1; return 1; }
EXPORT(divzero) int divzero(int a, int b) { return a / b; }

#ifndef __wasm__
int main(void) {
    printf("fib(24) = %d\n", fib(24));
    printf("sum64(100000) = %lld\n", sum64(100000));
    printf("divmix(-1234567890123, 97) = %lld\n", divmix(-1234567890123ll, 97));
    printf("divmix(9007199254740993, -3) = %lld\n", divmix(9007199254740993ll, -3));
    printf("mul64(123456789012345, -987654321) = %lld\n", mul64(123456789012345ll, -987654321ll));
    printf("collatz(3000) = %d\n", collatz(3000));
    printf("primes(60000) = %d\n", primes(60000));
    printf("crc32(50000) = %d\n", crc32(50000));
    printf("sort(4000) = %d\n", sort(4000));
    printf("memops(300) = %d\n", memops(300));
    printf("strwork(2000) = %d\n", strwork(2000));
    printf("globals(77) = %d\n", globals(77));
    printf("dispatch(1000) = %d\n", dispatch(1000));
    printf("bits(20000) = %d\n", bits(20000));
    printf("mandel(48) = %d\n", mandel(48));
    { union { double d; unsigned long long u; } c; c.d = series(100000); printf("series(100000) = f64:0x%016llx\n", c.u); }
    { union { float f; unsigned int u; } c; c.f = fmix(20000); printf("fmix(20000) = f32:0x%08x\n", c.u); }
    printf("conv(5000) = %d\n", conv(5000));
    return 0;
}
#endif
