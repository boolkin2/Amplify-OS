// ============================================================================
// qsort.c / bsearch / rand / env — общие утилиты stdlib.
// ----------------------------------------------------------------------------
// qsort — Lomuto-partition, рекурсивный (для маленьких массивов — switch
// на insertion sort). Этого хватит для всех 39-х пользователей; настоящий
// quicksort/introsort заложим в 44+.
// ============================================================================
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

static void swap_bytes(char* a, char* b, size_t sz) {
    while (sz--) {
        char t = *a; *a = *b; *b = t;
        a++; b++;
    }
}

static void quicksort_inner(char* base, size_t lo, size_t hi, size_t sz,
                            int (*cmp)(const void*, const void*)) {
    if (lo >= hi || hi == (size_t)-1) return;
    // Insertion sort на маленьких диапазонах.
    if (hi - lo < 8) {
        for (size_t i = lo + 1; i <= hi; i++) {
            for (size_t j = i; j > lo && cmp(base + j*sz, base + (j-1)*sz) < 0; j--) {
                swap_bytes(base + j*sz, base + (j-1)*sz, sz);
            }
        }
        return;
    }
    // Pivot — средний элемент.
    size_t mid = lo + (hi - lo) / 2;
    swap_bytes(base + mid*sz, base + hi*sz, sz);
    char* pivot = base + hi*sz;
    size_t store = lo;
    for (size_t i = lo; i < hi; i++) {
        if (cmp(base + i*sz, pivot) < 0) {
            swap_bytes(base + i*sz, base + store*sz, sz);
            store++;
        }
    }
    swap_bytes(base + store*sz, base + hi*sz, sz);
    if (store) quicksort_inner(base, lo, store - 1, sz, cmp);
    quicksort_inner(base, store + 1, hi, sz, cmp);
}

void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    if (n < 2 || sz == 0) return;
    quicksort_inner((char*)base, 0, n - 1, sz, cmp);
}

void* bsearch(const void* key, const void* base, size_t n, size_t sz,
              int (*cmp)(const void*, const void*)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void* p = (const char*)base + mid * sz;
        int r = cmp(key, p);
        if (r == 0) return (void*)p;
        if (r < 0) hi = mid;
        else       lo = mid + 1;
    }
    return NULL;
}

// xorshift64* (хорошие статистические свойства, минимум кода).
static uint64_t g_rand_state = 0x9E3779B97F4A7C15ULL;

void srand(unsigned int seed) {
    g_rand_state = (uint64_t)seed * 0x100000001B3ULL ^ 0xCBF29CE484222325ULL;
    if (g_rand_state == 0) g_rand_state = 1;
}

int rand(void) {
    uint64_t x = g_rand_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rand_state = x;
    return (int)((x * 0x2545F4914F6CDD1DULL) >> 33) & RAND_MAX;
}

// Окружение — на 39-м это пустой массив (CRT уже подставил { NULL }).
extern char** environ;

char* getenv(const char* name) {
    size_t nlen = 0;
    while (name[nlen] && name[nlen] != '=') nlen++;
    for (char** e = environ; e && *e; e++) {
        const char* p = *e;
        for (size_t i = 0; i < nlen; i++) {
            if (p[i] != name[i]) goto next;
        }
        if (p[nlen] == '=') return (char*)(p + nlen + 1);
    next: ;
    }
    return NULL;
}

int setenv(const char* name, const char* value, int overwrite) {
    (void)name; (void)value; (void)overwrite;
    return 0;       // На 39-м — окружение read-only.
}

int unsetenv(const char* name) { (void)name; return 0; }
int putenv  (char* string)     { (void)string; return 0; }

int system(const char* cmd) { (void)cmd; return -1; }

// abort — попытка SIGABRT, если не дошло — _exit.
extern void _exit(int);
__attribute__((noreturn)) void abort(void) {
    _exit(134);   // 128 + SIGABRT
    for (;;) __asm__ volatile("hlt");
}

// __assert_fail — выводит сообщение и вызывает abort.
extern int write(int, const void*, size_t);
extern size_t strlen(const char*);

__attribute__((noreturn))
void __assert_fail(const char* expr, const char* file, int line, const char* fn) {
    static const char prefix[] = "Assertion failed: ";
    static const char inmsg[]  = " in ";
    static const char atmsg[]  = " at ";
    static const char colon[]  = ":";
    static const char nl[]     = "\n";
    write(2, prefix, sizeof(prefix) - 1);
    write(2, expr, strlen(expr));
    if (fn)   { write(2, inmsg, sizeof(inmsg) - 1); write(2, fn, strlen(fn)); }
    if (file) { write(2, atmsg, sizeof(atmsg) - 1); write(2, file, strlen(file)); }
    write(2, colon, sizeof(colon) - 1);
    // line → text
    char lb[16]; int ln = 0;
    if (line == 0) lb[ln++] = '0';
    else { while (line > 0) { lb[ln++] = '0' + (line % 10); line /= 10; } }
    while (ln) { write(2, &lb[--ln], 1); }
    write(2, nl, 1);
    abort();
}
