// ============================================================================
// printf.c — vfprintf/printf/snprintf/sprintf и весь форматный движок.
// ----------------------------------------------------------------------------
// Поддержка спецификаторов:
//   %d %i %u %o %x %X %p — целые и указатели
//   %c %s %% — символы и строки
//   %lld/%llu/%ld/%lu/%zd/%zu — модификаторы длины
//   %nd с шириной (включая 0-padding и left-justify '-')
//   %.Ns — точность для %s
// Не поддерживаем (для 39-го): %f/%g/%e (придёт с math).
// ============================================================================
#include "stdio_impl.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct {
    char*       buf;            // если != NULL — пишем в строку
    size_t      cap;            // размер buf (snprintf/sprintf)
    size_t      pos;            // куда писать
    FILE_impl*  file;           // если buf == NULL — пишем в FILE
    int         err;
    int         truncated;      // 1 если буфер переполнился (snprintf)
} sink_t;

static void sink_putc(sink_t* s, int c) {
    if (s->err) return;
    if (s->buf) {
        if (s->cap == 0) { s->pos++; return; }   // sprintf без cap не должен — но safe
        if (s->pos + 1 < s->cap) s->buf[s->pos] = (char)c;
        else                     s->truncated = 1;
        s->pos++;
    } else {
        FILE_impl* f = s->file;
        if (f->bufmode == _IONBF || !f->buf) {
            unsigned char ch = (unsigned char)c;
            if (write(f->fd, &ch, 1) != 1) { f->err = 1; s->err = 1; }
        } else {
            f->buf[f->bufpos++] = (unsigned char)c;
            if (f->bufpos >= f->bufsize ||
                (f->bufmode == _IOLBF && c == '\n')) {
                if (__aos_flush_one(f) < 0) s->err = 1;
            }
        }
        s->pos++;
    }
}

static void sink_puts(sink_t* s, const char* str, size_t n) {
    while (n--) sink_putc(s, *str++);
}

static void emit_uint(sink_t* s, unsigned long long v, int base, int upper,
                      int width, int zero_pad, int left, int alt) {
    char buf[32]; int n = 0;
    const char* hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) buf[n++] = '0';
    else while (v > 0) { buf[n++] = hex[v % (unsigned)base]; v /= (unsigned)base; }

    int alt_len = 0;
    char alt_prefix[2] = {0,0};
    if (alt) {
        if (base == 16) { alt_prefix[0] = '0'; alt_prefix[1] = upper ? 'X' : 'x'; alt_len = 2; }
        else if (base == 8 && buf[n-1] != '0') { alt_prefix[0] = '0'; alt_len = 1; }
    }

    int total = n + alt_len;
    int pad = (width > total) ? (width - total) : 0;

    if (!left && !zero_pad) while (pad-- > 0) sink_putc(s, ' ');
    for (int i = 0; i < alt_len; i++) sink_putc(s, alt_prefix[i]);
    if (!left && zero_pad) while (pad-- > 0) sink_putc(s, '0');
    while (n > 0) sink_putc(s, buf[--n]);
    if (left) while (pad-- > 0) sink_putc(s, ' ');
}

static void emit_sint(sink_t* s, long long v, int width, int zero_pad,
                      int left, int plus, int space) {
    int neg = (v < 0);
    unsigned long long uv = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                                : (unsigned long long)v;
    char buf[32]; int n = 0;
    if (uv == 0) buf[n++] = '0';
    else while (uv > 0) { buf[n++] = '0' + (uv % 10); uv /= 10; }
    char sign = 0;
    if (neg) sign = '-';
    else if (plus) sign = '+';
    else if (space) sign = ' ';

    int sign_len = sign ? 1 : 0;
    int total = n + sign_len;
    int pad = (width > total) ? (width - total) : 0;

    if (!left && !zero_pad) while (pad-- > 0) sink_putc(s, ' ');
    if (sign) sink_putc(s, sign);
    if (!left && zero_pad) while (pad-- > 0) sink_putc(s, '0');
    while (n > 0) sink_putc(s, buf[--n]);
    if (left) while (pad-- > 0) sink_putc(s, ' ');
}

static int vformat(sink_t* s, const char* fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { sink_putc(s, *fmt++); continue; }
        fmt++;

        int left = 0, zero_pad = 0, alt = 0, plus = 0, space = 0;
        while (1) {
            if      (*fmt == '-') { left = 1;     fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '#') { alt = 1;      fmt++; }
            else if (*fmt == '+') { plus = 1;     fmt++; }
            else if (*fmt == ' ') { space = 1;    fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt++ - '0'); }
        }

        int len = 0;       // 0=int, 1=l, 2=ll, 3=z
        if      (fmt[0] == 'l' && fmt[1] == 'l') { len = 2; fmt += 2; }
        else if (*fmt == 'l') { len = 1; fmt++; }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { len = 3; fmt++; }
        else if (*fmt == 'j' || *fmt == 't') { len = 2; fmt++; }

        char spec = *fmt;
        if (!spec) break;
        fmt++;

        switch (spec) {
        case '%': sink_putc(s, '%'); break;

        case 'c': sink_putc(s, va_arg(ap, int)); break;

        case 's': {
            const char* str = va_arg(ap, const char*);
            if (!str) str = "(null)";
            size_t slen = 0;
            while (str[slen]) slen++;
            if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
            int pad = (width > (int)slen) ? (width - (int)slen) : 0;
            if (!left) while (pad-- > 0) sink_putc(s, ' ');
            sink_puts(s, str, slen);
            if (left)  while (pad-- > 0) sink_putc(s, ' ');
            break;
        }

        case 'd': case 'i': {
            long long v;
            if      (len == 2) v = va_arg(ap, long long);
            else if (len == 1) v = va_arg(ap, long);
            else if (len == 3) v = (long long)va_arg(ap, size_t);
            else               v = va_arg(ap, int);
            emit_sint(s, v, width, zero_pad, left, plus, space);
            break;
        }

        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v;
            if      (len == 2) v = va_arg(ap, unsigned long long);
            else if (len == 1) v = va_arg(ap, unsigned long);
            else if (len == 3) v = (unsigned long long)va_arg(ap, size_t);
            else               v = va_arg(ap, unsigned int);
            int base  = (spec == 'o') ? 8 : (spec == 'u') ? 10 : 16;
            int upper = (spec == 'X');
            emit_uint(s, v, base, upper, width, zero_pad, left, alt);
            break;
        }

        case 'p': {
            void* ptr = va_arg(ap, void*);
            sink_putc(s, '0'); sink_putc(s, 'x');
            emit_uint(s, (unsigned long long)(uintptr_t)ptr, 16, 0,
                      width > 2 ? width - 2 : 0, 1, left, 0);
            break;
        }

        case 'n': {
            int* dst = va_arg(ap, int*);
            if (dst) *dst = (int)s->pos;
            break;
        }

        default:
            sink_putc(s, '%');
            sink_putc(s, spec);
            break;
        }
    }
    return s->err ? -1 : (int)s->pos;
}

int vfprintf(FILE* __restrict__ stream, const char* __restrict__ fmt, va_list ap) {
    FILE_impl* f = (FILE_impl*)stream;
    sink_t s = { .buf = NULL, .cap = 0, .pos = 0, .file = f,
                 .err = 0, .truncated = 0 };
    return vformat(&s, fmt, ap);
}

int vprintf(const char* __restrict__ fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char* __restrict__ fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE* __restrict__ stream, const char* __restrict__ fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char* __restrict__ buf, size_t cap, const char* __restrict__ fmt, va_list ap) {
    sink_t s = { .buf = buf, .cap = cap, .pos = 0, .file = NULL,
                 .err = 0, .truncated = 0 };
    int r = vformat(&s, fmt, ap);
    if (buf && cap > 0) {
        size_t end = (s.pos < cap) ? s.pos : (cap - 1);
        buf[end] = 0;
    }
    return r;
}

int snprintf(char* __restrict__ buf, size_t cap, const char* __restrict__ fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* __restrict__ buf, const char* __restrict__ fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char* __restrict__ buf, const char* __restrict__ fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}
