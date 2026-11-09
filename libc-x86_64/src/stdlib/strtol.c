// ============================================================================
// strtol.c — конверсия строка → число для всех базовых типов.
// ----------------------------------------------------------------------------
// atoi/atol/atoll/strtol/strtoll/strtoul/strtoull — все через одну ядрную
// функцию parse_signed/unsigned.
// ============================================================================
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

static int digit_of(int c, int base) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return (d < base) ? d : -1;
}

static unsigned long long parse_ull(const char* s, char** end,
                                    int base, int* neg) {
    const char* start = s;
    while (isspace((unsigned char)*s)) s++;

    *neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { *neg = 1; s++; }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        s++;
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    if (base < 2 || base > 36) {
        if (end) *end = (char*)start;
        return 0;
    }

    unsigned long long acc = 0;
    int any = 0;
    int d;
    while ((d = digit_of(*s, base)) >= 0) {
        acc = acc * (unsigned)base + (unsigned)d;
        s++;
        any = 1;
    }
    if (end) *end = (char*)(any ? s : start);
    return acc;
}

long strtol(const char* __restrict__ s, char** __restrict__ end, int base) {
    int neg;
    unsigned long long u = parse_ull(s, end, base, &neg);
    if (neg) {
        if (u > (unsigned long long)LONG_MAX + 1ULL) { errno = ERANGE; return LONG_MIN; }
        return -(long)u;
    }
    if (u > (unsigned long long)LONG_MAX) { errno = ERANGE; return LONG_MAX; }
    return (long)u;
}

long long strtoll(const char* __restrict__ s, char** __restrict__ end, int base) {
    int neg;
    unsigned long long u = parse_ull(s, end, base, &neg);
    if (neg) {
        if (u > (unsigned long long)LLONG_MAX + 1ULL) { errno = ERANGE; return LLONG_MIN; }
        return -(long long)u;
    }
    if (u > (unsigned long long)LLONG_MAX) { errno = ERANGE; return LLONG_MAX; }
    return (long long)u;
}

unsigned long strtoul(const char* __restrict__ s, char** __restrict__ end, int base) {
    int neg;
    unsigned long long u = parse_ull(s, end, base, &neg);
    if (neg) return (unsigned long)-(long long)u;
    if (u > (unsigned long long)ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return (unsigned long)u;
}

unsigned long long strtoull(const char* __restrict__ s, char** __restrict__ end, int base) {
    int neg;
    unsigned long long u = parse_ull(s, end, base, &neg);
    if (neg) return (unsigned long long)-(long long)u;
    return u;
}

int atoi(const char* s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char* s) {
    return strtol(s, NULL, 10);
}

long long atoll(const char* s) {
    return strtoll(s, NULL, 10);
}

// strtod — упрощённый, без INF/NaN/exp-парсера (хватит для 39-го уровня).
double strtod(const char* __restrict__ s, char** __restrict__ end) {
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }

    double acc = 0.0;
    while (isdigit((unsigned char)*s)) { acc = acc * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (isdigit((unsigned char)*s)) { acc += frac * (*s - '0'); frac *= 0.1; s++; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '+') s++;
        else if (*s == '-') { eneg = 1; s++; }
        int e = 0;
        while (isdigit((unsigned char)*s)) { e = e * 10 + (*s - '0'); s++; }
        double scale = 1.0;
        for (int i = 0; i < e; i++) scale *= 10.0;
        acc = eneg ? acc / scale : acc * scale;
    }
    if (end) *end = (char*)s;
    return neg ? -acc : acc;
}

double atof(const char* s) {
    return strtod(s, NULL);
}

int  abs (int x)        { return x < 0 ? -x : x; }
long labs(long x)       { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
