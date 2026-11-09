// ============================================================================
// crt1.c — высокоуровневый загрузочный код (вызывается из crt0.S).
// ----------------------------------------------------------------------------
// __libc_start_main:
//   1. Сохраняет argv/envp в окружении (environ).
//   2. Инициализирует stdin/stdout/stderr (lazy — fopen-by-fd).
//   3. Вызывает main(argc, argv, envp).
//   4. Гарантирует flush открытых FILE* + atexit-callback'и + _exit(rc).
//
// Имя «__libc_start_main» зеркалит glibc/musl — будущее линковочное
// совместимое ПО ожидает именно его.
// ============================================================================
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

char** environ = (char*[]){ 0 };

// Forward — определены в stdio/init.c.
void __aos_stdio_init(void);
void __aos_stdio_flush_all(void);

// atexit-таблица.
#define ATEXIT_MAX_FUNCS 32
static void (*g_atexit_funcs[ATEXIT_MAX_FUNCS])(void);
static int    g_atexit_count = 0;

int atexit(void (*func)(void)) {
    if (g_atexit_count >= ATEXIT_MAX_FUNCS) return -1;
    g_atexit_funcs[g_atexit_count++] = func;
    return 0;
}

__attribute__((noreturn))
void exit(int status) {
    // atexit-функции в обратном порядке (LIFO).
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_funcs[i]) g_atexit_funcs[i]();
    }
    __aos_stdio_flush_all();
    _exit(status);
}

__attribute__((noreturn))
void _Exit(int status) {
    _exit(status);
}

__attribute__((noreturn))
void __libc_start_main(int (*main_fn)(int, char**, char**),
                       int argc, char** argv, char** envp) {
    environ = envp ? envp : (char*[]){ 0 };
    __aos_stdio_init();
    int rc = main_fn(argc, argv, envp);
    exit(rc);
}
