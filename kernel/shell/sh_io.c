// ============================================================================
// sh_io.c — Реализация абстракции ввода/вывода shell (Этап 29)
// ============================================================================

#include "sh_io.h"
#include "../vga.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Активный контекст. По умолчанию — нулевой (вывод в VGA, без stdin).
static sh_ctx_t  g_default_ctx = { NULL, NULL, 0 };
static sh_ctx_t* g_active = &g_default_ctx;

void sh_ctx_set(sh_ctx_t* ctx) {
    g_active = ctx ? ctx : &g_default_ctx;
}

sh_ctx_t* sh_ctx_get(void) {
    return g_active;
}

// ---------- ВЫВОД ----------

void sh_putc(char c) {
    if (g_active->out_pipe) {
        uint8_t b = (uint8_t)c;
        pipe_write(g_active->out_pipe, &b, 1);
    } else {
        vga_putchar(c);
    }
}

void sh_puts(const char* s) {
    if (!s) return;
    if (g_active->out_pipe) {
        // Считаем длину
        size_t len = 0;
        while (s[len]) len++;
        if (len) pipe_write(g_active->out_pipe, s, len);
    } else {
        vga_puts(s);
    }
}

void sh_println(const char* s) {
    sh_puts(s);
    sh_putc('\n');
}

// Простая печать беззнакового десятичного числа.
void sh_print_dec(uint32_t n) {
    if (g_active->out_pipe) {
        char buf[12];
        int  i = 0;
        if (n == 0) {
            sh_putc('0');
            return;
        }
        while (n > 0 && i < 11) {
            buf[i++] = '0' + (char)(n % 10);
            n /= 10;
        }
        // Разворачиваем
        while (i > 0) {
            sh_putc(buf[--i]);
        }
    } else {
        vga_print_dec(n);
    }
}

void sh_set_color(vga_color_t fg, vga_color_t bg) {
    if (!g_active->out_pipe) {
        vga_set_color(fg, bg);
    }
}

// ---------- ВВОД ----------

int sh_readline_pipe(char* buf, int max_len) {
    if (!g_active->in_pipe) return -1;
    pipe_t* p = g_active->in_pipe;

    int  pos = 0;
    bool got_any = false;

    while (pos < max_len - 1) {
        uint8_t c;
        size_t  n = pipe_read(p, &c, 1);
        if (n == 0) {
            // Пусто. EOF? — если запись закрыта.
            if (pipe_eof(p)) {
                if (got_any) break;
                return -1;
            }
            // Иначе — нет данных прямо сейчас. В нашем kernel нет
            // блокирующего ожидания, поэтому возвращаем то, что есть.
            if (got_any) break;
            return 0;
        }
        got_any = true;
        if (c == '\n') break;
        buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return pos;
}

bool sh_next_line(char* out_buf, int max_len) {
    while (1) {
        int rc = sh_readline_pipe(out_buf, max_len);
        if (rc < 0) return false;          // EOF
        if (rc == 0) {
            // Пайп пуст в данный момент. В нашей модели команды выполняются
            // последовательно (cmd1 завершилась → её out закрыт → cmd2 видит
            // EOF), поэтому "пусто, но не EOF" в реальности не встречается.
            return false;
        }
        return true;
    }
}
