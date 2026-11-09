// ============================================================================
// init.c — инициализация stdin/stdout/stderr и общая регистрация FILE*.
// ============================================================================
#include "stdio_impl.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE_impl* __aos_open_files = NULL;

// Статические структуры для трёх стандартных потоков — никаких malloc'ов,
// чтобы stdio был доступен ДО первого malloc (важно для printf в ранних
// тестах).
static unsigned char g_stdin_buf [AOS_BUFSZ];
static unsigned char g_stdout_buf[AOS_BUFSZ];
static unsigned char g_stderr_buf[AOS_BUFSZ];

static FILE_impl g_stdin  = {
    .fd = 0, .flags = AOS_FF_READ,  .bufmode = _IOLBF,
    .buf = g_stdin_buf,  .bufsize = AOS_BUFSZ, .pushback = -1 };
static FILE_impl g_stdout = {
    .fd = 1, .flags = AOS_FF_WRITE, .bufmode = _IOLBF,
    .buf = g_stdout_buf, .bufsize = AOS_BUFSZ, .pushback = -1 };
static FILE_impl g_stderr = {
    .fd = 2, .flags = AOS_FF_WRITE, .bufmode = _IONBF,
    .buf = g_stderr_buf, .bufsize = AOS_BUFSZ, .pushback = -1 };

FILE* stdin  = (FILE*)&g_stdin;
FILE* stdout = (FILE*)&g_stdout;
FILE* stderr = (FILE*)&g_stderr;

void __aos_stdio_init(void) {
    // Просто связываем три стандарта в список.
    g_stdin.next  = &g_stdout;
    g_stdout.next = &g_stderr;
    g_stderr.next = NULL;
    __aos_open_files = &g_stdin;
}

void __aos_register_file(FILE_impl* f) {
    f->next = __aos_open_files;
    __aos_open_files = f;
}

void __aos_unregister_file(FILE_impl* f) {
    FILE_impl** p = &__aos_open_files;
    while (*p && *p != f) p = &(*p)->next;
    if (*p) *p = f->next;
}

int __aos_flush_one(FILE_impl* f) {
    if (!f || !(f->flags & AOS_FF_WRITE) || f->bufpos == 0) return 0;
    size_t off = 0;
    while (off < f->bufpos) {
        ssize_t w = write(f->fd, f->buf + off, f->bufpos - off);
        if (w <= 0) { f->err = 1; return -1; }
        off += (size_t)w;
    }
    f->bufpos = 0;
    return 0;
}

void __aos_stdio_flush_all(void) {
    for (FILE_impl* f = __aos_open_files; f; f = f->next) {
        __aos_flush_one(f);
    }
}
