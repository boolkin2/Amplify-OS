// ============================================================================
// string.c — полная реализация string.h (memcpy/memset/memmove/strXX).
// ----------------------------------------------------------------------------
// На 39-м этапе — простые byte-loops, без SSE/AVX оптимизаций. SSE можно
// включить в 40-м, когда GUI-приложениям станет нужен memcpy на десятки MB.
// ============================================================================
#include <string.h>
#include <stddef.h>
#include <stdint.h>

void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    unsigned char v = (unsigned char)c;
    while (n--) *p++ = v;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v) return (void*)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n--) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (*a == 0) return 0;
        a++; b++;
    }
    return 0;
}

static int tolow(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char* a, const char* b) {
    while (*a && tolow((unsigned char)*a) == tolow((unsigned char)*b)) { a++; b++; }
    return tolow((unsigned char)*a) - tolow((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    while (n--) {
        int ca = tolow((unsigned char)*a);
        int cb = tolow((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
        a++; b++;
    }
    return 0;
}

char* strcpy(char* __restrict__ dst, const char* __restrict__ src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* __restrict__ dst, const char* __restrict__ src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strcat(char* __restrict__ dst, const char* __restrict__ src) {
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* strncat(char* __restrict__ dst, const char* __restrict__ src, size_t n) {
    char* d = dst;
    while (*d) d++;
    size_t i;
    for (i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = 0;
    return dst;
}

char* strchr(const char* s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char*)s;
        s++;
    }
    return (ch == 0) ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
    char ch = (char)c;
    const char* last = NULL;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    if (ch == 0) return (char*)s;
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t i = 0;
    while (s[i]) {
        const char* a = accept;
        while (*a && *a != s[i]) a++;
        if (!*a) return i;
        i++;
    }
    return i;
}

size_t strcspn(const char* s, const char* reject) {
    size_t i = 0;
    while (s[i]) {
        const char* r = reject;
        while (*r) {
            if (*r == s[i]) return i;
            r++;
        }
        i++;
    }
    return i;
}

char* strpbrk(const char* s, const char* accept) {
    size_t n = strcspn(s, accept);
    return s[n] ? (char*)(s + n) : NULL;
}

char* strtok_r(char* __restrict__ s, const char* __restrict__ sep,
               char** __restrict__ saveptr) {
    if (!s) s = *saveptr;
    if (!s) return NULL;
    s += strspn(s, sep);
    if (!*s) { *saveptr = NULL; return NULL; }
    char* tok = s;
    s = strpbrk(tok, sep);
    if (!s) {
        *saveptr = NULL;
    } else {
        *s = 0;
        *saveptr = s + 1;
    }
    return tok;
}

static char* g_strtok_save = NULL;
char* strtok(char* __restrict__ s, const char* __restrict__ sep) {
    return strtok_r(s, sep, &g_strtok_save);
}

// strdup использует malloc — определяется в stdlib/malloc.c.
extern void* malloc(size_t);
char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* r = (char*)malloc(n);
    if (!r) return NULL;
    memcpy(r, s, n);
    return r;
}

char* strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* r = (char*)malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}

// strerror — табличка errno → строка.
static const char* const errstr[] = {
    [0]  = "Success",
    [1]  = "Operation not permitted",
    [2]  = "No such file or directory",
    [9]  = "Bad file descriptor",
    [11] = "Resource temporarily unavailable",
    [12] = "Out of memory",
    [13] = "Permission denied",
    [14] = "Bad address",
    [17] = "File exists",
    [22] = "Invalid argument",
    [38] = "Function not implemented",
};

char* strerror(int e) {
    static char buf[32];
    if (e >= 0 && e < (int)(sizeof(errstr)/sizeof(errstr[0])) && errstr[e])
        return (char*)errstr[e];
    // fallback: "Unknown error N" без printf, чтобы избежать рекурсии.
    static const char prefix[] = "Unknown error ";
    char* p = buf;
    for (const char* c = prefix; *c; c++) *p++ = *c;
    if (e < 0) { *p++ = '-'; e = -e; }
    char tmp[12]; int n = 0;
    if (e == 0) tmp[n++] = '0';
    else while (e > 0) { tmp[n++] = '0' + (e % 10); e /= 10; }
    while (n--) *p++ = tmp[n];
    *p = 0;
    return buf;
}
