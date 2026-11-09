// ============================================================================
// calc.c — проверочная программа для движка WebAssembly AMPLIFY-OS (этап 51).
// ----------------------------------------------------------------------------
// Считает много разного и печатает строки «имя=значение». Эталон даёт ТОТ ЖЕ
// исходник, собранный обычным gcc на хосте: вывод обязан совпасть побайтно.
// Поэтому здесь только типы точной ширины (long в wasm32 — 32 бита, на хосте —
// 64) и ничего, что зависит от платформы: ни указателей в выводе, ни времени,
// ни long double.
// Что задевает в движке: целую арифметику i32/i64 со знаком и без, деление и
// остаток, сдвиги и вращения, clz/ctz/popcnt, плавающую точку f32/f64
// (сложение, деление, корень, округления, преобразования), вызов по указателю
// (call_indirect), switch (br_table), рекурсию, работу с памятью (в том числе
// групповые memory.copy / memory.fill в сборке с -mbulk-memory), статические
// данные, кучу.
// ============================================================================
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static uint32_t ackermann(uint32_t m, uint32_t n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static uint32_t crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}
static int cmp_u32_desc(const void* a, const void* b) { return -cmp_u32(a, b); }

static const char* weekday(int d) {
    switch (d) {
    case 0: return "mon"; case 1: return "tue"; case 2: return "wed"; case 3: return "thu";
    case 4: return "fri"; case 5: return "sat"; case 6: return "sun"; default: return "???";
    }
}

static uint32_t rotl32(uint32_t v, unsigned s) { return (v << (s & 31)) | (v >> ((32 - s) & 31)); }
static uint64_t rotr64(uint64_t v, unsigned s) { return (v >> (s & 63)) | (v << ((64 - s) & 63)); }

// volatile — чтобы компилятор не вычислил всё сам на этапе сборки: считать
// должен движок.
static volatile int32_t  vi = -1234567;
static volatile uint32_t vu = 3000000000u;
static volatile int64_t  vl = -9876543210123ll;
static volatile double   vd = 2.718281828459045;
static volatile float    vf = 3.1415927f;

int main(void) {
    printf("fib30=%u\n", fib(30));
    printf("ack2_3=%u ack3_5=%u\n", ackermann(2, 3), ackermann(3, 5));

    uint64_t fact = 1;
    for (uint64_t i = 2; i <= 20; i++) fact *= i;
    printf("fact20=%llu\n", (unsigned long long)fact);

    // Решето: простых меньше 200000.
    enum { N = 200000 };
    uint8_t* sieve = malloc(N);
    memset(sieve, 1, N);
    sieve[0] = sieve[1] = 0;
    for (uint32_t i = 2; i * i < N; i++)
        if (sieve[i]) for (uint32_t k = i * i; k < N; k += i) sieve[k] = 0;
    uint32_t primes = 0, last = 0;
    for (uint32_t i = 0; i < N; i++) if (sieve[i]) { primes++; last = i; }
    printf("primes=%u last=%u\n", primes, last);

    // Буфер из генератора, его контрольная сумма, копии внахлёст.
    uint8_t* buf = malloc(65536);
    for (uint32_t i = 0; i < 65536; i++) buf[i] = (uint8_t)(rng() >> 24);
    printf("crc=%08x\n", crc32(buf, 65536));
    memmove(buf + 100, buf, 40000);
    memcpy(buf + 50000, buf + 7, 9000);
    memset(buf + 61000, 0xA5, 3000);
    printf("crc_moved=%08x\n", crc32(buf, 65536));

    // Сортировка с функцией сравнения по указателю.
    uint32_t* arr = malloc(5000 * sizeof(uint32_t));
    for (int i = 0; i < 5000; i++) arr[i] = (uint32_t)rng();
    qsort(arr, 5000, sizeof(uint32_t), cmp_u32);
    uint32_t asc = arr[0] ^ arr[2499] ^ arr[4999];
    qsort(arr, 5000, sizeof(uint32_t), cmp_u32_desc);
    printf("sort=%08x %08x first=%u\n", asc, arr[0] ^ arr[2499] ^ arr[4999], arr[4999]);

    // Целые: знак, деление, остаток, сдвиги.
    int32_t i = vi; uint32_t u = vu; int64_t l = vl;
    printf("div=%d rem=%d udiv=%u urem=%u\n", i / 1000, i % 1000, u / 7u, u % 7u);
    printf("ldiv=%lld lrem=%lld\n", (long long)(l / 100003), (long long)(l % 100003));
    printf("shr=%d ushr=%u shl=%u\n", i >> 5, u >> 5, u << 3);
    printf("rot=%08x %016llx\n", rotl32(u, 13), (unsigned long long)rotr64((uint64_t)l, 29));
    printf("bits=%d %d %d %d %d\n", __builtin_popcount(u), __builtin_clz(u >> 9), __builtin_ctz(u),
           __builtin_popcountll((uint64_t)l), __builtin_clzll((uint64_t)l >> 20));
    printf("mul64=%llu\n", (unsigned long long)((uint64_t)u * (uint64_t)u + (uint64_t)l));
    printf("ext=%d %d %lld\n", (int8_t)(u >> 3), (int16_t)i, (long long)(int32_t)u);

    // Плавающая точка.
    double d = vd; float f = vf;
    double pi = 0;
    for (int k = 0; k < 200000; k++) pi += (k & 1 ? -4.0 : 4.0) / (2 * k + 1);
    float basel = 0;
    for (int k = 1; k <= 10000; k++) basel += 1.0f / ((float)k * (float)k);
    printf("pi=%.9f basel=%.6f\n", pi, (double)basel);
    printf("sqrt=%.12f %.6f\n", sqrt(d), (double)sqrtf(f));
    printf("round=%.1f %.1f %.1f %.1f %.1f\n", floor(-d), ceil(-d), trunc(-d), rint(2.5), rint(3.5));
    printf("conv=%d %u %lld %.3f %.3f\n", (int32_t)(d * -1000.0), (uint32_t)(f * 1e9f),
           (long long)(d * 1e15), (double)(float)l, (double)u);
    printf("minmax=%.3f %.3f %.3f\n", fmin(d, (double)f), fmax(d, (double)f), copysign(d, -1.0));
    printf("fabs=%.4f neg=%.4f cmp=%d%d%d\n", fabs(-d), -(double)f, d < f, d == d, d != d);

    for (int k = 0; k < 8; k++) printf("%s%s", weekday(k), k == 7 ? "\n" : " ");

    char text[64];
    snprintf(text, sizeof(text), "%s-%05d-%x", "rune", 42, 0xBEEF);
    printf("text=%s len=%u\n", text, (unsigned)strlen(text));

    free(arr); free(buf); free(sieve);
    return 0;
}
