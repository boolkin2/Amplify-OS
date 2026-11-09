// ============================================================================
// stdlib.h — общие утилиты (память, конверсия, выход, env).
// ============================================================================
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF

// Память
void* malloc (size_t size);
void* calloc (size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void  free   (void* ptr);

// Преобразования
int    atoi (const char* s);
long   atol (const char* s);
long long atoll(const char* s);
double atof (const char* s);

long      strtol  (const char* __restrict__ s, char** __restrict__ end, int base);
long long strtoll (const char* __restrict__ s, char** __restrict__ end, int base);
unsigned long      strtoul (const char* __restrict__ s, char** __restrict__ end, int base);
unsigned long long strtoull(const char* __restrict__ s, char** __restrict__ end, int base);
double             strtod  (const char* __restrict__ s, char** __restrict__ end);

// Случайные числа (xorshift64)
int  rand   (void);
void srand  (unsigned int seed);

// Управление процессом
void   exit   (int status) __attribute__((noreturn));
void   _Exit  (int status) __attribute__((noreturn));
void   abort  (void)       __attribute__((noreturn));
int    atexit (void (*func)(void));

// Окружение
char*  getenv (const char* name);
int    setenv (const char* name, const char* value, int overwrite);
int    unsetenv(const char* name);
int    putenv (char* string);

// Утилиты
int    abs (int x);
long   labs(long x);
long long llabs(long long x);

void   qsort(void* base, size_t nmemb, size_t size,
             int (*cmp)(const void*, const void*));
void*  bsearch(const void* key, const void* base, size_t nmemb, size_t size,
               int (*cmp)(const void*, const void*));

int    system(const char* cmd);

#ifdef __cplusplus
}
#endif
#endif
