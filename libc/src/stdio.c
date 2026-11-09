#include "../include/stdio.h"
#include "../include/string.h"
#include "syscalls.h"
#include <stdarg.h>

static FILE _stdin = {0};
static FILE _stdout = {1};
static FILE _stderr = {2};

FILE* stdin = &_stdin;
FILE* stdout = &_stdout;
FILE* stderr = &_stderr;

#define MYOS_O_RDONLY   0x01
#define MYOS_O_WRONLY   0x02
#define MYOS_O_RDWR     0x03

int putchar(int c) {
    char ch = c;
    _syscall3(SYS_WRITE, 1, (uint32_t)&ch, 1);
    return c;
}

int puts(const char* s) {
    _syscall3(SYS_WRITE, 1, (uint32_t)s, strlen(s));
    putchar('\n');
    return 1;
}

static void print_int(int value) {
    char buf[16];
    int pos = 0;
    int is_neg = 0;
    if (value < 0) { is_neg = 1; value = -value; }
    if (value == 0) { buf[pos++] = '0'; }
    else {
        char tmp[16]; int tpos = 0;
        while (value > 0) { tmp[tpos++] = '0' + (value % 10); value /= 10; }
        if (is_neg) buf[pos++] = '-';
        for (int i = tpos - 1; i >= 0; i--) buf[pos++] = tmp[i];
    }
    buf[pos] = '\0';
    _syscall3(SYS_WRITE, 1, (uint32_t)buf, strlen(buf));
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 'd') { print_int(va_arg(args, int)); }
            else if (*format == 's') { 
                char* s = va_arg(args, char*);
                _syscall3(SYS_WRITE, 1, (uint32_t)s, strlen(s));
            }
            else if (*format == 'c') { putchar(va_arg(args, int)); }
            else if (*format == '%') { putchar('%'); }
        } else {
            putchar(*format);
        }
        format++;
    }
    va_end(args);
    return 0;
}

FILE* fopen(const char* path, const char* mode) {
    int flags = MYOS_O_RDONLY;
    if (mode[0] == 'w') flags = MYOS_O_WRONLY | 0x08 | 0x10; // CREAT | TRUNC
    else if (mode[0] == 'a') flags = MYOS_O_WRONLY | 0x08; // CREAT
    
    int fd = _syscall2(SYS_OPEN, (uint32_t)path, (uint32_t)flags);
    if (fd < 0) return NULL;
    
    FILE* f = (FILE*)_syscall1(SYS_SBRK, sizeof(FILE)); 
    f->fd = fd;
    return f;
}

int fclose(FILE* stream) {
    if (!stream) return -1;
    return _syscall1(SYS_CLOSE, (uint32_t)stream->fd);
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    int res = _syscall3(SYS_READ, (uint32_t)stream->fd, (uint32_t)ptr, size * nmemb);
    if (res < 0) return 0;
    return res / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    int res = _syscall3(SYS_WRITE, (uint32_t)stream->fd, (uint32_t)ptr, size * nmemb);
    if (res < 0) return 0;
    return res / size;
}
