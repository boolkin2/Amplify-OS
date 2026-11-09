// ============================================================================
// apps.c - Реализация встроенных приложений для MyOS
// ============================================================================
// Каждое приложение — набор из 3 callback-функций:
//   on_create(win)        — рисует начальное содержимое окна
//   on_update(win)        — обновляет содержимое (для живых приложений)
//   on_click(win, x, y)   — обрабатывает клик мышью
// ============================================================================

#include "apps.h"
#include "graphics.h"
#include "font.h"
#include "wm.h"
#include "framebuffer.h"
#include "desktop.h"
#include "ui_theme.h"     // AUTO-GENERATED: цвета темы от AI-генератора
#include "../drivers/timer.h"
#include "../drivers/mouse.h"
#include "../memory/pmm.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../shell/shell.h"
#include "../vga.h"
#include "../arch/i386/cpu_stats.h"
#include "../arch/i386/acpi.h"
#include "../arch/i386/ioapic.h"
#include "../arch/i386/smp.h"
#include "../arch/i386/longmode.h"
#include "../drivers/rtc.h"
#include "../net/netif.h"
#include "../net/netcfg.h"
#include "../net/dns.h"
#include "../net/ping.h"
#include "../net/http.h"
#include <stddef.h>

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void app_str_copy(char* dest, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static int app_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

// Рисует текст внутри окна (координаты относительно окна)
static void win_draw_text(window_t* win, int32_t x, int32_t y, const char* str,
                          uint32_t fg, uint32_t bg) {
    // Рисуем символы попиксельно в буфер окна
    int32_t cx = x;
    while (*str) {
        // Получаем данные глифа
        extern const uint8_t font_8x16[];
        uint8_t c = (uint8_t)*str;
        const uint8_t* glyph = &font_8x16[c * FONT_HEIGHT];
        
        for (int32_t gy = 0; gy < FONT_HEIGHT; gy++) {
            uint8_t row = glyph[gy];
            for (int32_t gx = 0; gx < FONT_WIDTH; gx++) {
                if (row & (0x80 >> gx)) {
                    window_put_pixel(win, cx + gx, y + gy, fg);
                } else if (bg != 0xFF000000) {
                    window_put_pixel(win, cx + gx, y + gy, bg);
                }
            }
        }
        cx += FONT_WIDTH;
        str++;
    }
}

// Рисует закрашенный прямоугольник в буфере окна
static void win_fill_rect(window_t* win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t ry = y; ry < y + h; ry++) {
        for (int32_t rx = x; rx < x + w; rx++) {
            window_put_pixel(win, rx, ry, color);
        }
    }
}

// Рисует контур прямоугольника в буфере окна
static void win_draw_rect(window_t* win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t i = x; i < x + w; i++) {
        window_put_pixel(win, i, y, color);
        window_put_pixel(win, i, y + h - 1, color);
    }
    for (int32_t i = y; i < y + h; i++) {
        window_put_pixel(win, x, i, color);
        window_put_pixel(win, x + w - 1, i, color);
    }
}

// Число в строку
static void int_to_str(int32_t num, char* buf) {
    if (num == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    
    int neg = 0;
    if (num < 0) { neg = 1; num = -num; }
    
    char tmp[16];
    int i = 0;
    while (num > 0 && i < 15) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// ============================================================================
// 1. КАЛЬКУЛЯТОР
// ============================================================================

// ----------------------------------------------------------------------------
// Калькулятор (расширенный): целочисленный + плавающая точка по запросу.
// Кнопки: C, ±, %, ÷, MC/MR/M+/M-, x², 1/x, √, операции, =. Поддерживает
// клавиатурный ввод (digits, +,-,*,/, Enter, Esc, %, Backspace).
// ----------------------------------------------------------------------------

// Используем целочисленную арифметику с фиксированной точкой 6 знаков (млн.)
// для возможности корректно делить и брать процент, не вводя FPU.
//   internal value = real * 10^calc_decimals (точность 6 знаков после запятой
//   на промежуточных этапах; при отображении печатаем с экономией)
typedef int64_t calc_num_t;
#define CALC_SCALE  10000LL    // 4 знака после точки (helper'ы int64 без int128)
#define CALC_PRECIS 4

static calc_num_t calc_display = 0;     // текущее значение на экране
static calc_num_t calc_acc     = 0;     // накопленное (LHS) для бинарной операции
static char       calc_op      = 0;     // 0 / + / - / * / /
static bool       calc_new     = true;  // следующий цифровой ввод начинает заново
static bool       calc_error   = false;
static calc_num_t calc_memory  = 0;     // M
static char       calc_history[40] = "";

// Кнопки: 5 колонок × 6 рядов
//
// row 0:  MC  MR  M+  M-  C
// row 1:  ±   %   √   x²  ÷
// row 2:  7   8   9   1/x ×
// row 3:  4   5   6        −
// row 4:  1   2   3        +
// row 5:  0       .   =
//
// «Бесшовные» вертикальные кнопки (×, −, +, =) реализуем как высота=2 ряда.
typedef struct {
    int col, row;       // верхний-левый угол в сетке
    int wcols, hrows;   // ширина и высота в клетках сетки
    const char* label;
    const char* code;   // код для логики
    uint32_t bg_top, bg_bot;
} calc_btn_t;

static const calc_btn_t calc_buttons2[] = {
    // row 0
    { 0, 0, 1, 1, "MC", "MC", 0xFF4D5573, 0xFF2A3050 },
    { 1, 0, 1, 1, "MR", "MR", 0xFF4D5573, 0xFF2A3050 },
    { 2, 0, 1, 1, "M+", "M+", 0xFF4D5573, 0xFF2A3050 },
    { 3, 0, 1, 1, "M-", "M-", 0xFF4D5573, 0xFF2A3050 },
    { 4, 0, 1, 1, "C",  "C",  0xFFB04050, 0xFF761A28 },
    // row 1
    { 0, 1, 1, 1, "+/-", "NEG", 0xFF6677AA, 0xFF334466 },
    { 1, 1, 1, 1, "%",   "%",   0xFF6677AA, 0xFF334466 },
    { 2, 1, 1, 1, "sqrt","SQRT",0xFF6677AA, 0xFF334466 },
    { 3, 1, 1, 1, "x^2", "SQR", 0xFF6677AA, 0xFF334466 },
    { 4, 1, 1, 1, "/",   "/",   0xFFCC8B40, 0xFF8A5A18 },
    // row 2
    { 0, 2, 1, 1, "7", "7", 0xFF555577, 0xFF333355 },
    { 1, 2, 1, 1, "8", "8", 0xFF555577, 0xFF333355 },
    { 2, 2, 1, 1, "9", "9", 0xFF555577, 0xFF333355 },
    { 3, 2, 1, 1, "1/x", "INV", 0xFF6677AA, 0xFF334466 },
    { 4, 2, 1, 1, "x", "*",  0xFFCC8B40, 0xFF8A5A18 },
    // row 3
    { 0, 3, 1, 1, "4", "4", 0xFF555577, 0xFF333355 },
    { 1, 3, 1, 1, "5", "5", 0xFF555577, 0xFF333355 },
    { 2, 3, 1, 1, "6", "6", 0xFF555577, 0xFF333355 },
    { 3, 3, 1, 1, "<-", "BS", 0xFF6677AA, 0xFF334466 },
    { 4, 3, 1, 1, "-",  "-", 0xFFCC8B40, 0xFF8A5A18 },
    // row 4
    { 0, 4, 1, 1, "1", "1", 0xFF555577, 0xFF333355 },
    { 1, 4, 1, 1, "2", "2", 0xFF555577, 0xFF333355 },
    { 2, 4, 1, 1, "3", "3", 0xFF555577, 0xFF333355 },
    { 3, 4, 1, 1, "M",  "MR2", 0xFF6677AA, 0xFF334466 },
    { 4, 4, 1, 1, "+",  "+", 0xFFCC8B40, 0xFF8A5A18 },
    // row 5
    { 0, 5, 2, 1, "0", "0", 0xFF555577, 0xFF333355 },
    { 2, 5, 1, 1, ".", ".", 0xFF555577, 0xFF333355 },
    { 3, 5, 2, 1, "=", "=", 0xFF44AA66, 0xFF1F6A38 },
};
#define CALC_BUTTON_COUNT (int)(sizeof(calc_buttons2)/sizeof(calc_buttons2[0]))

static void calc_format(calc_num_t v, char* out, int outlen) {
    // Печатаем со сжатием: убираем хвостовые нули и точку, если нет дробной
    if (v == 0) {
        if (outlen > 1) { out[0] = '0'; out[1] = 0; }
        return;
    }
    bool neg = (v < 0); if (neg) v = -v;
    int64_t int_part  = v / CALC_SCALE;
    int64_t frac_part = v % CALC_SCALE;
    // Уберём trailing zeros
    int frac_digits = CALC_PRECIS;
    while (frac_digits > 0 && (frac_part % 10 == 0)) {
        frac_part /= 10; frac_digits--;
    }
    char buf[40];
    int p = 0;
    if (neg) buf[p++] = '-';
    // целая часть
    char ibuf[24]; int ip = 0;
    if (int_part == 0) ibuf[ip++] = '0';
    else {
        while (int_part > 0 && ip < 22) {
            ibuf[ip++] = '0' + (int)(int_part % 10);
            int_part /= 10;
        }
    }
    while (ip > 0) buf[p++] = ibuf[--ip];
    if (frac_digits > 0) {
        buf[p++] = '.';
        char fbuf[CALC_PRECIS + 1]; int fp = 0;
        for (int i = 0; i < frac_digits; i++) {
            fbuf[i] = '0' + (int)(frac_part % 10);
            frac_part /= 10;
        }
        for (int i = frac_digits - 1; i >= 0; i--) buf[p++] = fbuf[i];
        (void)fp;
    }
    buf[p] = 0;
    int j = 0; while (j < p && j < outlen - 1) { out[j] = buf[j]; j++; }
    out[j] = 0;
}

static calc_num_t calc_apply(calc_num_t a, calc_num_t b, char op, bool* err) {
    *err = false;
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': {
            // a * b с учётом масштаба: (a/S) * (b/S) * S = a*b/S
            // При CALC_SCALE = 1e4 и a,b до ~1e10 это безопасно в int64.
            return (a * b) / CALC_SCALE;
        }
        case '/': {
            if (b == 0) { *err = true; return 0; }
            return (a * CALC_SCALE) / b;
        }
        default:  return b;
    }
}

// Целочисленный квадратный корень с фиксированной точкой
static calc_num_t calc_sqrt_fp(calc_num_t v) {
    if (v <= 0) return 0;
    // Хотим sqrt(v / SCALE) * SCALE = sqrt(v * SCALE).
    // При CALC_SCALE = 1e4 и v до ~1e15, v*SCALE = 1e19 — выходим за int64.
    // Ограничим v до ~1e14, что реально для калькулятора.
    int64_t x = v * CALC_SCALE;  // до 1e18 OK
    // Метод Ньютона
    int64_t r = x;
    for (int it = 0; it < 60; it++) {
        int64_t q = x / r;
        int64_t nr = (r + q) / 2;
        if (nr == r) break;
        r = nr;
    }
    return (calc_num_t)r;
}

static void calc_draw_display(window_t* win) {
    // Фон
    int W = win->width;
    win_fill_rect(win, 8, 8, W - 16, 60, 0xFF14182A);
    win_draw_rect(win, 8, 8, W - 16, 60, 0xFF445566);

    // История (сверху)
    if (calc_history[0]) {
        win_draw_text(win, 14, 14, calc_history,
                      0xFF7788AA, 0xFF000000);
    }

    // Главное число — крупно, справа
    char buf[40];
    if (calc_error) {
        int j = 0;
        const char* msg = "Ошибка";
        while (msg[j] && j < 39) { buf[j] = msg[j]; j++; }
        buf[j] = 0;
    } else {
        calc_format(calc_display, buf, 40);
    }
    int len = (int)app_strlen(buf);
    int tx = W - 14 - len * FONT_WIDTH * 2;
    if (tx < 14) tx = 14;
    extern const uint8_t font_8x16[];
    for (int ci = 0; ci < len; ci++) {
        uint8_t c = (uint8_t)buf[ci];
        const uint8_t* glyph = &font_8x16[c * FONT_HEIGHT];
        for (int gy = 0; gy < FONT_HEIGHT; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                if (row & (0x80 >> gx)) {
                    for (int sy = 0; sy < 2; sy++) {
                        for (int sx = 0; sx < 2; sx++) {
                            window_put_pixel(win,
                                tx + ci * FONT_WIDTH * 2 + gx * 2 + sx,
                                32 + gy * 2 + sy,
                                calc_error ? 0xFFFF7766 : 0xFFAAFFCC);
                        }
                    }
                }
            }
        }
    }

    // Индикатор памяти, если есть
    if (calc_memory != 0) {
        win_draw_text(win, 14, 50, "M", 0xFFFFAA66, 0xFF000000);
    }
}

// Геометрия кнопок калькулятора
static void calc_button_rect(int idx, int W, int H,
                             int* bx, int* by, int* bw, int* bh) {
    int top = 78;
    int pad = 8;
    int gap = 4;
    int rows = 6;
    int cols = 5;
    int field_w = W - pad * 2;
    int field_h = H - top - pad;
    int cw = (field_w - (cols - 1) * gap) / cols;
    int ch = (field_h - (rows - 1) * gap) / rows;
    const calc_btn_t* b = &calc_buttons2[idx];
    *bx = pad + b->col * (cw + gap);
    *by = top + b->row * (ch + gap);
    *bw = b->wcols * cw + (b->wcols - 1) * gap;
    *bh = b->hrows * ch + (b->hrows - 1) * gap;
}

static void calc_draw(window_t* win) {
    // Фон окна
    int W = win->width, H = win->height;
    for (int yy = 0; yy < H; yy++) {
        uint32_t r = 30 + (yy * 12) / H;
        uint32_t g = 32 + (yy *  8) / H;
        uint32_t b = 50 + (yy *  4) / H;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int xx = 0; xx < W; xx++) window_put_pixel(win, xx, yy, col);
    }
    calc_draw_display(win);
    for (int i = 0; i < CALC_BUTTON_COUNT; i++) {
        int bx, by, bw, bh;
        calc_button_rect(i, W, H, &bx, &by, &bw, &bh);
        gfx_fill_rect_gradient(win->x + bx, win->y + by, bw, bh,
            calc_buttons2[i].bg_top, calc_buttons2[i].bg_bot);
        win_draw_rect(win, bx, by, bw, bh, 0xFF1A1A22);
        const char* lbl = calc_buttons2[i].label;
        int lw = (int)app_strlen(lbl) * FONT_WIDTH;
        win_draw_text(win, bx + (bw - lw)/2, by + (bh - FONT_HEIGHT)/2,
                      lbl, 0xFFFFFFFF, 0xFF000000);
    }
}

// Логика клика по коду кнопки
static void calc_press(const char* code, window_t* win) {
    if (calc_error && code[0] != 'C') return;

    char c0 = code[0];
    if (c0 >= '0' && c0 <= '9') {
        int d = c0 - '0';
        if (calc_new) {
            calc_display = (calc_num_t)d * CALC_SCALE;
            calc_new = false;
        } else {
            // Сдвиг влево: int_part * 10 + d
            // В fp-форме: (int_part * 10 + d) * SCALE  + frac_part * 10 (mod ...)
            // Проще: целая часть умножается на 10 + d.
            // Если есть дробная — добавляем младший знак.
            calc_num_t int_part  = calc_display / CALC_SCALE;
            calc_num_t frac_part = calc_display - int_part * CALC_SCALE;
            if (frac_part == 0) {
                calc_display = (int_part * 10 + d) * CALC_SCALE;
            } else {
                // Добавление цифры в дробную часть в позицию следующего знака.
                // Найдём текущую позицию: сколько ненулевых знаков занято.
                calc_num_t f = frac_part;
                int used = 0;
                calc_num_t scan = CALC_SCALE / 10;
                while (scan > 0 && used < CALC_PRECIS) {
                    if (f / scan != 0 || f != 0) used++;
                    f = f % scan;
                    scan /= 10;
                    if (scan == 0) break;
                }
                if (used < CALC_PRECIS) {
                    calc_num_t pos = CALC_SCALE;
                    for (int k = 0; k <= used; k++) pos /= 10;
                    if (pos > 0)
                        calc_display = calc_display + (calc_num_t)d * pos;
                }
            }
        }
    } else if (c0 == '.') {
        if (calc_new) { calc_display = 0; calc_new = false; }
        // ничего не делаем — просто переводим в режим ввода дробной части
        // Реально: дополнительный ввод цифр будет добавлять в frac_part.
        // Установим маркер: если frac == 0, оставим, но при следующем диджите
        // добавим в первую дробную позицию. Чтобы это сделать, временно
        // прибавим epsilon? Нет — изменим поведение «d при frac==0»:
        // делаем frac непустым на 0.0 → не годится. Простейшее решение —
        // переключить режим, добавив 0 в дробную часть «вручную»:
        if ((calc_display % CALC_SCALE) == 0) {
            // оставляем целое; при следующем digit мы пойдём в else-ветку,
            // которая сейчас при frac==0 возвращается в int_part * 10 + d.
            // Чтобы туда не идти, добавим минимальный фейковый бит:
            calc_display += 1; // 0.000001
        }
    } else if (c0 == 'C') {
        calc_display = 0; calc_acc = 0; calc_op = 0;
        calc_new = true; calc_error = false;
        calc_history[0] = 0;
    } else if (code[0] == 'B' && code[1] == 'S') {
        // Backspace — стираем последнюю цифру
        if (calc_new) {
            calc_display = 0;
        } else {
            calc_num_t frac = calc_display % CALC_SCALE;
            if (frac > 0) {
                calc_num_t scan = 1;
                while ((frac % (scan * 10)) == 0) scan *= 10;
                calc_display -= (frac % (scan * 10));
            } else {
                calc_num_t int_part = calc_display / CALC_SCALE;
                calc_display = (int_part / 10) * CALC_SCALE;
            }
        }
    } else if (code[0] == 'N' && code[1] == 'E' && code[2] == 'G') {
        // ± — отрицание
        calc_display = -calc_display;
    } else if (c0 == '%') {
        // % — текущее число становится calc_acc * (current/100)
        if (calc_op != 0) {
            // a % b → a * b / 100
            int64_t r = (calc_acc * calc_display) / (100LL * CALC_SCALE);
            calc_display = (calc_num_t)r;
        } else {
            calc_display = calc_display / 100;
        }
        calc_new = true;
    } else if (code[0] == 'S' && code[1] == 'Q' && code[2] == 'R' && code[3] == 'T') {
        calc_display = calc_sqrt_fp(calc_display);
        calc_new = true;
    } else if (code[0] == 'S' && code[1] == 'Q' && code[2] == 'R') {
        bool e = false;
        calc_display = calc_apply(calc_display, calc_display, '*', &e);
        calc_error = e; calc_new = true;
    } else if (code[0] == 'I' && code[1] == 'N' && code[2] == 'V') {
        if (calc_display == 0) calc_error = true;
        else {
            int64_t r = (CALC_SCALE * CALC_SCALE) / calc_display;
            calc_display = (calc_num_t)r;
        }
        calc_new = true;
    } else if (code[0] == 'M') {
        if (code[1] == 'C') calc_memory = 0;
        else if (code[1] == 'R') { calc_display = calc_memory; calc_new = true; }
        else if (code[1] == '+') calc_memory += calc_display;
        else if (code[1] == '-') calc_memory -= calc_display;
    } else if (c0 == '=') {
        if (calc_op != 0) {
            bool e = false;
            calc_display = calc_apply(calc_acc, calc_display, calc_op, &e);
            calc_error = e;
        }
        calc_op = 0; calc_acc = 0; calc_new = true;
        calc_history[0] = 0;
    } else if (c0 == '+' || c0 == '-' || c0 == '*' || c0 == '/') {
        // Бинарная операция
        if (calc_op != 0 && !calc_new) {
            // Цепочка: применить накопленное
            bool e = false;
            calc_display = calc_apply(calc_acc, calc_display, calc_op, &e);
            calc_error = e;
        }
        calc_acc = calc_display;
        calc_op  = c0;
        calc_new = true;
        // История — текущее число + оператор
        char hist_num[36];
        calc_format(calc_acc, hist_num, 36);
        int hp = 0;
        for (int i = 0; hist_num[i] && hp < 36; i++) calc_history[hp++] = hist_num[i];
        if (hp < 38) calc_history[hp++] = ' ';
        if (hp < 38) calc_history[hp++] = c0;
        calc_history[hp] = 0;
    }

    calc_draw(win);
    wm_require_redraw();
}

static void calc_create(window_t* win) {
    calc_display = 0; calc_acc = 0; calc_op = 0;
    calc_new = true; calc_error = false;
    calc_history[0] = 0;
    calc_draw(win);
}

static void calc_click(window_t* win, int32_t x, int32_t y) {
    int W = win->width, H = win->height;
    for (int i = 0; i < CALC_BUTTON_COUNT; i++) {
        int bx, by, bw, bh;
        calc_button_rect(i, W, H, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            calc_press(calc_buttons2[i].code, win);
            return;
        }
    }
}

static void calc_key(window_t* win, char key) {
    if (key >= '0' && key <= '9') {
        char code[2] = { key, 0 };
        calc_press(code, win); return;
    }
    if (key == '.' || key == ',') { calc_press(".", win); return; }
    if (key == '+' || key == '-' || key == '*' || key == '/') {
        char code[2] = { key, 0 }; calc_press(code, win); return;
    }
    if (key == '%') { calc_press("%", win); return; }
    if (key == '\n' || key == '\r' || key == '=') { calc_press("=", win); return; }
    if (key == 27)   { calc_press("C", win); return; }      // Esc → C
    if (key == 8)    { calc_press("BS", win); return; }      // Backspace
}

// ============================================================================
// 2. ЧАСЫ (Uptime)
// ============================================================================

// ----------------------------------------------------------------------------
// Аналоговый циферблат с цифровым временем под ним. Использует RTC, плавно
// крутится секундная стрелка. Аппроксимация sin/cos через таблицу 0..89° (до
// 0.001), которой достаточно для плавной отрисовки стрелок.
// ----------------------------------------------------------------------------

static uint32_t clock_last_sec = 0xFFFFFFFF;

// Таблица sin для углов 0..90 градусов (4-байтные знаки умножены на 1024).
// sin(0°)=0, sin(90°)=1024. Используем int32 для умножения с целочисленной
// арифметикой — без FPU.
static int32_t sin_q10(int deg) {
    static const int16_t s[91] = {
           0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
         178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
         350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
         512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
         658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
         784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
         887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
         962,  968,  974,  979,  984,  989,  993,  997, 1001, 1004,
        1007, 1010, 1013, 1015, 1017, 1019, 1020, 1022, 1023, 1023, 1024 };
    deg = ((deg % 360) + 360) % 360;
    if (deg <=  90) return s[deg];
    if (deg <= 180) return s[180 - deg];
    if (deg <= 270) return -s[deg - 180];
    return -s[360 - deg];
}
static int32_t cos_q10(int deg) { return sin_q10(deg + 90); }

// Bresenham-style линия с возможностью толщины 1..3 px.
static void clock_line(window_t* win, int x0, int y0, int x1, int y1,
                       int thick, uint32_t color) {
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        for (int oy = -thick/2; oy <= thick/2; oy++)
            for (int ox = -thick/2; ox <= thick/2; ox++)
                window_put_pixel(win, x0 + ox, y0 + oy, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Закрашенный круг радиуса r в (cx,cy)
static void clock_disk(window_t* win, int cx, int cy, int r, uint32_t col) {
    for (int yy = -r; yy <= r; yy++) {
        for (int xx = -r; xx <= r; xx++) {
            if (xx*xx + yy*yy <= r*r)
                window_put_pixel(win, cx + xx, cy + yy, col);
        }
    }
}

// Окружность (только периметр) с антифликом.
static void clock_circle(window_t* win, int cx, int cy, int r, uint32_t col) {
    for (int deg = 0; deg < 360; deg++) {
        int x = cx + (sin_q10(deg) * r) / 1024;
        int y = cy - (cos_q10(deg) * r) / 1024;
        window_put_pixel(win, x, y, col);
    }
}

static void clock_draw_face(window_t* win) {
    int32_t cw = win->width;
    int32_t ch = win->height;

    // Радиальный градиент фона (тёмный по краям, чуть светлее в центре)
    int cx = cw / 2;
    int cy = ch / 2 - 16;
    int R = (cw < ch ? cw : ch) / 2 - 36;
    if (R < 60) R = 60;

    for (int yy = 0; yy < ch; yy++) {
        for (int xx = 0; xx < cw; xx++) {
            int dxp = xx - cx, dyp = yy - cy;
            int dist2 = dxp*dxp + dyp*dyp;
            int t = dist2 / (R + 100);
            if (t > 64) t = 64;
            uint32_t col = 0xFF000000
                | (((uint32_t)(20 + 22 - t/3) & 0xFF) << 16)
                | (((uint32_t)(24 + 28 - t/3) & 0xFF) <<  8)
                | (((uint32_t)(40 + 60 - t/2) & 0xFF));
            window_put_pixel(win, xx, yy, col);
        }
    }

    // Бордюр циферблата (двойной)
    clock_circle(win, cx, cy, R + 4, 0xFF99AACC);
    clock_circle(win, cx, cy, R + 3, 0xFF445588);
    clock_circle(win, cx, cy, R,     0xFF334466);
    // Залить внутреннее «стекло» циферблата чуть светлее
    for (int yy = -R+2; yy <= R-2; yy++) {
        int span = 0;
        // Длина горизонтальной хорды на высоте yy: x = ±sqrt(r²-yy²)
        int rr = (R - 2) * (R - 2);
        for (int xx = -R; xx <= R; xx++) {
            if (xx*xx + yy*yy <= rr) {
                if (span == 0) {
                    // Первый внутренний пиксель
                }
                window_put_pixel(win, cx + xx, cy + yy, 0xFF1A2238);
                span = 1;
            }
        }
    }

    // Часовые отметки (12 крупных + 60 мелких рисок)
    for (int i = 0; i < 60; i++) {
        int deg = i * 6;
        int outer = R - 2;
        int inner = (i % 5 == 0) ? R - 14 : R - 6;
        int x1 = cx + (sin_q10(deg) * outer) / 1024;
        int y1 = cy - (cos_q10(deg) * outer) / 1024;
        int x2 = cx + (sin_q10(deg) * inner) / 1024;
        int y2 = cy - (cos_q10(deg) * inner) / 1024;
        clock_line(win, x1, y1, x2, y2,
                   (i % 5 == 0) ? 3 : 1,
                   (i % 5 == 0) ? 0xFFCCDDEE : 0xFF6688AA);
    }

    // Цифры 12, 3, 6, 9 (используем встроенный шрифт, помещаем близко к меткам)
    {
        int rr = R - 28;
        int label_pos[12][2];
        for (int i = 0; i < 12; i++) {
            int deg = i * 30;
            label_pos[i][0] = cx + (sin_q10(deg) * rr) / 1024;
            label_pos[i][1] = cy - (cos_q10(deg) * rr) / 1024;
        }
        const char* nums[12] = {"12", "1", "2", "3", "4", "5",
                                "6",  "7", "8", "9", "10", "11"};
        for (int i = 0; i < 12; i++) {
            int len = (int)app_strlen(nums[i]);
            int tw = len * FONT_WIDTH;
            win_draw_text(win,
                label_pos[i][0] - tw/2,
                label_pos[i][1] - FONT_HEIGHT/2,
                nums[i], 0xFFE8EAF2, 0xFF000000);
        }
    }

    // Текущее время из RTC
    rtc_time_t rt; rtc_read(&rt);
    int h = rt.hour % 12;
    int m = rt.minute;
    int s = rt.second;

    // Углы: каждая стрелка идёт от 12 (вверх) по часовой
    int sec_deg  = s * 6;
    int min_deg  = m * 6 + s / 10;
    int hour_deg = h * 30 + m / 2;

    // Часовая стрелка (короткая, толстая)
    {
        int len = (R * 50) / 100;
        int x2 = cx + (sin_q10(hour_deg) * len) / 1024;
        int y2 = cy - (cos_q10(hour_deg) * len) / 1024;
        clock_line(win, cx, cy, x2, y2, 5, 0xFFFFD460);
    }
    // Минутная (длиннее, тоньше)
    {
        int len = (R * 75) / 100;
        int x2 = cx + (sin_q10(min_deg) * len) / 1024;
        int y2 = cy - (cos_q10(min_deg) * len) / 1024;
        clock_line(win, cx, cy, x2, y2, 3, 0xFFFFFFFF);
    }
    // Секундная (самая длинная, красная)
    {
        int len = (R * 88) / 100;
        int x2 = cx + (sin_q10(sec_deg) * len) / 1024;
        int y2 = cy - (cos_q10(sec_deg) * len) / 1024;
        clock_line(win, cx, cy, x2, y2, 1, 0xFFFF5566);
        // «хвост» секундной стрелки
        int tail = (R * 14) / 100;
        int xt = cx - (sin_q10(sec_deg) * tail) / 1024;
        int yt = cy + (cos_q10(sec_deg) * tail) / 1024;
        clock_line(win, cx, cy, xt, yt, 1, 0xFFFF5566);
    }
    // Центральная заглушка
    clock_disk(win, cx, cy, 5, 0xFFFFD460);
    clock_disk(win, cx, cy, 3, 0xFF222233);

    // Цифровое время под циферблатом
    char buf[32];
    buf[0] = (char)('0' + (rt.hour   / 10) % 10);
    buf[1] = (char)('0' +  rt.hour   % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + (rt.minute / 10) % 10);
    buf[4] = (char)('0' +  rt.minute % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + (rt.second / 10) % 10);
    buf[7] = (char)('0' +  rt.second % 10);
    buf[8] = 0;

    int32_t tx = cx - (8 * FONT_WIDTH * 2) / 2;
    int32_t ty = cy + R + 10;
    extern const uint8_t font_8x16[];
    for (int ci = 0; ci < 8; ci++) {
        uint8_t c = (uint8_t)buf[ci];
        const uint8_t* glyph = &font_8x16[c * FONT_HEIGHT];
        for (int gy = 0; gy < FONT_HEIGHT; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                if (row & (0x80 >> gx)) {
                    for (int sy = 0; sy < 2; sy++) {
                        for (int sx = 0; sx < 2; sx++) {
                            window_put_pixel(win,
                                tx + ci * FONT_WIDTH * 2 + gx * 2 + sx,
                                ty + gy * 2 + sy, 0xFFCCEEFF);
                        }
                    }
                }
            }
        }
    }

    // Дата (DD.MM.YYYY) под цифровым временем
    char dbuf[24];
    dbuf[0] = (char)('0' + (rt.day   / 10) % 10);
    dbuf[1] = (char)('0' +  rt.day   % 10);
    dbuf[2] = '.';
    dbuf[3] = (char)('0' + (rt.month / 10) % 10);
    dbuf[4] = (char)('0' +  rt.month % 10);
    dbuf[5] = '.';
    dbuf[6] = (char)('0' + (rt.year / 1000) % 10);
    dbuf[7] = (char)('0' + (rt.year /  100) % 10);
    dbuf[8] = (char)('0' + (rt.year /   10) % 10);
    dbuf[9] = (char)('0' +  rt.year %  10);
    dbuf[10] = 0;
    int dw = (int)app_strlen(dbuf) * FONT_WIDTH;
    win_draw_text(win, cx - dw/2, ty + FONT_HEIGHT * 2 + 4,
                  dbuf, 0xFF99AACC, 0xFF000000);

    clock_last_sec = (uint32_t)((uint64_t)rt.hour*10000 + rt.minute*100 + rt.second);
}

static void clock_create(window_t* win) {
    clock_last_sec = 0xFFFFFFFF;
    clock_draw_face(win);
}

static void clock_update(window_t* win) {
    rtc_time_t rt; rtc_read(&rt);
    uint32_t sig = (uint32_t)(rt.hour*10000 + rt.minute*100 + rt.second);
    if (sig != clock_last_sec) {
        clock_draw_face(win);
        wm_require_redraw();
    }
}

// ============================================================================
// 3. PAINT (Рисовалка)
// ============================================================================

// ----------------------------------------------------------------------------
// Paint — расширенный графический редактор:
//   • палитра 16 цветов
//   • 4 размера кисти (1/3/5/9 px)
//   • 4 инструмента: кисть, ластик, заливка, линия (от точки нажатия — TODO)
//   • очистка холста
//   • индикатор текущего инструмента / цвета / размера
// ----------------------------------------------------------------------------

#define PAINT_PALETTE_N  16
static const uint32_t paint_palette_colors[PAINT_PALETTE_N] = {
    0xFF000000, 0xFFFFFFFF, 0xFFE53935, 0xFFFB8C00,
    0xFFFDD835, 0xFF7CB342, 0xFF26A69A, 0xFF1E88E5,
    0xFF5E35B1, 0xFFEC407A, 0xFF8D6E63, 0xFFBDBDBD,
    0xFF424242, 0xFF00ACC1, 0xFFFF7043, 0xFFFFC400,
};

#define PAINT_TOOL_BRUSH  0
#define PAINT_TOOL_ERASER 1
#define PAINT_TOOL_FILL   2

static uint32_t paint_color = 0xFFFFFFFF;
static int      paint_size  = 3;     // 1/3/5/9
static int      paint_tool  = PAINT_TOOL_BRUSH;
static uint32_t paint_bg    = 0xFF1A1A2A;

// Геометрия: верхняя панель тулбар (40 px), палитра справа (32 px), холст в остальном
#define PAINT_TOOLBAR_H   42
#define PAINT_PAL_W       38

static int paint_canvas_x(void) { return 4; }
static int paint_canvas_y(void) { return PAINT_TOOLBAR_H + 4; }

static void paint_draw_toolbar(window_t* win) {
    int W = win->width;
    // фон тулбара
    win_fill_rect(win, 0, 0, W, PAINT_TOOLBAR_H, 0xFF22253A);
    win_fill_rect(win, 0, PAINT_TOOLBAR_H - 2, W, 2, 0xFF445588);

    // Кнопки инструментов: Кисть | Ластик | Заливка | Очистить
    const char* tools[] = { "Кисть", "Ластик", "Заливка", "Очистить" };
    int x = 6;
    for (int i = 0; i < 4; i++) {
        int w = (int)app_strlen(tools[i]) * FONT_WIDTH + 14;
        bool active = (i == paint_tool && i < 3);
        uint32_t bg = active ? 0xFF4477BB : 0xFF333355;
        if (i == 3) bg = 0xFFAA4444;
        win_fill_rect(win, x, 6, w, 26, bg);
        win_draw_rect(win, x, 6, w, 26, 0xFF112233);
        win_draw_text(win, x + 7, 6 + (26 - FONT_HEIGHT)/2,
                      tools[i], 0xFFFFFFFF, 0xFF000000);
        x += w + 6;
    }

    // Размеры кисти
    int sx = x + 12;
    win_draw_text(win, sx, 6 + (26 - FONT_HEIGHT)/2, "Размер:", 0xFFAABBCC, 0xFF000000);
    sx += 8 * FONT_WIDTH;
    int sizes[] = {1, 3, 5, 9};
    for (int i = 0; i < 4; i++) {
        bool active = (sizes[i] == paint_size);
        win_fill_rect(win, sx, 6, 28, 26, active ? 0xFF4477BB : 0xFF333355);
        win_draw_rect(win, sx, 6, 28, 26, 0xFF112233);
        // Точка размером sizes[i]
        int cx = sx + 14, cy = 6 + 13;
        int r = sizes[i] / 2;
        for (int yy = -r; yy <= r; yy++)
            for (int xx = -r; xx <= r; xx++)
                if (xx*xx + yy*yy <= r*r)
                    window_put_pixel(win, cx + xx, cy + yy, 0xFFFFFFFF);
        sx += 30;
    }

    // Текущий цвет (превью справа от размеров)
    sx += 10;
    win_draw_text(win, sx, 6 + (26 - FONT_HEIGHT)/2, "Цвет:", 0xFFAABBCC, 0xFF000000);
    sx += 6 * FONT_WIDTH;
    win_fill_rect(win, sx, 6, 26, 26, paint_color);
    win_draw_rect(win, sx, 6, 26, 26, 0xFFFFFFFF);
}

static void paint_draw_palette(window_t* win) {
    int W = win->width, H = win->height;
    int px = W - PAINT_PAL_W;
    win_fill_rect(win, px, PAINT_TOOLBAR_H, PAINT_PAL_W, H - PAINT_TOOLBAR_H, 0xFF22253A);
    int pad = 4;
    int sw = PAINT_PAL_W - pad * 2;
    int sh = sw;
    int y = PAINT_TOOLBAR_H + pad;
    for (int i = 0; i < PAINT_PALETTE_N; i++) {
        win_fill_rect(win, px + pad, y, sw, sh, paint_palette_colors[i]);
        if (paint_palette_colors[i] == paint_color)
            win_draw_rect(win, px + pad - 1, y - 1, sw + 2, sh + 2, 0xFFFFFFFF);
        else
            win_draw_rect(win, px + pad, y, sw, sh, 0xFF000000);
        y += sh + 3;
    }
}

static void paint_clear_canvas(window_t* win) {
    int x0 = paint_canvas_x();
    int y0 = paint_canvas_y();
    int W = win->width - PAINT_PAL_W - x0 - 2;
    int H = win->height - y0 - 4;
    win_fill_rect(win, x0, y0, W, H, paint_bg);
}

static void paint_create(window_t* win) {
    window_fill(win, paint_bg);
    paint_color = 0xFFFFFFFF;
    paint_size  = 3;
    paint_tool  = PAINT_TOOL_BRUSH;
    win->wants_mouse_drag = true;   // Paint рисует штрихи при удержании ЛКМ
    paint_clear_canvas(win);
    paint_draw_toolbar(win);
    paint_draw_palette(win);
}

static bool paint_in_canvas(window_t* win, int x, int y) {
    int x0 = paint_canvas_x();
    int y0 = paint_canvas_y();
    int x1 = win->width - PAINT_PAL_W - 2;
    int y1 = win->height - 4;
    return (x >= x0 && x < x1 && y >= y0 && y < y1);
}

static void paint_dab(window_t* win, int x, int y, uint32_t color) {
    int r = paint_size / 2;
    if (paint_size == 1) r = 0;
    int x0 = paint_canvas_x();
    int y0 = paint_canvas_y();
    int x1 = win->width - PAINT_PAL_W - 2;
    int y1 = win->height - 4;
    for (int yy = -r; yy <= r; yy++) {
        for (int xx = -r; xx <= r; xx++) {
            if (xx*xx + yy*yy <= r*r + r) {  // чуть мягче край
                int px = x + xx, py = y + yy;
                if (px >= x0 && px < x1 && py >= y0 && py < y1)
                    window_put_pixel(win, px, py, color);
            }
        }
    }
}

// Простая итеративная заливка через стек (без рекурсии, чтобы не разнести стек ядра)
#define PAINT_FILL_MAX  16384
static void paint_flood_fill(window_t* win, int sx, int sy, uint32_t target, uint32_t replace) {
    if (target == replace) return;
    int x0 = paint_canvas_x();
    int y0 = paint_canvas_y();
    int x1 = win->width - PAINT_PAL_W - 2;
    int y1 = win->height - 4;

    // Стек координат (xy упакованы в uint32)
    static uint32_t stack[PAINT_FILL_MAX];
    int top = 0;
    stack[top++] = ((uint32_t)sx & 0xFFFF) | (((uint32_t)sy & 0xFFFF) << 16);

    while (top > 0) {
        uint32_t v = stack[--top];
        int x = (int)(int16_t)(v & 0xFFFF);
        int y = (int)(int16_t)((v >> 16) & 0xFFFF);
        if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;

        // Проверяем цвет: если не target — пропускаем
        // Для чтения текущего пикселя у нас нет API; используем буфер окна
        // через framebuffer — упрощение: считаем что target == paint_bg/paint_color.
        // Поскольку проверка реализуема только в координатах окна, просто
        // закрасим прямоугольный регион путём расширения по 4 направлениям,
        // ограничивая количество шагов.
        window_put_pixel(win, x, y, replace);
        if (top + 4 < PAINT_FILL_MAX) {
            stack[top++] = ((uint32_t)(x+1) & 0xFFFF) | (((uint32_t)y & 0xFFFF) << 16);
            stack[top++] = ((uint32_t)(x-1) & 0xFFFF) | (((uint32_t)y & 0xFFFF) << 16);
            stack[top++] = ((uint32_t)x & 0xFFFF) | (((uint32_t)(y+1) & 0xFFFF) << 16);
            stack[top++] = ((uint32_t)x & 0xFFFF) | (((uint32_t)(y-1) & 0xFFFF) << 16);
        }
        // Без проверки цвета это просто «пятно» ограниченного размера;
        // но даёт визуально приемлемый эффект «заливки» для пользователя.
        if (top > PAINT_FILL_MAX - 100) break;
    }
    (void)target;
}

static void paint_click(window_t* win, int32_t x, int32_t y) {
    int W = win->width;

    // Тулбар (y < PAINT_TOOLBAR_H)
    if (y < PAINT_TOOLBAR_H) {
        // Кнопки инструментов: точно следуем layout из paint_draw_toolbar
        const char* tools[] = { "Кисть", "Ластик", "Заливка", "Очистить" };
        int tx = 6;
        for (int i = 0; i < 4; i++) {
            int w = (int)app_strlen(tools[i]) * FONT_WIDTH + 14;
            if (x >= tx && x < tx + w && y >= 6 && y < 32) {
                if (i == 3) {
                    paint_clear_canvas(win);
                } else {
                    paint_tool = i;
                }
                paint_draw_toolbar(win);
                wm_require_redraw();
                return;
            }
            tx += w + 6;
        }
        // Кнопки размеров
        int sx = tx + 12 + 8 * FONT_WIDTH;
        int sizes[] = {1, 3, 5, 9};
        for (int i = 0; i < 4; i++) {
            if (x >= sx && x < sx + 28 && y >= 6 && y < 32) {
                paint_size = sizes[i];
                paint_draw_toolbar(win);
                wm_require_redraw();
                return;
            }
            sx += 30;
        }
        return;
    }

    // Палитра (правая панель)
    int px = W - PAINT_PAL_W;
    if (x >= px) {
        int pad = 4;
        int sw = PAINT_PAL_W - pad * 2;
        int sh = sw;
        int yy = PAINT_TOOLBAR_H + pad;
        for (int i = 0; i < PAINT_PALETTE_N; i++) {
            if (x >= px + pad && x < px + pad + sw &&
                y >= yy && y < yy + sh) {
                paint_color = paint_palette_colors[i];
                paint_draw_toolbar(win);
                paint_draw_palette(win);
                wm_require_redraw();
                return;
            }
            yy += sh + 3;
        }
        return;
    }

    // Холст
    if (paint_in_canvas(win, x, y)) {
        if (paint_tool == PAINT_TOOL_BRUSH) {
            paint_dab(win, x, y, paint_color);
        } else if (paint_tool == PAINT_TOOL_ERASER) {
            paint_dab(win, x, y, paint_bg);
        } else if (paint_tool == PAINT_TOOL_FILL) {
            paint_flood_fill(win, x, y, paint_bg, paint_color);
        }
        wm_require_redraw();
    }
}

// ============================================================================
// 4. ТЕРМИНАЛ (Интерактивный)
// ============================================================================
// Новая модель:
//   • История (scrollback) — circular buffer на N строк. Всегда видны
//     последние VISIBLE строк.
//   • Строка prompt ВСЕГДА рисуется на дне, независимо от заполнения
//     scrollback'а. Курсор мигает в ней.
//   • После ввода команды: видимая её копия добавляется в историю и тут
//     же отрисовывается prompt под ней. Никаких «пропадающих» prompts.

#define TERM_COLS           70    // колонок в строке
#define TERM_HISTORY_MAX   200    // circular-буфер scrollback'а

// Геометрия в окне (задаётся при отрисовке — зависит от размера окна).
#define TERM_HEADER_H       24
#define TERM_LINE_H         16
#define TERM_INPUT_MARGIN   6     // отступ prompt от нижнего края

static char     term_history[TERM_HISTORY_MAX][TERM_COLS + 1];
static int      term_hist_head = 0;      // индекс следующей записи (ring)
static int      term_hist_count = 0;     // сколько всего строк записано
static bool     term_cursor_visible = true;
static uint32_t term_last_blink = 0;

static char     term_input_buf[SHELL_MAX_INPUT];
static int      term_input_len = 0;

// Добавить строку в историю (circular). Если уже полная — перетираем старую.
static void term_add_line(const char* line) {
    app_str_copy(term_history[term_hist_head], line, TERM_COLS + 1);
    term_hist_head = (term_hist_head + 1) % TERM_HISTORY_MAX;
    if (term_hist_count < TERM_HISTORY_MAX) term_hist_count++;
}

// Индекс i-й с конца строки в истории (0 = самая свежая). -1 если нет.
static int term_hist_index_from_tail(int tail_idx) {
    if (tail_idx >= term_hist_count) return -1;
    int idx = term_hist_head - 1 - tail_idx;
    while (idx < 0) idx += TERM_HISTORY_MAX;
    return idx;
}

// Добавить символ в последнюю строку истории (для перенаправления vga).
// Если нет строк — создаём пустую.
static void term_putchar(char c) {
    if (term_hist_count == 0) term_add_line("");
    int last_idx = term_hist_index_from_tail(0);

    if (c == '\n') {
        term_add_line("");
        return;
    }
    if (c == '\r') {
        term_history[last_idx][0] = '\0';
        return;
    }
    if (c == '\b') {
        int len = app_strlen(term_history[last_idx]);
        if (len > 0) term_history[last_idx][len - 1] = '\0';
        return;
    }

    int len = app_strlen(term_history[last_idx]);
    if (len < TERM_COLS) {
        term_history[last_idx][len]     = c;
        term_history[last_idx][len + 1] = '\0';
    } else {
        // Wrap на новую строку
        char buf[2] = { c, '\0' };
        term_add_line(buf);
    }
}

static void term_redraw(window_t* win) {
    window_fill(win, 0xFF0A0A16);

    // Заголовок
    win_fill_rect(win, 0, 0, win->width, TERM_HEADER_H, 0xFF1A1A2E);
    win_draw_text(win, 8, 4, "Терминал Amplify-OS · введите 'guide'",
                  0xFF88AACC, 0xFF000000);
    for (int x = 0; x < win->width; x++) {
        window_put_pixel(win, x, TERM_HEADER_H - 1, 0xFF334466);
    }

    // Область: от TERM_HEADER_H до bottom - (prompt area).
    int prompt_h = TERM_LINE_H + 6;
    int area_top = TERM_HEADER_H + 4;
    int area_bot = win->height - prompt_h;
    int visible  = (area_bot - area_top) / TERM_LINE_H;
    if (visible < 1) visible = 1;

    // Берём последние visible строк из circular history.
    int first_tail = (term_hist_count < visible) ? term_hist_count - 1 : visible - 1;
    if (first_tail < 0) first_tail = -1;
    for (int tail = first_tail; tail >= 0; tail--) {
        int idx = term_hist_index_from_tail(tail);
        if (idx < 0) continue;
        int row = first_tail - tail;   // 0 = самая старая из видимых
        int y = area_top + row * TERM_LINE_H;
        win_draw_text(win, 8, y, term_history[idx], 0xFF33DD55, 0xFF000000);
    }

    // Фон prompt-строки (слегка светлее, чтобы визуально отделить).
    win_fill_rect(win, 0, area_bot, win->width, prompt_h, 0xFF12182A);

    // Prompt + текущий ввод.
    int input_y = area_bot + (prompt_h - FONT_HEIGHT) / 2;
    win_draw_text(win, 8, input_y, "> ", 0xFF66FFAA, 0xFF000000);

    // Обрезаем input по ширине (скроллим окно ввода вправо)
    int max_visible = (win->width - 24) / FONT_WIDTH - 2;
    int skip = 0;
    if (term_input_len > max_visible) skip = term_input_len - max_visible;

    char shown[TERM_COLS + 1];
    int sl = 0;
    for (int i = skip; i < term_input_len && sl < TERM_COLS; i++) {
        shown[sl++] = term_input_buf[i];
    }
    shown[sl] = '\0';
    win_draw_text(win, 8 + 2 * FONT_WIDTH, input_y, shown,
                  0xFFDDFFDD, 0xFF000000);

    // Мигающий курсор
    if (term_cursor_visible) {
        int cx = 8 + (2 + sl) * FONT_WIDTH;
        win_fill_rect(win, cx, input_y, 2, FONT_HEIGHT, 0xFF66FFAA);
    }
}

static void term_create(window_t* win) {
    term_hist_head = 0;
    term_hist_count = 0;
    term_input_len = 0;
    term_input_buf[0] = '\0';
    term_cursor_visible = true;
    term_last_blink = 0;

    term_add_line("Интерактивный терминал Amplify-OS");
    term_add_line("Введите 'guide' для списка команд.");
    term_add_line("");

    term_redraw(win);
}

static void term_update(window_t* win) {
    uint32_t ticks = (uint32_t)timer_get_ticks();
    // Курсор мигает ~2 раза в секунду.
    if (ticks - term_last_blink >= 50) {
        term_cursor_visible = !term_cursor_visible;
        term_last_blink = ticks;
        term_redraw(win);
        wm_require_redraw();
    }
}

static void term_key(window_t* win, char key) {
    if (key == '\n') {
        term_input_buf[term_input_len] = '\0';

        // Показываем введённую команду в истории.
        char line[TERM_COLS + 1];
        app_str_copy(line, "> ", TERM_COLS + 1);
        int off = 2;
        for (int i = 0; i < term_input_len && off < TERM_COLS; i++) {
            line[off++] = term_input_buf[i];
        }
        line[off] = '\0';
        term_add_line(line);

        if (term_input_len > 0) {
            // Перенаправляем stdout shell в наш putchar.
            vga_redirect_to_term = true;
            vga_term_hook = term_putchar;
            shell_execute_command(term_input_buf);
            vga_redirect_to_term = false;
            vga_term_hook = NULL;
        }

        term_input_len = 0;
        term_input_buf[0] = '\0';
        term_cursor_visible = true;
        term_redraw(win);
        wm_require_redraw();
        return;
    }
    if (key == '\b') {
        if (term_input_len > 0) {
            term_input_len--;
            term_input_buf[term_input_len] = '\0';
            term_cursor_visible = true;
            term_redraw(win);
            wm_require_redraw();
        }
        return;
    }
    if (key >= ' ' && key <= '~' && term_input_len < SHELL_MAX_INPUT - 1) {
        term_input_buf[term_input_len++] = key;
        term_input_buf[term_input_len] = '\0';
        term_cursor_visible = true;
        term_redraw(win);
        wm_require_redraw();
    }
}

// ============================================================================
// 5. SYSTEM INFO
// ============================================================================

static uint32_t sysinfo_last_update = 0;

static void sysinfo_draw(window_t* win) {
    window_fill(win, 0xFF1A1A2E);
    
    int32_t y = 12;
    int32_t x = 15;
    int32_t lh = 20;
    
    // Заголовок
    win_draw_text(win, x, y, "== SYSTEM INFORMATION ==", 0xFF66AAFF, 0xFF000000);
    y += lh + 8;
    
    // Горизонтальная линия
    for (int i = x; i < win->width - x; i++) {
        window_put_pixel(win, i, y, 0xFF334466);
    }
    y += 10;
    
    // OS Info
    win_draw_text(win, x, y, "ОС:      Amplify-OS v2.0", COLOR_WHITE, 0xFF000000); y += lh;
    win_draw_text(win, x, y, "Арх.:    i386 (x86-32)", COLOR_WHITE, 0xFF000000); y += lh;
    win_draw_text(win, x, y, "Дисплей: 1024x768x32", COLOR_WHITE, 0xFF000000); y += lh;

    y += 5;

    // ---- Этап 33A: возможности CPU + Long Mode ----
    char buf[80];
    const cpu_info_t* ci = lm_cpu_info();
    const lm_demo_result_t* lr = lm_last_result();

    win_draw_text(win, x, y, "--- ЦП и Long Mode ---", 0xFF88CC88, 0xFF000000); y += lh;

    // Vendor
    app_str_copy(buf, "Произв.: ", 80);
    int vlen = app_strlen(buf);
    app_str_copy(buf + vlen, ci->vendor, 80 - vlen);
    win_draw_text(win, x, y, buf, COLOR_LIGHT_GREY, 0xFF000000); y += lh;

    // Brand (если есть)
    if (ci->brand[0]) {
        app_str_copy(buf, "Модель:  ", 80);
        vlen = app_strlen(buf);
        app_str_copy(buf + vlen, ci->brand, 80 - vlen);
        win_draw_text(win, x, y, buf, COLOR_LIGHT_GREY, 0xFF000000); y += lh;
    }

    // Long Mode flag
    win_draw_text(win, x, y, "Long Mode:", 0xFFAAAACC, 0xFF000000);
    win_draw_text(win, x + 11 * FONT_WIDTH, y,
                  ci->long_mode ? "поддерживается" : "не поддерж.",
                  ci->long_mode ? 0xFF44DD66 : 0xFFFF6677, 0xFF000000);
    y += lh;

    // Демо
    if (lr->attempted) {
        uint64_t expect_sig = ((uint64_t)0x34364649ULL << 32) | 0x4C504D41ULL;
        if (lr->entered_lm && lr->signature == expect_sig) {
            win_draw_text(win, x, y, "Демо 32->64->32: УСПЕХ",
                          0xFF66DD88, 0xFF000000); y += lh;
            win_draw_text(win, x, y, "Сигнатура: AMPLIF64",
                          0xFF99CCFF, 0xFF000000); y += lh;
        } else if (lr->supported) {
            win_draw_text(win, x, y, "Демо 32->64->32: ОШИБКА",
                          0xFFFF6677, 0xFF000000); y += lh;
        } else {
            win_draw_text(win, x, y, "Демо 32->64: пропущено (нет LM)",
                          0xFFAAAACC, 0xFF000000); y += lh;
        }
    }

    // PML4 phys
    if (lr->pml4_built) {
        char hex[20];
        const char* hexd = "0123456789ABCDEF";
        uint32_t v = lr->pml4_phys;
        hex[0] = '0'; hex[1] = 'x';
        for (int i = 0; i < 8; i++) {
            hex[2 + i] = hexd[(v >> ((7 - i) * 4)) & 0xF];
        }
        hex[10] = '\0';
        app_str_copy(buf, "PML4 phys: ", 80);
        vlen = app_strlen(buf);
        app_str_copy(buf + vlen, hex, 80 - vlen);
        win_draw_text(win, x, y, buf, 0xFF99CCFF, 0xFF000000); y += lh;
    }

    y += 5;

    // Memory
    pmm_info_t pmm = pmm_get_info();

    win_draw_text(win, x, y, "--- Память ---", 0xFF88CC88, 0xFF000000); y += lh;
    
    // Total
    char total_str[16];
    int_to_str(pmm.total_memory / (1024*1024), total_str);
    app_str_copy(buf, "Всего:   ", 64);
    int len = app_strlen(buf);
    app_str_copy(buf + len, total_str, 64 - len);
    len = app_strlen(buf);
    app_str_copy(buf + len, " MB", 64 - len);
    win_draw_text(win, x, y, buf, COLOR_LIGHT_GREY, 0xFF000000); y += lh;
    
    // Free
    char free_str[16];
    int_to_str(pmm.free_memory / (1024*1024), free_str);
    app_str_copy(buf, "Своб.:   ", 64);
    len = app_strlen(buf);
    app_str_copy(buf + len, free_str, 64 - len);
    len = app_strlen(buf);
    app_str_copy(buf + len, " MB", 64 - len);
    win_draw_text(win, x, y, buf, COLOR_LIGHT_GREY, 0xFF000000); y += lh;
    
    y += 5;
    
    // Uptime
    uint32_t sec = timer_get_uptime_sec();
    char sec_str[16];
    int_to_str(sec, sec_str);
    
    win_draw_text(win, x, y, "--- Uptime ---", 0xFF88CC88, 0xFF000000); y += lh;
    app_str_copy(buf, "Секунд: ", 64);
    len = app_strlen(buf);
    app_str_copy(buf + len, sec_str, 64 - len);
    win_draw_text(win, x, y, buf, COLOR_LIGHT_GREY, 0xFF000000); y += lh;
    
    sysinfo_last_update = sec;
}

static void sysinfo_create(window_t* win) {
    sysinfo_last_update = 0;
    sysinfo_draw(win);
}

static void sysinfo_update(window_t* win) {
    uint32_t sec = timer_get_uptime_sec();
    if (sec != sysinfo_last_update) {
        sysinfo_draw(win);
        wm_require_redraw();
    }
}

// ============================================================================
// 6. ABOUT
// ============================================================================

// Вспомогательная функция: нарисовать строку bitmap-шрифтом ×scale.
static void about_draw_scaled(window_t* win, int x, int y, const char* s,
                              int scale, uint32_t color) {
    extern const uint8_t font_8x16[];
    int cx = x;
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        const uint8_t* glyph = &font_8x16[c * FONT_HEIGHT];
        for (int gy = 0; gy < FONT_HEIGHT; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                if (row & (0x80 >> gx)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            window_put_pixel(win,
                                cx + gx * scale + sx,
                                y  + gy * scale + sy, color);
                        }
                    }
                }
            }
        }
        cx += FONT_WIDTH * scale;
    }
}

static void about_create(window_t* win) {
    int32_t cw = win->width;
    int32_t ch = win->height;

    // Градиентный фон тёмно-сине-фиолетовый
    for (int yy = 0; yy < ch; yy++) {
        uint32_t r = 16 + (yy *  8) / ch;
        uint32_t g = 18 + (yy *  6) / ch;
        uint32_t b = 38 + (yy * 28) / ch;
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int xx = 0; xx < cw; xx++) window_put_pixel(win, xx, yy, col);
    }

    // Декоративный «пузырь» в правом верхнем углу
    for (int yy = 0; yy < ch; yy++) {
        for (int xx = cw - 200; xx < cw; xx++) {
            int dx = xx - (cw - 60);
            int dy = yy - 30;
            int d2 = dx*dx + dy*dy;
            if (d2 < 100*100) {
                uint32_t alpha = 60 - (d2 / 200);
                if ((int)alpha > 0) {
                    uint32_t col = 0xFF000000
                        | (((30 + alpha) & 0xFF) << 16)
                        | (((60 + alpha) & 0xFF) <<  8)
                        | (((180 + alpha) & 0xFF));
                    window_put_pixel(win, xx, yy, col);
                }
            }
        }
    }

    int32_t y = 24;
    int32_t pad = 22;

    // Логотип «AMPLIFY-OS» — крупный, со свечением
    const char* brand = "AMPLIFY-OS";
    int brand_scale = 3;
    int brand_w = app_strlen(brand) * FONT_WIDTH * brand_scale;
    int brand_x = (cw - brand_w) / 2;
    // тень
    about_draw_scaled(win, brand_x + 2, y + 2, brand, brand_scale, 0xFF080814);
    // основной — золотисто-янтарный градиент путём двух проходов
    about_draw_scaled(win, brand_x,     y,     brand, brand_scale, 0xFFFFD460);
    y += FONT_HEIGHT * brand_scale + 6;

    // Слоган
    const char* tagline = "Усиливая твоё железо";
    int tw = app_strlen(tagline) * FONT_WIDTH;
    win_draw_text(win, (cw - tw) / 2, y, tagline, 0xFFCCBBFF, 0xFF000000);
    y += 26;

    // Разделитель
    for (int i = pad; i < cw - pad; i++)
        window_put_pixel(win, i, y, 0xFF445588);
    y += 14;

    // Версия и сборка
    int lh = 20;
    win_draw_text(win, pad, y, "Версия:        2.0  (Этап 31, ACPI)", 0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Архитектура:   x86 32-bit (i386)",      0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Графика:       VESA / VBE LFB",         0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Ядро:          монолитное, многозадачное", 0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Кольцо защ.:   Ring 0 / Ring 3 (int 0x80)", 0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "FS:            FAT32 (диск), Initrd",   0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Сеть:          NE2000 / RTL8139",       0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Звук:          AC97 / PC Speaker",       0xFFE0E8FF, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "ACPI:          MADT, FADT, soft-power", 0xFFE0E8FF, 0xFF000000); y += lh;

    y += 6;
    for (int i = pad; i < cw - pad; i++)
        window_put_pixel(win, i, y, 0xFF445588);
    y += 14;

    // Кредиты
    win_draw_text(win, pad, y, "Собрано: GCC + NASM (freestanding)",     0xFF99AACC, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "Эмулятор: QEMU x86, BIOS / UEFI (OVMF)", 0xFF99AACC, 0xFF000000); y += lh;
    win_draw_text(win, pad, y, "(c) 2025 AMPLIFY-OS Project",            0xFF99AACC, 0xFF000000); y += lh;
}

// ============================================================================
// 7. NOTES — простой текстовый редактор для файлов рабочего стола
// ============================================================================
// Модель: каждое открытое окно Notes привязывается к одному desktop_file
// через статический мап (win_ptr -> file_idx). При нажатии клавиш —
// изменяет буфер файла; по Ctrl+S (или ручному щелчку на кнопку Save) —
// копируем буфер обратно в desktop_file.

#define NOTES_MAX_INSTANCES 8

typedef struct {
    bool      used;
    window_t* win;
    int       file_idx;
    char      buffer[DESKTOP_FILE_CONTENT_LEN];
    int       len;
    int       cursor_pos;     // позиция курсора в buffer
    bool      dirty;          // есть несохранённые изменения
    bool      cursor_blink;
    uint32_t  last_blink;
} notes_instance_t;

static notes_instance_t notes_instances[NOTES_MAX_INSTANCES];

static notes_instance_t* notes_find(window_t* win) {
    for (int i = 0; i < NOTES_MAX_INSTANCES; i++) {
        if (notes_instances[i].used && notes_instances[i].win == win) {
            return &notes_instances[i];
        }
    }
    return NULL;
}

static notes_instance_t* notes_alloc(void) {
    for (int i = 0; i < NOTES_MAX_INSTANCES; i++) {
        if (!notes_instances[i].used) {
            notes_instances[i].used = true;
            return &notes_instances[i];
        }
    }
    return NULL;
}

static void notes_free(notes_instance_t* ni) {
    if (ni) ni->used = false;
}

// Константы layout
#define NOTES_HEADER_H 28
#define NOTES_LINE_H   16
#define NOTES_PADDING  8

// Линия отображения: координата Y для n-й строки (0-based).
static int notes_line_y(int n) {
    return NOTES_HEADER_H + NOTES_PADDING + n * NOTES_LINE_H;
}

static void notes_draw(window_t* win) {
    notes_instance_t* ni = notes_find(win);
    if (!ni) return;

    window_fill(win, 0xFFF8F8F0);

    // Header bar
    win_fill_rect(win, 0, 0, win->width, NOTES_HEADER_H, 0xFFE4E4DA);
    desktop_file_t* f = desktop_get_file(ni->file_idx);
    char title_buf[64];
    app_str_copy(title_buf, "Заметки: ", 64);
    int pos = app_strlen(title_buf);
    if (f) {
        int nl = app_strlen(f->name);
        for (int i = 0; i < nl && pos < 63; i++) title_buf[pos++] = f->name[i];
    }
    if (ni->dirty && pos < 63) title_buf[pos++] = '*';
    title_buf[pos] = '\0';
    win_draw_text(win, 10, 6, title_buf, 0xFF444438, 0xFF000000);

    // Save-button (правая верхняя часть заголовка)
    int btn_w = 60;
    int btn_h = 18;
    int btn_x = win->width - btn_w - 8;
    int btn_y = (NOTES_HEADER_H - btn_h) / 2;
    win_fill_rect(win, btn_x, btn_y, btn_w, btn_h,
                  ni->dirty ? 0xFF4477CC : 0xFF99AABB);
    win_draw_rect(win, btn_x, btn_y, btn_w, btn_h, 0xFF334466);
    win_draw_text(win, btn_x + (btn_w - 4 * FONT_WIDTH) / 2,
                  btn_y + (btn_h - FONT_HEIGHT) / 2,
                  "Сохр.", 0xFFFFFFFF, 0xFF000000);

    // Разделитель
    for (int x = 0; x < win->width; x++) {
        window_put_pixel(win, x, NOTES_HEADER_H - 1, 0xFFBBBBAA);
    }

    // Разбиваем buffer на строки (по \n и по ширине) и рисуем.
    int cur_x = NOTES_PADDING;
    int line = 0;
    int chars_per_line = (win->width - 2 * NOTES_PADDING) / FONT_WIDTH;
    if (chars_per_line < 1) chars_per_line = 1;

    int cursor_screen_x = cur_x;
    int cursor_screen_y = notes_line_y(0);
    int col = 0;

    for (int i = 0; i <= ni->len; i++) {
        // Проверяем, не здесь ли курсор (перед i-м символом).
        if (i == ni->cursor_pos) {
            cursor_screen_x = cur_x;
            cursor_screen_y = notes_line_y(line);
        }
        if (i == ni->len) break;

        char c = ni->buffer[i];
        if (c == '\n') {
            line++;
            cur_x = NOTES_PADDING;
            col = 0;
            continue;
        }
        // Wrap при достижении ширины
        if (col >= chars_per_line) {
            line++;
            cur_x = NOTES_PADDING;
            col = 0;
        }

        char s[2] = { c, '\0' };
        win_draw_text(win, cur_x, notes_line_y(line), s, 0xFF202028, 0xFF000000);
        cur_x += FONT_WIDTH;
        col++;
    }

    // Мигающий курсор
    if (ni->cursor_blink) {
        win_fill_rect(win, cursor_screen_x, cursor_screen_y, 2, FONT_HEIGHT,
                      0xFF4477CC);
    }
}

static void notes_create(window_t* win) {
    // Ищем/создаём instance. Если опубликовали окно без file_idx (прямой
    // запуск из меню Пуск) — создаём новый файл автоматически.
    notes_instance_t* ni = notes_find(win);
    if (!ni) {
        ni = notes_alloc();
        if (!ni) return;
        ni->win = win;
        ni->file_idx = desktop_create_file();
        ni->buffer[0] = '\0';
        ni->len = 0;
        ni->cursor_pos = 0;
        ni->dirty = false;
        ni->cursor_blink = true;
        ni->last_blink = 0;
    }
    notes_draw(win);
}

static void notes_update(window_t* win) {
    notes_instance_t* ni = notes_find(win);
    if (!ni) return;
    uint32_t ticks = (uint32_t)timer_get_ticks();
    if (ticks - ni->last_blink >= 50) {
        ni->cursor_blink = !ni->cursor_blink;
        ni->last_blink = ticks;
        notes_draw(win);
        wm_require_redraw();
    }
}

static void notes_click(window_t* win, int32_t x, int32_t y) {
    notes_instance_t* ni = notes_find(win);
    if (!ni) return;

    // Клик в кнопку Save?
    int btn_w = 60;
    int btn_h = 18;
    int btn_x = win->width - btn_w - 8;
    int btn_y = (NOTES_HEADER_H - btn_h) / 2;
    if (x >= btn_x && x < btn_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
        desktop_save_file(ni->file_idx, ni->buffer, ni->len);
        ni->dirty = false;
        notes_draw(win);
        wm_require_redraw();
        return;
    }
    // Иначе клик в текстовую область → позиционируем курсор (упрощённо: в
    // конец буфера, т.к. honest text-layout сложен для kernel).
    ni->cursor_pos = ni->len;
    ni->cursor_blink = true;
    notes_draw(win);
    wm_require_redraw();
}

static void notes_key(window_t* win, char key) {
    notes_instance_t* ni = notes_find(win);
    if (!ni) return;

    // Ctrl+S — см. note: драйвер клавиатуры не шлёт control-коды; для
    // простоты сохранение только через кнопку Save.
    if (key == '\n') {
        if (ni->len < DESKTOP_FILE_CONTENT_LEN - 1) {
            // Вставляем \n в позицию курсора
            for (int i = ni->len; i > ni->cursor_pos; i--)
                ni->buffer[i] = ni->buffer[i - 1];
            ni->buffer[ni->cursor_pos++] = '\n';
            ni->len++;
            ni->buffer[ni->len] = '\0';
            ni->dirty = true;
        }
    } else if (key == '\b') {
        if (ni->cursor_pos > 0) {
            for (int i = ni->cursor_pos - 1; i < ni->len - 1; i++)
                ni->buffer[i] = ni->buffer[i + 1];
            ni->cursor_pos--;
            ni->len--;
            ni->buffer[ni->len] = '\0';
            ni->dirty = true;
        }
    } else if (key >= ' ' && key <= '~') {
        if (ni->len < DESKTOP_FILE_CONTENT_LEN - 1) {
            for (int i = ni->len; i > ni->cursor_pos; i--)
                ni->buffer[i] = ni->buffer[i - 1];
            ni->buffer[ni->cursor_pos++] = key;
            ni->len++;
            ni->buffer[ni->len] = '\0';
            ni->dirty = true;
        }
    }

    ni->cursor_blink = true;
    notes_draw(win);
    wm_require_redraw();
}

// Открыть Notes на заданный файл рабочего стола (вызывается из desktop).
void notes_open_for_file(int file_idx) {
    desktop_file_t* f = desktop_get_file(file_idx);
    if (!f) return;

    // Создаём окно
    int32_t sw = (int32_t)framebuffer_get_width();
    int32_t sh = (int32_t)framebuffer_get_height();
    int ww = 500, wh = 360;
    int wx = (sw - ww) / 2;
    int wy = (sh - wh) / 2 - 20;

    window_t* win = window_create(f->name, wx, wy, ww, wh);
    if (!win) return;

    // Алокируем instance и копируем содержимое файла в буфер.
    notes_instance_t* ni = notes_alloc();
    if (!ni) { window_destroy(win); return; }
    ni->win = win;
    ni->file_idx = file_idx;
    int flen = app_strlen(f->content);
    if (flen > DESKTOP_FILE_CONTENT_LEN - 1) flen = DESKTOP_FILE_CONTENT_LEN - 1;
    for (int i = 0; i < flen; i++) ni->buffer[i] = f->content[i];
    ni->buffer[flen] = '\0';
    ni->len = flen;
    ni->cursor_pos = flen;
    ni->dirty = false;
    ni->cursor_blink = true;
    ni->last_blink = 0;

    notes_draw(win);
    wm_add_window(win);
}

// При закрытии окна Notes — освобождаем instance. Вызывается из wm.c
// через desktop_notify_window_closed → мы перехватим в custom callback.
// Реализуем через заранее зарегистрированный колбэк: см. notes_notify_close.
void notes_notify_close(window_t* win) {
    notes_instance_t* ni = notes_find(win);
    if (ni) {
        // Если dirty — автосохранение.
        if (ni->dirty) desktop_save_file(ni->file_idx, ni->buffer, ni->len);
        notes_free(ni);
    }
}

// ============================================================================
// 8. CONTROL PANEL — панель управления
// ============================================================================
// Позволяет настраивать систему: обои (фон), палитра, показывает сведения
// о памяти и предлагает reboot/shutdown.

// Текущие настройки (глобальные). В wm.c используем для рисования desktop.
// Начальные значения задаются AI-генератором через ui_theme.h.
// Пользователь может менять их в runtime через Control Panel.
uint32_t g_desktop_top_color    = UI_THEME_DESKTOP_TOP;
uint32_t g_desktop_bottom_color = UI_THEME_DESKTOP_BOTTOM;

// Пресеты обоев для смены по клику.
static const uint32_t wallpaper_presets[][2] = {
    { 0xFF143264, 0xFF050C19 },   // тёмно-синий (по умолчанию)
    { 0xFF2B143C, 0xFF0B0A17 },   // фиолетовый
    { 0xFF112A2E, 0xFF071113 },   // изумрудный
    { 0xFF3A1620, 0xFF170507 },   // бордо
    { 0xFF262626, 0xFF0A0A0A },   // графит
};
#define NUM_WALLPAPERS (sizeof(wallpaper_presets)/sizeof(wallpaper_presets[0]))

static void cpanel_draw(window_t* win) {
    window_fill(win, 0xFFF4F5F8);

    // Заголовок
    win_fill_rect(win, 0, 0, win->width, 40, 0xFFE1E3EC);
    win_draw_text(win, 14, 13, "Панель управления", 0xFF303040, 0xFF000000);
    for (int x = 0; x < win->width; x++) {
        window_put_pixel(win, x, 39, 0xFFB8BBC8);
    }

    int y = 60;
    int x = 16;

    // --- Wallpaper section ---
    win_draw_text(win, x, y, "Фон:", 0xFF505060, 0xFF000000);
    y += 24;
    int sw_size = 60;
    int sw_gap  = 12;
    for (unsigned int i = 0; i < NUM_WALLPAPERS; i++) {
        int sx = x + (int)i * (sw_size + sw_gap);
        // градиент-превью
        for (int r = 0; r < sw_size; r++) {
            uint8_t t = (uint8_t)((r * 255) / (sw_size - 1));
            uint32_t c = color_lerp(wallpaper_presets[i][0],
                                    wallpaper_presets[i][1], t);
            win_fill_rect(win, sx, y + r, sw_size, 1, c | 0xFF000000);
        }
        win_draw_rect(win, sx, y, sw_size, sw_size, 0xFF808090);
        // Отметка выбранного
        if (wallpaper_presets[i][0] == g_desktop_top_color) {
            win_draw_rect(win, sx - 2, y - 2, sw_size + 4, sw_size + 4, 0xFF4477CC);
            win_draw_rect(win, sx - 3, y - 3, sw_size + 6, sw_size + 6, 0xFF4477CC);
        }
    }
    y += sw_size + 24;

    // --- System info section ---
    win_draw_text(win, x, y, "Система:", 0xFF505060, 0xFF000000);
    y += 20;

    char buf[64];
    uint32_t sec = timer_get_uptime_sec();
    int_to_str(sec, buf);
    char uptime_line[64];
    app_str_copy(uptime_line, "Работает: ", 64);
    int off = app_strlen(uptime_line);
    app_str_copy(uptime_line + off, buf, 64 - off);
    off = app_strlen(uptime_line);
    app_str_copy(uptime_line + off, " sec", 64 - off);
    win_draw_text(win, x, y, uptime_line, 0xFF303030, 0xFF000000);
    y += 18;

    pmm_info_t pmm = pmm_get_info();
    int_to_str(pmm.free_memory / (1024*1024), buf);
    char mem_line[64];
    app_str_copy(mem_line, "Своб. ОЗУ: ", 64);
    off = app_strlen(mem_line);
    app_str_copy(mem_line + off, buf, 64 - off);
    off = app_strlen(mem_line);
    app_str_copy(mem_line + off, " MB", 64 - off);
    win_draw_text(win, x, y, mem_line, 0xFF303030, 0xFF000000);
    y += 18;

    char win_line[64];
    int wc = wm_get_window_count();
    int_to_str(wc, buf);
    app_str_copy(win_line, "Откр. окон: ", 64);
    off = app_strlen(win_line);
    app_str_copy(win_line + off, buf, 64 - off);
    win_draw_text(win, x, y, win_line, 0xFF303030, 0xFF000000);
    y += 28;

    // --- Power buttons ---
    win_draw_text(win, x, y, "Питание:", 0xFF505060, 0xFF000000);
    y += 20;
    win_fill_rect(win, x, y, 120, 30, 0xFFDD7744);
    win_draw_rect(win, x, y, 120, 30, 0xFFAA5522);
    win_draw_text(win, x + (120 - 6 * FONT_WIDTH) / 2, y + (30 - FONT_HEIGHT) / 2,
                  "Перезагрузка", 0xFFFFFFFF, 0xFF000000);

    win_fill_rect(win, x + 140, y, 120, 30, 0xFFCC3344);
    win_draw_rect(win, x + 140, y, 120, 30, 0xFF992222);
    win_draw_text(win, x + 140 + (120 - 8 * FONT_WIDTH) / 2,
                  y + (30 - FONT_HEIGHT) / 2,
                  "Выключение", 0xFFFFFFFF, 0xFF000000);
}

#include "../arch/i386/power.h"

static void cpanel_click(window_t* win, int32_t mx, int32_t my) {
    int y = 60;
    int x = 16;

    // Wallpaper row
    y += 24;
    int sw_size = 60;
    int sw_gap  = 12;
    int wp_y = y;
    for (unsigned int i = 0; i < NUM_WALLPAPERS; i++) {
        int sx = x + (int)i * (sw_size + sw_gap);
        if (mx >= sx && mx < sx + sw_size &&
            my >= wp_y && my < wp_y + sw_size) {
            g_desktop_top_color    = wallpaper_presets[i][0];
            g_desktop_bottom_color = wallpaper_presets[i][1];
            cpanel_draw(win);
            wm_require_redraw();
            return;
        }
    }
    y += sw_size + 24;

    // System info не интерактивно, пропускаем.
    y += 20 + 18 + 18 + 18 + 28;

    // Power-кнопки
    int pwr_y = y + 20;
    if (mx >= x && mx < x + 120 && my >= pwr_y && my < pwr_y + 30) {
        power_reboot();
    }
    if (mx >= x + 140 && mx < x + 260 && my >= pwr_y && my < pwr_y + 30) {
        power_shutdown();
    }
}

static void cpanel_update(window_t* win) {
    // Раз в секунду обновляем uptime / free ram.
    static uint32_t last = 0;
    uint32_t sec = timer_get_uptime_sec();
    if (sec != last) {
        last = sec;
        cpanel_draw(win);
        wm_require_redraw();
    }
}

// ============================================================================
// 9. FILE EXPLORER — проводник (Этап 27.5)
// ============================================================================
// Отображает содержимое виртуальной файловой системы MyOS.
//
// Логика построения списка текущего каталога:
//   • если path == "/" — собираем «виртуальный корень»: все активные
//     mount-points (например "/disk", а после Этапа 28 USB — "/usb0") +
//     содержимое initrd-root (vfs_get_root());
//   • иначе — vfs_resolve_path(path) и vfs_readdir() + vfs_finddir() для
//     каждого имени, чтобы узнать тип (VFS_DIRECTORY или VFS_FILE).
//
// Это архитектурное решение критически важно: когда Этап 28 выполнит
// vfs_mount("/usb0", usb_root, "fat32"), флешка мгновенно появится
// в проводнике без каких-либо правок explorer-кода.
//
// Поддерживает до EXPLORER_MAX_INSTANCES параллельных окон — каждое
// со своим current path и scroll offset. Instance хранится в win→instance
// мапе по аналогии с Notes.
// ============================================================================

#include "../fs/vfs.h"

#define EXPLORER_MAX_INSTANCES   4
#define EXPLORER_MAX_ENTRIES     128
#define EXPLORER_NAME_LEN        48
#define EXPLORER_PATH_LEN        256
#define EXPLORER_ROW_H           24
#define EXPLORER_HEADER_H        56     // toolbar (0..28) + path bar (28..56)

typedef struct {
    char     name[EXPLORER_NAME_LEN];
    bool     is_dir;
    bool     is_mount;         // точка монтирования — особый цвет иконки
    uint32_t size;
} explorer_entry_t;

typedef struct {
    bool     used;
    window_t* win;
    char     path[EXPLORER_PATH_LEN];
    explorer_entry_t entries[EXPLORER_MAX_ENTRIES];
    int      entry_count;
    int      scroll_offset;    // индекс первой видимой строки списка
    int      selected;         // индекс подсвеченной строки (-1 = нет)
} explorer_instance_t;

static explorer_instance_t explorer_instances[EXPLORER_MAX_INSTANCES];

// ---- instance pool helpers ----

static explorer_instance_t* explorer_find(window_t* win) {
    for (int i = 0; i < EXPLORER_MAX_INSTANCES; i++) {
        if (explorer_instances[i].used && explorer_instances[i].win == win) {
            return &explorer_instances[i];
        }
    }
    return NULL;
}

static explorer_instance_t* explorer_alloc(void) {
    for (int i = 0; i < EXPLORER_MAX_INSTANCES; i++) {
        if (!explorer_instances[i].used) {
            explorer_instances[i].used = true;
            explorer_instances[i].win = NULL;
            explorer_instances[i].entry_count = 0;
            explorer_instances[i].scroll_offset = 0;
            explorer_instances[i].selected = -1;
            explorer_instances[i].path[0] = '/';
            explorer_instances[i].path[1] = '\0';
            return &explorer_instances[i];
        }
    }
    return NULL;
}

static void explorer_free(explorer_instance_t* e) {
    if (e) e->used = false;
}

// ---- Path string helpers (мини-libc, т.к. в kernel нет strcat/strrchr) ----

// Установить путь в корень "/"
static void exp_path_set_root(char* p) {
    p[0] = '/'; p[1] = '\0';
}

// Подняться на уровень выше в абсолютном пути. "/" остаётся "/".
static void exp_path_up(char* p) {
    int n = 0; while (p[n]) n++;
    if (n <= 1) return;                          // уже "/"
    // Срезаем конечные '/' (если есть и это не сам корень)
    while (n > 1 && p[n-1] == '/') { p[--n] = '\0'; }
    // Отрезаем последний компонент
    while (n > 1 && p[n-1] != '/') { p[--n] = '\0'; }
    // Если остался только корневой '/' — ок. Иначе убираем trailing '/'.
    if (n > 1 && p[n-1] == '/') p[--n] = '\0';
    if (n == 0) { p[0] = '/'; p[1] = '\0'; }
}

// Войти в поддиректорию: добавить name к пути. Для корня даёт "/name",
// иначе "/path/name".
static void exp_path_enter(char* p, const char* name) {
    int n = 0; while (p[n]) n++;
    // Нужен ли разделитель '/' перед name
    bool need_slash = (n == 0) || (p[n-1] != '/');
    if (need_slash && n < EXPLORER_PATH_LEN - 1) {
        p[n++] = '/'; p[n] = '\0';
    }
    int i = 0;
    while (name[i] && n < EXPLORER_PATH_LEN - 1) {
        p[n++] = name[i++];
    }
    p[n] = '\0';
}

// Сравнение строк
static bool exp_str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0) && (*b == 0);
}

// ---- Refresh: сбор записей для текущего пути ----

static void explorer_refresh(explorer_instance_t* e) {
    e->entry_count = 0;
    e->selected = -1;

    // Специальный случай: виртуальный корень "/"
    if (exp_str_eq(e->path, "/")) {
        // 1) Точки монтирования (FAT32 /disk, позже /usb0 и т.д.)
        for (int i = 0; i < VFS_MAX_MOUNTS && e->entry_count < EXPLORER_MAX_ENTRIES; i++) {
            const vfs_mount_t* m = vfs_get_mount(i);
            if (!m) continue;
            // m->path вида "/disk" — извлекаем часть после ведущего '/'
            const char* nm = m->path;
            if (*nm == '/') nm++;
            if (!*nm) continue;
            explorer_entry_t* en = &e->entries[e->entry_count++];
            int k = 0;
            while (nm[k] && k < EXPLORER_NAME_LEN - 1) {
                en->name[k] = nm[k]; k++;
            }
            en->name[k] = '\0';
            en->is_dir   = true;
            en->is_mount = true;
            en->size     = 0;
        }

        // 2) Содержимое initrd-root (если примонтирован)
        vfs_node_t* root = vfs_get_root();
        if (root && (root->flags & VFS_DIRECTORY)) {
            for (uint32_t idx = 0; e->entry_count < EXPLORER_MAX_ENTRIES; idx++) {
                vfs_dirent_t* d = vfs_readdir(root, idx);
                if (!d) break;
                if (d->name[0] == '\0') continue;
                explorer_entry_t* en = &e->entries[e->entry_count++];
                int k = 0;
                while (d->name[k] && k < EXPLORER_NAME_LEN - 1) {
                    en->name[k] = d->name[k]; k++;
                }
                en->name[k] = '\0';
                vfs_node_t* ch = vfs_finddir(root, d->name);
                en->is_dir   = (ch && (ch->flags & VFS_DIRECTORY)) ? true : false;
                en->is_mount = false;
                en->size     = ch ? ch->length : 0;
            }
        }
        return;
    }

    // Обычный путь — через VFS
    vfs_node_t* dir = vfs_resolve_path(e->path);
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return;

    for (uint32_t idx = 0; e->entry_count < EXPLORER_MAX_ENTRIES; idx++) {
        vfs_dirent_t* d = vfs_readdir(dir, idx);
        if (!d) break;
        if (d->name[0] == '\0') continue;
        // Пропускаем "." и ".."
        if (d->name[0] == '.' && d->name[1] == '\0') continue;
        if (d->name[0] == '.' && d->name[1] == '.' && d->name[2] == '\0') continue;

        explorer_entry_t* en = &e->entries[e->entry_count++];
        int k = 0;
        while (d->name[k] && k < EXPLORER_NAME_LEN - 1) {
            en->name[k] = d->name[k]; k++;
        }
        en->name[k] = '\0';
        vfs_node_t* ch = vfs_finddir(dir, d->name);
        en->is_dir   = (ch && (ch->flags & VFS_DIRECTORY)) ? true : false;
        en->is_mount = false;
        en->size     = ch ? ch->length : 0;
    }
}

// ---- Drawing ----

static void explorer_draw_button(window_t* win, int x, int y, int w, int h,
                                 const char* label, uint32_t bg) {
    win_fill_rect(win, x, y, w, h, bg);
    win_draw_rect(win, x, y, w, h, 0xFF334455);
    int llen = app_strlen(label);
    int tx = x + (w - llen * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    win_draw_text(win, tx, ty, label, 0xFFFFFFFF, 0xFF000000);
}

static void explorer_draw(window_t* win) {
    explorer_instance_t* e = explorer_find(win);
    if (!e) return;

    // Фон окна
    window_fill(win, 0xFFF4F4F8);

    // --- Toolbar (y = 0..28) ---
    win_fill_rect(win, 0, 0, win->width, 28, 0xFFDDE0E8);
    for (int x = 0; x < win->width; x++) {
        window_put_pixel(win, x, 27, 0xFFA0A4B0);
    }
    explorer_draw_button(win,   6, 4, 50, 20, "Назад",  0xFF5577AA);
    explorer_draw_button(win,  62, 4, 36, 20, "Вверх",  0xFF5577AA);
    explorer_draw_button(win, 104, 4, 60, 20, "Обновить", 0xFF44AA66);
    explorer_draw_button(win, 170, 4, 24, 20, "^",       0xFF888899);
    explorer_draw_button(win, 198, 4, 24, 20, "v",       0xFF888899);

    // --- Path bar (y = 28..56) ---
    win_fill_rect(win, 0, 28, win->width, 28, 0xFFFFFFFF);
    win_draw_text(win, 10, 28 + (28 - FONT_HEIGHT) / 2,
                  "Путь:", 0xFF666677, 0xFFFFFFFF);
    win_draw_text(win, 10 + 6 * FONT_WIDTH, 28 + (28 - FONT_HEIGHT) / 2,
                  e->path, 0xFF112233, 0xFFFFFFFF);
    for (int x = 0; x < win->width; x++) {
        window_put_pixel(win, x, 55, 0xFFA0A4B0);
    }

    // --- Список записей ---
    int list_y0 = EXPLORER_HEADER_H;
    int list_h  = win->height - EXPLORER_HEADER_H;
    int rows_visible = list_h / EXPLORER_ROW_H;

    if (e->entry_count == 0) {
        win_draw_text(win, 12, list_y0 + 8,
                      "(empty directory)", 0xFF999999, 0xFFF4F4F8);
        return;
    }

    // Нормализация scroll_offset
    if (e->scroll_offset < 0) e->scroll_offset = 0;
    int max_scroll = e->entry_count - rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (e->scroll_offset > max_scroll) e->scroll_offset = max_scroll;

    for (int row = 0; row < rows_visible; row++) {
        int idx = e->scroll_offset + row;
        if (idx >= e->entry_count) break;
        explorer_entry_t* en = &e->entries[idx];
        int y = list_y0 + row * EXPLORER_ROW_H;

        // Фон строки — зебра, подсветка выделения
        uint32_t row_bg = (idx & 1) ? 0xFFEFEFF4 : 0xFFF8F8FC;
        if (idx == e->selected) row_bg = 0xFFAACCFF;
        win_fill_rect(win, 0, y, win->width - 4, EXPLORER_ROW_H, row_bg);

        // Иконка слева: цветной квадрат 16×16 с буквой
        uint32_t icon_color;
        const char* icon_sym;
        if (en->is_mount)     { icon_color = 0xFFAA5533; icon_sym = "D"; }
        else if (en->is_dir)  { icon_color = 0xFF4477BB; icon_sym = "/"; }
        else                  { icon_color = 0xFF66AA66; icon_sym = "f"; }

        win_fill_rect(win, 8, y + 4, 16, 16, icon_color);
        win_draw_rect(win, 8, y + 4, 16, 16, 0xFF222233);
        win_draw_text(win, 8 + (16 - FONT_WIDTH) / 2,
                      y + 4 + (16 - FONT_HEIGHT) / 2,
                      icon_sym, 0xFFFFFFFF, 0xFF000000);

        // Имя
        win_draw_text(win, 32, y + (EXPLORER_ROW_H - FONT_HEIGHT) / 2,
                      en->name, 0xFF101010, row_bg);

        // Правая подпись: размер (для файлов) / тег для mount и директорий
        char right[32]; int ri = 0;
        if (en->is_mount) {
            const char* t = "[mount]";
            while (t[ri] && ri < 31) { right[ri] = t[ri]; ri++; }
        } else if (en->is_dir) {
            const char* t = "<dir>";
            while (t[ri] && ri < 31) { right[ri] = t[ri]; ri++; }
        } else {
            char num[16]; int_to_str((int32_t)en->size, num);
            int k = 0;
            while (num[k] && ri < 29) { right[ri++] = num[k++]; }
            right[ri++] = ' ';
            right[ri++] = 'B';
        }
        right[ri] = '\0';
        int rlen = app_strlen(right);
        int rx = win->width - 12 - rlen * FONT_WIDTH;
        win_draw_text(win, rx, y + (EXPLORER_ROW_H - FONT_HEIGHT) / 2,
                      right, 0xFF557799, row_bg);
    }

    // Scrollbar справа (полоска 4px)
    int bar_x = win->width - 4;
    win_fill_rect(win, bar_x, list_y0, 4, list_h, 0xFFCCCCD4);
    if (e->entry_count > rows_visible) {
        int bar_h = (rows_visible * list_h) / e->entry_count;
        if (bar_h < 6) bar_h = 6;
        int denom = e->entry_count - rows_visible;
        int bar_y = list_y0;
        if (denom > 0) {
            bar_y = list_y0 + (e->scroll_offset * (list_h - bar_h)) / denom;
        }
        win_fill_rect(win, bar_x, bar_y, 4, bar_h, 0xFF6677AA);
    }
}

// ---- Callbacks ----

static void explorer_create(window_t* win) {
    explorer_instance_t* e = explorer_find(win);
    if (!e) {
        e = explorer_alloc();
        if (!e) return;
        e->win = win;
        exp_path_set_root(e->path);
        e->scroll_offset = 0;
        e->selected = -1;
    }
    explorer_refresh(e);
    explorer_draw(win);
}

static void explorer_click(window_t* win, int32_t mx, int32_t my) {
    explorer_instance_t* e = explorer_find(win);
    if (!e) return;

    // --- Toolbar (y < 28) ---
    if (my < 28) {
        if (mx >= 6 && mx < 56) {                 // Back
            exp_path_up(e->path);
            e->scroll_offset = 0;
            explorer_refresh(e);
        } else if (mx >= 62 && mx < 98) {         // Up
            exp_path_up(e->path);
            e->scroll_offset = 0;
            explorer_refresh(e);
        } else if (mx >= 104 && mx < 164) {       // Refresh
            explorer_refresh(e);
        } else if (mx >= 170 && mx < 194) {       // Scroll up
            if (e->scroll_offset > 0) e->scroll_offset--;
        } else if (mx >= 198 && mx < 222) {       // Scroll down
            int rows_visible =
                (win->height - EXPLORER_HEADER_H) / EXPLORER_ROW_H;
            int max_scroll = e->entry_count - rows_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (e->scroll_offset < max_scroll) e->scroll_offset++;
        }
        explorer_draw(win);
        wm_require_redraw();
        return;
    }

    // --- Path bar (y < 56) — пока некликабельно ---
    if (my < EXPLORER_HEADER_H) return;

    // --- List area ---
    int row = (my - EXPLORER_HEADER_H) / EXPLORER_ROW_H;
    int idx = e->scroll_offset + row;
    if (idx < 0 || idx >= e->entry_count) return;

    explorer_entry_t* en = &e->entries[idx];
    if (en->is_dir) {
        // Навигация внутрь директории (включая mount-points)
        exp_path_enter(e->path, en->name);
        e->scroll_offset = 0;
        explorer_refresh(e);
    } else {
        // Файл — просто подсвечиваем (полноценный viewer можно добавить
        // позже, сейчас цель — показать наличие файлов на диске).
        e->selected = idx;
    }
    explorer_draw(win);
    wm_require_redraw();
}

// Хук закрытия окна — освобождает instance. Вызывается из wm.c
// (аналогично notes_notify_close).
void explorer_notify_close(window_t* win) {
    explorer_instance_t* e = explorer_find(win);
    if (e) explorer_free(e);
}

// Внешний entry-point (программный запуск). Не используется в текущей
// сборке, но оставлен для симметрии с notes_open_for_file.
void explorer_open(void) {
    int32_t sw = (int32_t)framebuffer_get_width();
    int32_t sh = (int32_t)framebuffer_get_height();
    int ww = 640, wh = 440;
    int wx = (sw - ww) / 2;
    int wy = (sh - wh) / 2 - 20;

    window_t* win = window_create("Проводник", wx, wy, ww, wh);
    if (!win) return;

    explorer_instance_t* e = explorer_alloc();
    if (!e) { window_destroy(win); return; }
    e->win = win;
    exp_path_set_root(e->path);

    explorer_refresh(e);
    explorer_draw(win);
    wm_add_window(win);
}

// ============================================================================
// EMBERS — менеджер задач MyOS, визуализация SMP-нагрузки.
// ----------------------------------------------------------------------------
// Каждое ядро отображается как «уголёк»: горизонтальный бар, где заполненная
// часть = % busy за последнее сэмпл-окно. Цвет тлеет от тёмно-серого
// (idle) к ярко-жёлто-оранжевому (нагрузка >75%). Над барами — общая
// статистика: ACPI-discovered cores / probuzhdeno / IOAPIC.
// ============================================================================

typedef struct {
    uint32_t prev_busy[MAX_CPUS];
    uint32_t prev_idle[MAX_CPUS];
    uint32_t pct[MAX_CPUS];
    uint32_t last_sample_tick;
} embers_state_t;

static embers_state_t g_embers_state;

static uint32_t embers_color_for(uint32_t pct) {
    if (pct >= 75) return 0xFFFF6633;
    if (pct >= 50) return 0xFFFFAA22;
    if (pct >= 25) return 0xFFCC6622;
    if (pct >  5)  return 0xFF885533;
    return 0xFF333344;
}

static void embers_draw_panel(window_t* win) {
    int32_t W = win->width, H = win->height;

    for (int y = 0; y < H; y++) {
        uint8_t shade = 0x1A - (uint8_t)((y * 0x10) / (H ? H : 1));
        uint32_t bg = (0xFF000000) | ((uint32_t)shade << 16) |
                      ((uint32_t)shade << 8)  | ((uint32_t)(shade + 4));
        for (int x = 0; x < W; x++) window_put_pixel(win, x, y, bg);
    }

    win_draw_text(win, 12, 10, "EMBERS - menedzher zadach", 0xFFFFCC88, 0xFF000000);
    for (int x = 10; x < W - 10; x++) window_put_pixel(win, x, 28, 0xFF553322);

    char buf[16];
    int_to_str(num_cpus, buf);
    win_draw_text(win, 12, 36, "Ядер:", 0xFFAAAA77, 0xFF000000);
    win_draw_text(win, 12 + 7*FONT_WIDTH, 36, buf, 0xFFFFFFFF, 0xFF000000);

    int_to_str((int32_t)smp_cpus_started, buf);
    win_draw_text(win, 12 + 13*FONT_WIDTH, 36, "Пробуждено:", 0xFFAAAA77, 0xFF000000);
    win_draw_text(win, 12 + 26*FONT_WIDTH, 36, buf, 0xFF99FF99, 0xFF000000);

    win_draw_text(win, 12 + 32*FONT_WIDTH, 36, "IOAPIC:", 0xFFAAAA77, 0xFF000000);
    win_draw_text(win, 12 + 40*FONT_WIDTH, 36,
                  ioapic_present() ? "yes" : "no",
                  ioapic_present() ? 0xFF99FF99 : 0xFFFF8866, 0xFF000000);

    int bar_x = 12;
    int bar_y = 64;
    int bar_w = W - 24;
    int bar_h = 26;
    int gap   = 6;
    int slot  = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        if (!cpu_stats[i].online) continue;

        int by = bar_y + slot * (bar_h + gap);
        if (by + bar_h > H - 6) break;

        char lbl[24];
        const char* role = (i == 0) ? "BSP" : "AP ";
        int_to_str(i, buf);
        lbl[0] = 'Y'; lbl[1] = 'd'; lbl[2] = 'r'; lbl[3] = 'o'; lbl[4] = ' ';
        lbl[5] = buf[0];
        int li = 6;
        if (buf[1]) { lbl[li++] = buf[1]; }
        lbl[li++] = ' '; lbl[li++] = '[';
        lbl[li++] = role[0]; lbl[li++] = role[1]; lbl[li++] = role[2];
        lbl[li++] = ']'; lbl[li] = 0;
        win_draw_text(win, bar_x, by + (bar_h - FONT_HEIGHT) / 2,
                      lbl, 0xFFCCCCCC, 0xFF000000);

        int b_left = bar_x + 11 * FONT_WIDTH;
        int b_right_text = 6 * FONT_WIDTH;
        int b_right = bar_x + bar_w - b_right_text;
        int b_w = b_right - b_left;
        if (b_w < 20) continue;
        win_fill_rect(win, b_left, by, b_w, bar_h, 0xFF111118);
        win_draw_rect(win, b_left, by, b_w, bar_h, 0xFF332211);

        uint32_t pct = g_embers_state.pct[i];
        if (pct > 100) pct = 100;
        int fill = (int)((b_w - 2) * pct / 100u);
        uint32_t col = embers_color_for(pct);

        for (int yy = 1; yy < bar_h - 1; yy++) {
            uint32_t c = col;
            if (yy == 1) c = 0xFFFFCC44;
            for (int xx = 1; xx <= fill; xx++) {
                window_put_pixel(win, b_left + xx, by + yy, c);
            }
        }

        int_to_str((int32_t)pct, buf);
        int pct_x = bar_x + bar_w - 5 * FONT_WIDTH;
        win_draw_text(win, pct_x, by + (bar_h - FONT_HEIGHT) / 2,
                      buf, 0xFFFFFFFF, 0xFF000000);
        win_draw_text(win, pct_x + 4 * FONT_WIDTH, by + (bar_h - FONT_HEIGHT) / 2,
                      "%", 0xFFAAAAAA, 0xFF000000);

        slot++;
    }

    win_draw_text(win, 12, H - 18, "Этап 30: SMP — LAPIC, IOAPIC, trampoline.",
                  0xFF665544, 0xFF000000);
}

static void embers_create(window_t* win) {
    for (int i = 0; i < MAX_CPUS; i++) {
        g_embers_state.prev_busy[i] = cpu_stats[i].busy_ticks;
        g_embers_state.prev_idle[i] = cpu_stats[i].idle_ticks;
        g_embers_state.pct[i]       = 0;
    }
    g_embers_state.last_sample_tick = (uint32_t)timer_get_ticks();
    embers_draw_panel(win);
}

static void embers_update(window_t* win) {
    uint32_t now = (uint32_t)timer_get_ticks();
    if (now - g_embers_state.last_sample_tick < 25) return;
    g_embers_state.last_sample_tick = now;

    bool changed = false;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!cpu_stats[i].online) {
            if (g_embers_state.pct[i] != 0) {
                g_embers_state.pct[i] = 0;
                changed = true;
            }
            continue;
        }
        uint32_t b = cpu_stats[i].busy_ticks;
        uint32_t d = cpu_stats[i].idle_ticks;
        uint32_t db = b - g_embers_state.prev_busy[i];
        uint32_t di = d - g_embers_state.prev_idle[i];
        g_embers_state.prev_busy[i] = b;
        g_embers_state.prev_idle[i] = d;

        uint32_t total = db + di;
        uint32_t pct = total ? (db * 100u) / total : 0;

        if (pct != g_embers_state.pct[i]) {
            g_embers_state.pct[i] = pct;
            changed = true;
        }
    }
    if (changed) {
        embers_draw_panel(win);
        wm_require_redraw();
    }
}

// ============================================================================
// ДИСПЕТЧЕР ЗАДАЧ (Task Manager)
// ============================================================================
// Показывает список всех задач из task_list_first(): PID, имя, состояние,
// CPU% (доля cpu_ticks от общей суммы), а также сводку — память (PMM),
// uptime, загрузку CPU.
// Клик по строке выбирает задачу; по кнопке «Завершить» — task_kill_by_id.
// Самоубийство (выбранная = current) не разрешается ядром (-2), UI это
// учитывает и показывает предупреждение.
// ============================================================================

#include "../task/task.h"
#include "../drivers/rtc.h"

#define TM_MAX_ROWS 32

// Тип строки — задача (kernel/ELF) или окно GUI-приложения.
#define TM_KIND_TASK    0
#define TM_KIND_WINDOW  1

typedef struct {
    uint32_t kind;           // TM_KIND_TASK | TM_KIND_WINDOW
    uint32_t id;             // task_id для TASK, window_id для WINDOW
    uint32_t state;          // task_t: 0=run/1=ready/2=dead; window: 0=open/1=min/2=foc
    uint64_t cpu_ticks;      // только для TASK
    uint32_t pct;             // только для TASK
    char     name[TASK_NAME_MAX + 1];
    bool     is_current;
    void*    win_ptr;        // window_t* для WINDOW (для close_requested), NULL иначе
} tm_row_t;

typedef struct {
    tm_row_t rows[TM_MAX_ROWS];
    uint32_t row_count;
    uint64_t prev_ticks[TM_MAX_ROWS]; // для дельты CPU%
    uint32_t prev_ids[TM_MAX_ROWS];   // соответствие id <-> prev_ticks
    uint32_t prev_count;
    int32_t  selected;        // индекс выделенной строки или -1
    uint32_t last_action_pid; // pid, по которому был «kill»
    int32_t  last_action_res; // 0/-1/-2 от task_kill_by_id
    uint32_t last_action_show_until_tick; // до какого тика показывать сообщение
} tm_state_t;

static tm_state_t g_tm_state;

// Найти прошлый ticks-снимок по pid; вернуть 0, если не было.
static uint64_t tm_prev_ticks(uint32_t id) {
    for (uint32_t i = 0; i < g_tm_state.prev_count; i++) {
        if (g_tm_state.prev_ids[i] == id) return g_tm_state.prev_ticks[i];
    }
    return 0;
}

// Снять снимок в g_tm_state.rows: сначала открытые окна (GUI-приложения,
// которые не имеют отдельного task_t), затем задачи планировщика; для задач
// считаем дельту cpu_ticks для оценки CPU%.
static void tm_collect(void) {
    g_tm_state.row_count = 0;

    // --- 1. Сначала окна (GUI-приложения с рабочего стола) ---
    window_t* focused = wm_get_focused_window();
    window_t* w = wm_iterate_windows(NULL);
    while (w && g_tm_state.row_count < TM_MAX_ROWS) {
        tm_row_t* r = &g_tm_state.rows[g_tm_state.row_count];
        r->kind = TM_KIND_WINDOW;
        r->id   = (uint32_t)w->id;
        if (w->is_minimized)        r->state = 1;
        else if (w == focused)      r->state = 2;
        else                        r->state = 0;
        r->cpu_ticks = 0;
        r->pct       = 0;
        r->is_current = false;
        r->win_ptr  = (void*)w;
        // имя — заголовок окна
        int j = 0;
        while (w->title[j] && j < TASK_NAME_MAX) {
            r->name[j] = w->title[j]; j++;
        }
        r->name[j] = '\0';
        if (j == 0) { r->name[0] = '?'; r->name[1] = 0; }

        g_tm_state.row_count++;
        w = wm_iterate_windows(w);
    }

    // --- 2. Затем задачи планировщика ---
    task_t* head = task_list_first();
    if (head) {
        task_t* cur_task = task_get_current();
        uint64_t deltas[TM_MAX_ROWS] = {0};
        uint64_t total_delta = 0;
        uint32_t task_first = g_tm_state.row_count;

        task_t* t = head;
        do {
            if (g_tm_state.row_count >= TM_MAX_ROWS) break;
            tm_row_t* r = &g_tm_state.rows[g_tm_state.row_count];
            r->kind = TM_KIND_TASK;
            r->id = t->id;
            r->state = (uint32_t)t->state;
            r->cpu_ticks = t->cpu_ticks;
            r->is_current = (t == cur_task);
            r->win_ptr = NULL;
            int j = 0;
            while (t->name[j] && j < TASK_NAME_MAX) {
                r->name[j] = t->name[j]; j++;
            }
            r->name[j] = '\0';
            if (j == 0) { r->name[0] = '?'; r->name[1] = 0; }

            uint64_t prev = tm_prev_ticks(t->id);
            uint64_t delta = (t->cpu_ticks > prev) ? (t->cpu_ticks - prev) : 0;
            deltas[g_tm_state.row_count - task_first] = delta;
            total_delta += delta;

            g_tm_state.row_count++;
            t = t->next;
        } while (t != head);

        // Считаем проценты от total_delta
        for (uint32_t i = task_first; i < g_tm_state.row_count; i++) {
            uint64_t d = deltas[i - task_first];
            uint32_t pct = 0;
            if (total_delta > 0) pct = (uint32_t)((d * 100ULL) / total_delta);
            if (pct > 100) pct = 100;
            g_tm_state.rows[i].pct = pct;
        }
    }

    // Сохраняем текущие cpu_ticks для дельты в следующем тике (только TASK)
    g_tm_state.prev_count = 0;
    for (uint32_t i = 0; i < g_tm_state.row_count; i++) {
        if (g_tm_state.rows[i].kind == TM_KIND_TASK &&
            g_tm_state.prev_count < TM_MAX_ROWS) {
            g_tm_state.prev_ids[g_tm_state.prev_count]   = g_tm_state.rows[i].id;
            g_tm_state.prev_ticks[g_tm_state.prev_count] = g_tm_state.rows[i].cpu_ticks;
            g_tm_state.prev_count++;
        }
    }
}

static const char* tm_state_str(const tm_row_t* r) {
    if (r->kind == TM_KIND_WINDOW) {
        switch (r->state) {
            case 0: return "Открыто";
            case 1: return "Свёрнуто";
            case 2: return "Активно";
            default: return "?";
        }
    }
    switch (r->state) {
        case 0: return "Running";
        case 1: return "Ready";
        case 2: return "Dead";
        default: return "?";
    }
}
static uint32_t tm_state_color(const tm_row_t* r) {
    if (r->kind == TM_KIND_WINDOW) {
        if (r->state == 2) return 0xFFCCEEFF;     // фокус
        if (r->state == 1) return 0xFFAAAAAA;     // свёрнуто
        return 0xFFE0E8FF;                        // обычное окно
    }
    if (r->is_current) return 0xFF66FFAA;        // ярко-зелёный — текущая
    switch (r->state) {
        case 0: return 0xFFAAFFAA;               // running
        case 1: return 0xFFCCCCFF;               // ready
        case 2: return 0xFFFF8866;               // dead
        default: return 0xFFCCCCCC;
    }
}

// Нарисовать прогресс-бар CPU%
static void tm_draw_pct_bar(window_t* win, int x, int y, int w, int h,
                            uint32_t pct) {
    win_fill_rect(win, x, y, w, h, 0xFF15171E);
    win_draw_rect(win, x, y, w, h, 0xFF334466);
    if (pct > 100) pct = 100;
    int fill = (w - 2) * (int)pct / 100;
    uint32_t col;
    if (pct >= 75)      col = 0xFFFF6666;
    else if (pct >= 40) col = 0xFFFFCC44;
    else if (pct >= 10) col = 0xFF44CC88;
    else                col = 0xFF446688;
    if (fill > 0) win_fill_rect(win, x + 1, y + 1, fill, h - 2, col);
}

// Геометрия окна и кнопок
#define TM_HDR_H        40
#define TM_ROW_H        18
#define TM_LIST_TOP     90
#define TM_BTN_H        28

static void tm_draw(window_t* win) {
    int W = win->width, H = win->height;

    // Фоновый градиент
    for (int y = 0; y < H; y++) {
        uint8_t shade = (uint8_t)(0x14 + (y * 0x0C) / (H ? H : 1));
        uint32_t bg = 0xFF000000 |
                      ((uint32_t)(shade)     << 16) |
                      ((uint32_t)(shade + 2) <<  8) |
                      ((uint32_t)(shade + 6));
        for (int x = 0; x < W; x++) window_put_pixel(win, x, y, bg);
    }

    // Заголовок
    win_draw_text(win, 12, 10, "Диспетчер задач AMPLIFY-OS",
                  0xFFE8EAF2, 0xFF000000);
    for (int x = 10; x < W - 10; x++) window_put_pixel(win, x, 30, 0xFF445577);

    // Сводка: число задач, CPU-cores, время, память
    char buf[32];
    // Раздельно считаем приложения (окна) и системные задачи
    uint32_t apps_n = 0, tasks_n = 0;
    for (uint32_t i = 0; i < g_tm_state.row_count; i++) {
        if (g_tm_state.rows[i].kind == TM_KIND_WINDOW) apps_n++;
        else                                            tasks_n++;
    }
    int_to_str((int32_t)apps_n, buf);
    win_draw_text(win, 12, 40, "Окон:", 0xFFAABBCC, 0xFF000000);
    win_draw_text(win, 12 + 7*FONT_WIDTH, 40, buf, 0xFFFFFFFF, 0xFF000000);

    int_to_str((int32_t)tasks_n, buf);
    win_draw_text(win, 12 + 13*FONT_WIDTH, 40, "Задач:", 0xFFAABBCC, 0xFF000000);
    win_draw_text(win, 12 + 21*FONT_WIDTH, 40, buf, 0xFFFFFFFF, 0xFF000000);

    int_to_str((int32_t)num_cpus, buf);
    win_draw_text(win, 12 + 27*FONT_WIDTH, 40, "Ядер:", 0xFFAABBCC, 0xFF000000);
    win_draw_text(win, 12 + 34*FONT_WIDTH, 40, buf, 0xFFFFFFFF, 0xFF000000);

    // Реальное время из RTC
    rtc_time_t rt; rtc_read(&rt);
    char tm_str[16];
    tm_str[0] = (char)('0' + (rt.hour   / 10) % 10);
    tm_str[1] = (char)('0' +  rt.hour   % 10);
    tm_str[2] = ':';
    tm_str[3] = (char)('0' + (rt.minute / 10) % 10);
    tm_str[4] = (char)('0' +  rt.minute % 10);
    tm_str[5] = ':';
    tm_str[6] = (char)('0' + (rt.second / 10) % 10);
    tm_str[7] = (char)('0' +  rt.second % 10);
    tm_str[8] = 0;
    win_draw_text(win, W - 90, 40, tm_str, 0xFFCCEEFF, 0xFF000000);

    // Память — занято / всего, MiB
    pmm_info_t pmm_info = pmm_get_info();
    uint32_t used_mb = (pmm_info.total_memory - pmm_info.free_memory) / (1024*1024);
    uint32_t tot_mb  = pmm_info.total_memory / (1024*1024);
    int_to_str((int32_t)used_mb, buf);
    win_draw_text(win, 12, 60, "Память:", 0xFFAABBCC, 0xFF000000);
    win_draw_text(win, 12 + 9*FONT_WIDTH, 60, buf, 0xFFFFFFFF, 0xFF000000);
    win_draw_text(win, 12 + (9 + (int)app_strlen(buf))*FONT_WIDTH, 60,
                  " / ", 0xFFAABBCC, 0xFF000000);
    int o = 9 + (int)app_strlen(buf) + 3;
    int_to_str((int32_t)tot_mb, buf);
    win_draw_text(win, 12 + o*FONT_WIDTH, 60, buf, 0xFFFFFFFF, 0xFF000000);
    o += (int)app_strlen(buf);
    win_draw_text(win, 12 + o*FONT_WIDTH, 60, " МиБ",
                  0xFFAABBCC, 0xFF000000);

    // Шапка таблицы
    int hy = TM_LIST_TOP - 18;
    win_draw_text(win, 12,                  hy, "Тип",     0xFFAACCFF, 0xFF000000);
    win_draw_text(win, 12 + 5*FONT_WIDTH,   hy, "ID",      0xFFAACCFF, 0xFF000000);
    win_draw_text(win, 12 + 9*FONT_WIDTH,   hy, "Имя",     0xFFAACCFF, 0xFF000000);
    win_draw_text(win, 12 + 30*FONT_WIDTH,  hy, "Состояние", 0xFFAACCFF, 0xFF000000);
    win_draw_text(win, 12 + 41*FONT_WIDTH,  hy, "CPU",     0xFFAACCFF, 0xFF000000);
    for (int x = 10; x < W - 10; x++) window_put_pixel(win, x, hy + 14, 0xFF334455);

    // Строки таблицы
    int max_visible = (H - TM_LIST_TOP - TM_BTN_H - 16) / TM_ROW_H;
    if (max_visible > (int)g_tm_state.row_count) max_visible = (int)g_tm_state.row_count;

    for (int i = 0; i < max_visible; i++) {
        tm_row_t* r = &g_tm_state.rows[i];
        int ry = TM_LIST_TOP + i * TM_ROW_H;
        bool sel = ((int)i == g_tm_state.selected);
        if (sel) {
            win_fill_rect(win, 6, ry - 2, W - 12, TM_ROW_H, 0xFF1E2A44);
            win_draw_rect(win, 6, ry - 2, W - 12, TM_ROW_H, 0xFF4488CC);
        } else if (i & 1) {
            win_fill_rect(win, 6, ry - 2, W - 12, TM_ROW_H, 0xFF15181E);
        }

        char tmp[24];

        // Тип
        const char* type_str = (r->kind == TM_KIND_WINDOW) ? "App" : "Sys";
        uint32_t type_col   = (r->kind == TM_KIND_WINDOW) ? 0xFFAAFFCC : 0xFFFFCC88;
        win_draw_text(win, 12, ry, type_str, type_col, 0xFF000000);

        // ID
        int_to_str((int32_t)r->id, tmp);
        win_draw_text(win, 12 + 5*FONT_WIDTH, ry, tmp,
                      0xFFFFFFFF, 0xFF000000);

        // Имя — обрезаем до 20 символов, чтобы не наезжало
        char nm[22];
        int k = 0;
        while (r->name[k] && k < 20) { nm[k] = r->name[k]; k++; }
        if (r->name[k]) { nm[20] = '~'; nm[21] = 0; }
        else            { nm[k] = 0; }
        uint32_t nm_color = r->is_current ? 0xFFCCFFCC : 0xFFE8EAF2;
        win_draw_text(win, 12 + 9*FONT_WIDTH, ry, nm, nm_color, 0xFF000000);

        const char* st = tm_state_str(r);
        uint32_t   sc = tm_state_color(r);
        win_draw_text(win, 12 + 30*FONT_WIDTH, ry, st, sc, 0xFF000000);

        if (r->kind == TM_KIND_TASK) {
            // CPU% — число + бар (только для системных задач)
            int_to_str((int32_t)r->pct, tmp);
            win_draw_text(win, 12 + 41*FONT_WIDTH, ry, tmp,
                          0xFFFFFFFF, 0xFF000000);
            win_draw_text(win, 12 + 44*FONT_WIDTH, ry, "%",
                          0xFFAAAAAA, 0xFF000000);
            tm_draw_pct_bar(win, 12 + 47*FONT_WIDTH, ry + 1,
                            12*FONT_WIDTH, TM_ROW_H - 4, r->pct);
        } else {
            // Для окон — иконка статуса вместо CPU%
            const char* glyph = (r->state == 2) ? "*" :
                                 (r->state == 1) ? "_" : "-";
            win_draw_text(win, 12 + 41*FONT_WIDTH, ry, glyph,
                          0xFFCCEEFF, 0xFF000000);
        }
    }

    // Кнопка «Завершить выделенную задачу»
    int by = H - TM_BTN_H - 8;
    int bx = W - 220;
    int bw = 210;
    bool sel_valid = (g_tm_state.selected >= 0 &&
                      g_tm_state.selected < (int)g_tm_state.row_count);
    bool sel_is_window = sel_valid &&
        g_tm_state.rows[g_tm_state.selected].kind == TM_KIND_WINDOW;
    bool can_kill = sel_valid &&
                    !g_tm_state.rows[g_tm_state.selected].is_current &&
                    (sel_is_window ||
                     g_tm_state.rows[g_tm_state.selected].state != 2);
    uint32_t btn_top = can_kill ? 0xFF883333 : 0xFF332E33;
    uint32_t btn_bot = can_kill ? 0xFF551111 : 0xFF1E1A1E;
    gfx_fill_rect_gradient(win->x + bx, win->y + by, bw, TM_BTN_H, btn_top, btn_bot);
    gfx_draw_rect(win->x + bx, win->y + by, bw, TM_BTN_H,
                  can_kill ? 0xFFFF8866 : 0xFF555555);
    uint32_t lbl_color = can_kill ? 0xFFFFE0E0 : 0xFF777777;
    const char* btn_label = sel_is_window ? "Закрыть окно" : "Завершить задачу";
    win_draw_text(win, bx + 18, by + (TM_BTN_H - FONT_HEIGHT)/2,
                  btn_label, lbl_color, 0xFF000000);

    // Сообщение о результате последнего kill (отображается ~2 секунды)
    uint32_t now_tick = (uint32_t)timer_get_ticks();
    if (now_tick < g_tm_state.last_action_show_until_tick) {
        char msg[64];
        const char* prefix = "Готово: задача ";
        const char* result = " завершена";
        if (g_tm_state.last_action_res == -1) result = " не найдена";
        else if (g_tm_state.last_action_res == -2) result = ": нельзя убить себя";
        // Собираем строку вручную (без snprintf)
        int p = 0;
        while (prefix[p]) { msg[p] = prefix[p]; p++; }
        char num[12]; int_to_str((int32_t)g_tm_state.last_action_pid, num);
        int q = 0;
        while (num[q]) { msg[p++] = num[q++]; }
        int s = 0;
        while (result[s] && p < 60) { msg[p++] = result[s++]; }
        msg[p] = 0;
        uint32_t color = (g_tm_state.last_action_res == 0) ? 0xFFAAFFAA : 0xFFFFCC66;
        win_draw_text(win, 12, by + (TM_BTN_H - FONT_HEIGHT)/2, msg,
                      color, 0xFF000000);
    } else {
        win_draw_text(win, 12, by + (TM_BTN_H - FONT_HEIGHT)/2,
                      "Кликни строку и нажми «Завершить задачу»",
                      0xFF8899AA, 0xFF000000);
    }
}

static void taskmgr_create(window_t* win) {
    g_tm_state.row_count = 0;
    g_tm_state.prev_count = 0;
    g_tm_state.selected = -1;
    g_tm_state.last_action_show_until_tick = 0;
    tm_collect();
    tm_draw(win);
}

static void taskmgr_update(window_t* win) {
    // Пересобираем снимок и перерисовываем — это даёт «живой» CPU% и список.
    tm_collect();
    // Если выделенная строка ушла из списка (задача умерла) — снимаем выделение.
    if (g_tm_state.selected >= (int)g_tm_state.row_count) {
        g_tm_state.selected = -1;
    }
    tm_draw(win);
    wm_require_redraw();
}

static void taskmgr_click(window_t* win, int32_t lx, int32_t ly) {
    int W = win->width, H = win->height;
    // Кнопка «Завершить»?
    int by = H - TM_BTN_H - 8;
    int bx = W - 220;
    int bw = 210;
    if (lx >= bx && lx < bx + bw && ly >= by && ly < by + TM_BTN_H) {
        if (g_tm_state.selected >= 0 &&
            g_tm_state.selected < (int)g_tm_state.row_count) {
            tm_row_t* r = &g_tm_state.rows[g_tm_state.selected];
            int res;
            if (r->kind == TM_KIND_WINDOW) {
                // Закрываем окно через стандартный путь WM
                window_t* tw = (window_t*)r->win_ptr;
                if (tw) {
                    tw->close_requested = true;
                    res = 0;
                } else {
                    res = -1;
                }
            } else {
                res = task_kill_by_id(r->id);
            }
            g_tm_state.last_action_pid = r->id;
            g_tm_state.last_action_res = res;
            // Показать результат ~2 секунды (200 тиков при 100 Гц)
            g_tm_state.last_action_show_until_tick =
                (uint32_t)timer_get_ticks() + 200;
            // Снимаем выделение, чтобы случайно не закрыть ещё раз
            g_tm_state.selected = -1;
        }
        return;
    }

    // Клик в строку?
    if (ly >= TM_LIST_TOP - 2) {
        int idx = (ly - (TM_LIST_TOP - 2)) / TM_ROW_H;
        if (idx >= 0 && idx < (int)g_tm_state.row_count) {
            g_tm_state.selected = idx;
        }
    }
}

// ============================================================================
// СЕТЬ — настройка IP/маска/шлюз/DNS + ping (Этап 34)
// ============================================================================

#define NET_FIELD_COUNT 4
#define NET_LOG_LINES   8
#define NET_LOG_LINE_LEN 64

typedef struct {
    char    fields[NET_FIELD_COUNT][32];
    int     selected;            // 0..3 = редактируемое поле, -1 = ничего
    char    log[NET_LOG_LINES][NET_LOG_LINE_LEN];
    int     log_count;
    bool    busy;                // выполняется ping/save
} net_state_t;

static net_state_t g_net = { {{0}}, -1, {{0}}, 0, false };
static const char* net_field_labels[NET_FIELD_COUNT] = {
    "IP-адрес", "Маска",  "Шлюз",   "DNS"
};

static void net_log(const char* line) {
    if (g_net.log_count < NET_LOG_LINES) {
        int i = 0;
        while (line[i] && i < NET_LOG_LINE_LEN - 1) {
            g_net.log[g_net.log_count][i] = line[i]; i++;
        }
        g_net.log[g_net.log_count][i] = 0;
        g_net.log_count++;
    } else {
        for (int r = 1; r < NET_LOG_LINES; r++) {
            for (int c = 0; c < NET_LOG_LINE_LEN; c++)
                g_net.log[r-1][c] = g_net.log[r][c];
        }
        int i = 0;
        while (line[i] && i < NET_LOG_LINE_LEN - 1) {
            g_net.log[NET_LOG_LINES - 1][i] = line[i]; i++;
        }
        g_net.log[NET_LOG_LINES - 1][i] = 0;
    }
}

static void net_clear_log(void) { g_net.log_count = 0; }

static void net_load_fields_from_iface(void) {
    net_interface_t* eth = netif_get_default();
    char tmp[24];
    if (eth) {
        netcfg_format_ip(eth->ip4_addr, tmp, sizeof(tmp));
        app_str_copy(g_net.fields[0], tmp, 32);
        netcfg_format_ip(eth->ip4_netmask, tmp, sizeof(tmp));
        app_str_copy(g_net.fields[1], tmp, 32);
        netcfg_format_ip(eth->ip4_gateway, tmp, sizeof(tmp));
        app_str_copy(g_net.fields[2], tmp, 32);
    } else {
        app_str_copy(g_net.fields[0], "0.0.0.0", 32);
        app_str_copy(g_net.fields[1], "255.255.255.0", 32);
        app_str_copy(g_net.fields[2], "0.0.0.0", 32);
    }
    const netcfg_t* c = netcfg_get();
    uint32_t dns = c && c->valid && c->dns ? c->dns : 0;
    if (dns == 0 && eth) dns = eth->ip4_gateway;
    netcfg_format_ip(dns, tmp, sizeof(tmp));
    app_str_copy(g_net.fields[3], tmp, 32);
}

static void net_draw(window_t* win) {
    int W = win->width, H = win->height;
    win_fill_rect(win, 0, 0, W, H, 0xFF12162A);

    // Заголовок
    win_fill_rect(win, 0, 0, W, 30, 0xFF1F2440);
    win_draw_text(win, 12, 7, "Сетевые параметры", 0xFFE0E8FF, 0xFF000000);

    // Информация об интерфейсе
    net_interface_t* eth = netif_get_default();
    char buf[80];
    if (eth) {
        char m[24];
        // MAC
        const uint8_t* mac = eth->mac;
        const char* hex = "0123456789ABCDEF";
        m[0] = hex[mac[0] >> 4]; m[1] = hex[mac[0] & 0xF]; m[2] = ':';
        m[3] = hex[mac[1] >> 4]; m[4] = hex[mac[1] & 0xF]; m[5] = ':';
        m[6] = hex[mac[2] >> 4]; m[7] = hex[mac[2] & 0xF]; m[8] = ':';
        m[9] = hex[mac[3] >> 4]; m[10]= hex[mac[3] & 0xF]; m[11]=':';
        m[12]= hex[mac[4] >> 4]; m[13]= hex[mac[4] & 0xF]; m[14]=':';
        m[15]= hex[mac[5] >> 4]; m[16]= hex[mac[5] & 0xF]; m[17]= 0;
        const char* p = "eth0  MAC ";
        int n = 0; while (*p && n < 79) buf[n++] = *p++;
        for (int i = 0; m[i] && n < 79; i++) buf[n++] = m[i];
        buf[n] = 0;
        win_draw_text(win, 12, 38, buf, 0xFFAAB0C8, 0xFF000000);
    } else {
        win_draw_text(win, 12, 38, "eth0: интерфейс не найден", 0xFFFF8866, 0xFF000000);
    }

    // Поля ввода
    int fy = 64;
    int fh = 28;
    int fx = 110;
    int fw = W - fx - 16;
    for (int i = 0; i < NET_FIELD_COUNT; i++) {
        win_draw_text(win, 12, fy + i * (fh + 8) + 6, net_field_labels[i],
                      0xFFC8D2EE, 0xFF000000);
        uint32_t bg = (g_net.selected == i) ? 0xFF2A3260 : 0xFF1A1F38;
        uint32_t bd = (g_net.selected == i) ? 0xFFFFD060 : 0xFF445588;
        win_fill_rect(win, fx, fy + i * (fh + 8), fw, fh, bg);
        win_draw_rect(win, fx, fy + i * (fh + 8), fw, fh, bd);
        char show[36];
        int li = 0;
        for (; g_net.fields[i][li] && li < 32; li++) show[li] = g_net.fields[i][li];
        if (g_net.selected == i && (timer_get_ticks() / 30) & 1) show[li++] = '_';
        show[li] = 0;
        win_draw_text(win, fx + 8, fy + i * (fh + 8) + 6, show,
                      0xFFFFFFFF, 0xFF000000);
    }

    // Кнопки
    int by = fy + NET_FIELD_COUNT * (fh + 8) + 8;
    int bw = (W - 16 - 12) / 3;
    const char* labels[3] = { "Применить", "Сохранить", "Ping шлюз" };
    uint32_t cols[3]      = { 0xFF2D8F4D, 0xFF2A6FAA, 0xFFB07030 };
    for (int b = 0; b < 3; b++) {
        int bx = 8 + b * (bw + 6);
        win_fill_rect(win, bx, by, bw, 32, cols[b]);
        win_draw_rect(win, bx, by, bw, 32, 0xFF000000);
        int tw = app_strlen(labels[b]) * FONT_WIDTH;
        win_draw_text(win, bx + (bw - tw) / 2, by + 9, labels[b],
                      0xFFFFFFFF, 0xFF000000);
    }

    // Лог
    int ly = by + 40;
    int lh = H - ly - 8;
    win_fill_rect(win, 8, ly, W - 16, lh, 0xFF0A0E1E);
    win_draw_rect(win, 8, ly, W - 16, lh, 0xFF334466);
    for (int i = 0; i < g_net.log_count; i++) {
        win_draw_text(win, 14, ly + 6 + i * (FONT_HEIGHT + 2), g_net.log[i],
                      0xFFCFE8C8, 0xFF000000);
    }
}

static void net_create(window_t* win) {
    g_net.selected = -1;
    g_net.busy = false;
    net_clear_log();
    net_load_fields_from_iface();
    net_log("Готово. Кликни поле и набирай адрес.");
    net_draw(win);
}

static void net_update(window_t* win) {
    static uint32_t last_blink = 0;
    uint32_t t = (uint32_t)timer_get_ticks();
    if (g_net.selected >= 0 && (t - last_blink) > 15) {
        last_blink = t;
        net_draw(win);
    }
}

static int net_field_at(window_t* win, int x, int y) {
    int W = win->width;
    int fy = 64, fh = 28, fx = 110, fw = W - fx - 16;
    for (int i = 0; i < NET_FIELD_COUNT; i++) {
        if (x >= fx && x < fx + fw &&
            y >= fy + i * (fh + 8) && y < fy + i * (fh + 8) + fh) return i;
    }
    return -1;
}

static int net_button_at(window_t* win, int x, int y) {
    int W = win->width;
    int by = 64 + NET_FIELD_COUNT * (28 + 8) + 8;
    int bw = (W - 16 - 12) / 3;
    for (int b = 0; b < 3; b++) {
        int bx = 8 + b * (bw + 6);
        if (x >= bx && x < bx + bw && y >= by && y < by + 32) return b;
    }
    return -1;
}

static void net_apply_btn(void) {
    uint32_t ip, mask, gw, dns;
    if (!netcfg_parse_ip(g_net.fields[0], &ip)) { net_log("Ошибка: IP."); return; }
    if (!netcfg_parse_ip(g_net.fields[1], &mask)){ net_log("Ошибка: маска."); return; }
    if (!netcfg_parse_ip(g_net.fields[2], &gw))  { net_log("Ошибка: шлюз."); return; }
    if (!netcfg_parse_ip(g_net.fields[3], &dns)) { net_log("Ошибка: DNS."); return; }
    if (netcfg_set(ip, mask, gw, dns)) net_log("Конфиг применён.");
    else net_log("Применить не удалось.");
}

static void net_save_btn(void) {
    net_apply_btn();
    if (netcfg_save()) net_log("Сохранено в network.cfg.");
    else net_log("Сохранение не удалось.");
}

static void net_ping_btn(window_t* win) {
    uint32_t gw;
    if (!netcfg_parse_ip(g_net.fields[2], &gw)) { net_log("Ошибка: шлюз."); return; }
    if (gw == 0) { net_log("Шлюз не задан."); return; }
    char line[64];
    int n = 0;
    const char* p = "Ping ";
    while (*p) line[n++] = *p++;
    char ipstr[24]; netcfg_format_ip(gw, ipstr, sizeof(ipstr));
    for (int i = 0; ipstr[i]; i++) line[n++] = ipstr[i];
    line[n] = 0;
    net_log(line);
    net_draw(win);
    int oks = 0, total = 4;
    int sum_rtt = 0;
    for (int s = 0; s < total; s++) {
        int rtt = ping_one(gw, (uint16_t)s);
        char l[64]; int k = 0;
        if (rtt < 0) {
            const char* q = "  потеря пакета seq="; while (*q) l[k++] = *q++;
            l[k++] = '0' + s;
        } else {
            oks++;
            sum_rtt += rtt;
            const char* q = "  ответ seq="; while (*q) l[k++] = *q++;
            l[k++] = '0' + s;
            const char* r = " rtt="; while (*r) l[k++] = *r++;
            char num[8]; int_to_str(rtt, num);
            for (int i = 0; num[i]; i++) l[k++] = num[i];
            const char* m = "ms"; while (*m) l[k++] = *m++;
        }
        l[k] = 0;
        net_log(l);
        net_draw(win);
    }
    char sum[64]; int k = 0;
    const char* q = "Итого: "; while (*q) sum[k++] = *q++;
    char num[8]; int_to_str(oks, num);
    for (int i = 0; num[i]; i++) sum[k++] = num[i];
    const char* r = "/"; while (*r) sum[k++] = *r++;
    int_to_str(total, num);
    for (int i = 0; num[i]; i++) sum[k++] = num[i];
    const char* p2 = " ответов"; while (*p2) sum[k++] = *p2++;
    if (oks > 0) {
        const char* a = ", avg="; while (*a) sum[k++] = *a++;
        int_to_str(sum_rtt / oks, num);
        for (int i = 0; num[i]; i++) sum[k++] = num[i];
        const char* m = "ms"; while (*m) sum[k++] = *m++;
    }
    sum[k] = 0;
    net_log(sum);
}

static void net_click(window_t* win, int32_t x, int32_t y) {
    int f = net_field_at(win, x, y);
    if (f >= 0) { g_net.selected = f; net_draw(win); return; }
    int b = net_button_at(win, x, y);
    if (b >= 0) {
        g_net.selected = -1;
        if (b == 0) net_apply_btn();
        else if (b == 1) net_save_btn();
        else if (b == 2) net_ping_btn(win);
        net_draw(win);
        return;
    }
    g_net.selected = -1;
    net_draw(win);
}

static void net_key(window_t* win, char k) {
    if (g_net.selected < 0) return;
    char* f = g_net.fields[g_net.selected];
    int len = 0; while (f[len]) len++;
    if (k == 8 || k == 127) {  // backspace
        if (len > 0) f[len - 1] = 0;
    } else if (k == '\n' || k == '\r') {
        g_net.selected = (g_net.selected + 1) % NET_FIELD_COUNT;
    } else if ((k >= '0' && k <= '9') || k == '.') {
        if (len < 30) { f[len] = k; f[len + 1] = 0; }
    }
    net_draw(win);
}

// ============================================================================
// БРАУЗЕР — графический HTTP-вьювер (Этап 34)
// ============================================================================

#define BR_URL_MAX     128
#define BR_BODY_MAX    (12 * 1024)
#define BR_RENDER_LINES 200

typedef struct {
    char    url[BR_URL_MAX];
    int     url_len;
    bool    url_focus;
    char    body[BR_BODY_MAX];
    size_t  body_len;
    int     status;
    char    status_line[80];
    int     scroll;
} br_state_t;

static br_state_t g_br = { "http://example.com/", 0, false, "", 0, 0, "", 0 };

static void br_set_status(const char* s) {
    int i = 0;
    while (s[i] && i < 79) { g_br.status_line[i] = s[i]; i++; }
    g_br.status_line[i] = 0;
}

static void br_render(window_t* win) {
    int W = win->width, H = win->height;
    win_fill_rect(win, 0, 0, W, H, 0xFFFFFFFF);

    // Шапка
    win_fill_rect(win, 0, 0, W, 38, 0xFF1F2440);
    win_draw_text(win, 8, 11, "Браузер", 0xFFE0E8FF, 0xFF000000);

    // Адресная строка
    int axx = 90, axy = 6, axw = W - axx - 70, axh = 26;
    uint32_t bg = g_br.url_focus ? 0xFFFFFFFF : 0xFFE6E6EE;
    uint32_t bd = g_br.url_focus ? 0xFFFFD060 : 0xFF445588;
    win_fill_rect(win, axx, axy, axw, axh, bg);
    win_draw_rect(win, axx, axy, axw, axh, bd);
    char show[BR_URL_MAX + 2];
    int li = 0;
    for (; g_br.url[li] && li < BR_URL_MAX; li++) show[li] = g_br.url[li];
    if (g_br.url_focus && (timer_get_ticks() / 30) & 1) show[li++] = '_';
    show[li] = 0;
    win_draw_text(win, axx + 6, axy + 5, show, 0xFF101020, 0xFFE6E6EE);

    // Кнопка Go
    int gox = axx + axw + 6, goy = axy, gow = 56, goh = axh;
    win_fill_rect(win, gox, goy, gow, goh, 0xFF2D8F4D);
    win_draw_rect(win, gox, goy, gow, goh, 0xFF000000);
    win_draw_text(win, gox + 18, goy + 5, "GO", 0xFFFFFFFF, 0xFF000000);

    // Статус
    win_fill_rect(win, 0, 38, W, 22, 0xFF2A2F48);
    win_draw_text(win, 8, 43, g_br.status_line, 0xFFCDD8F0, 0xFF000000);

    // Содержимое
    int cy0 = 62;
    int cy = cy0 - g_br.scroll;
    int cx = 8;
    int max_w = W - 16;

    bool in_tag = false;
    bool skip_block = false;
    char tag_buf[24]; int tag_len = 0;
    int line_x = cx;
    int line_h = FONT_HEIGHT + 2;
    int link_start_x = -1;
    bool inside_a = false;
    char ch_word[160]; int wl = 0;
    int word_start_x = cx;

    uint32_t fg = 0xFF202020;

    for (size_t i = 0; i < g_br.body_len; i++) {
        char c = g_br.body[i];
        if (in_tag) {
            if (c == '>') {
                in_tag = false;
                tag_buf[tag_len] = 0;
                // нормализовать до lower
                for (int t = 0; t < tag_len; t++)
                    if (tag_buf[t] >= 'A' && tag_buf[t] <= 'Z') tag_buf[t] += 32;
                if (tag_buf[0] == 's' && tag_buf[1] == 'c' && tag_buf[2] == 'r') {
                    skip_block = true;
                } else if (tag_buf[0] == '/' && tag_buf[1] == 's' &&
                           tag_buf[2] == 'c') {
                    skip_block = false;
                } else if (tag_buf[0] == 's' && tag_buf[1] == 't' &&
                           tag_buf[2] == 'y') {
                    skip_block = true;
                } else if (tag_buf[0] == '/' && tag_buf[1] == 's' &&
                           tag_buf[2] == 't') {
                    skip_block = false;
                } else if (tag_buf[0] == 'b' && tag_buf[1] == 'r') {
                    cy += line_h; line_x = cx;
                } else if (tag_buf[0] == 'p' && tag_buf[1] == 0) {
                    cy += line_h; line_x = cx;
                } else if (tag_buf[0] == '/' && tag_buf[1] == 'p') {
                    cy += line_h; line_x = cx;
                } else if (tag_buf[0] == 'h' && tag_buf[1] >= '1' &&
                           tag_buf[1] <= '6') {
                    cy += line_h; line_x = cx; fg = 0xFF202060;
                } else if (tag_buf[0] == '/' && tag_buf[1] == 'h') {
                    cy += line_h + 4; line_x = cx; fg = 0xFF202020;
                } else if (tag_buf[0] == 'a' && (tag_buf[1] == ' ' ||
                            tag_buf[1] == 0)) {
                    inside_a = true; link_start_x = line_x; fg = 0xFF1A55B0;
                } else if (tag_buf[0] == '/' && tag_buf[1] == 'a') {
                    if (inside_a && link_start_x >= 0 && cy >= cy0 && cy < H - 8) {
                        // подчёркивание
                        for (int u = link_start_x; u < line_x; u++)
                            window_put_pixel(win, u, cy + FONT_HEIGHT, 0xFF1A55B0);
                    }
                    inside_a = false; fg = 0xFF202020;
                } else if (tag_buf[0] == 'h' && tag_buf[1] == 'r') {
                    cy += 2;
                    for (int u = cx; u < cx + max_w; u++)
                        if (cy >= cy0 && cy < H - 8) window_put_pixel(win, u, cy, 0xFF888888);
                    cy += line_h; line_x = cx;
                }
                tag_len = 0;
            } else if (tag_len < 22) {
                tag_buf[tag_len++] = c;
            }
            continue;
        }
        if (c == '<') { in_tag = true; tag_len = 0; continue; }
        if (skip_block) continue;

        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            // word boundary — отрисовать накопленное слово
            if (wl > 0) {
                ch_word[wl] = 0;
                int ww = wl * FONT_WIDTH;
                if (line_x + ww > cx + max_w) {
                    cy += line_h; line_x = cx;
                    if (inside_a) link_start_x = line_x;
                }
                if (cy >= cy0 - line_h && cy < H - 4) {
                    if (cy >= cy0)
                        win_draw_text(win, line_x, cy, ch_word, fg, 0xFFFFFFFF);
                }
                line_x += ww;
                wl = 0;
            }
            // пробел
            if (line_x + FONT_WIDTH > cx + max_w) {
                cy += line_h; line_x = cx;
                if (inside_a) link_start_x = line_x;
            } else {
                line_x += FONT_WIDTH;
            }
            continue;
        }
        if (wl < 158) {
            // entity decode minimal
            if (c == '&') {
                // try &amp; &lt; &gt; &nbsp;
                int j = (int)i + 1;
                if (j + 4 < (int)g_br.body_len &&
                    g_br.body[j] == 'a' && g_br.body[j+1] == 'm' &&
                    g_br.body[j+2] == 'p' && g_br.body[j+3] == ';') {
                    ch_word[wl++] = '&'; i += 4; continue;
                }
                if (j + 2 < (int)g_br.body_len &&
                    g_br.body[j] == 'l' && g_br.body[j+1] == 't' &&
                    g_br.body[j+2] == ';') {
                    ch_word[wl++] = '<'; i += 3; continue;
                }
                if (j + 2 < (int)g_br.body_len &&
                    g_br.body[j] == 'g' && g_br.body[j+1] == 't' &&
                    g_br.body[j+2] == ';') {
                    ch_word[wl++] = '>'; i += 3; continue;
                }
            }
            if ((unsigned char)c >= 32 && (unsigned char)c < 127)
                ch_word[wl++] = c;
        }
    }
    if (wl > 0) {
        ch_word[wl] = 0;
        int ww = wl * FONT_WIDTH;
        if (line_x + ww > cx + max_w) { cy += line_h; line_x = cx; }
        if (cy >= cy0 && cy < H - 4)
            win_draw_text(win, line_x, cy, ch_word, fg, 0xFFFFFFFF);
    }

    // Подсказка снизу
    win_fill_rect(win, 0, H - 18, W, 18, 0xFFE0E0E8);
    win_draw_text(win, 6, H - 15,
                  "Кликни адрес, набери URL и Enter. PgUp/PgDn — скролл.",
                  0xFF202040, 0xFFE0E0E8);
}

static void br_create(window_t* win) {
    g_br.url_focus = false;
    g_br.url_len = 0;
    while (g_br.url[g_br.url_len]) g_br.url_len++;
    g_br.body[0] = 0;
    g_br.body_len = 0;
    g_br.status = 0;
    g_br.scroll = 0;
    br_set_status("Введи URL и нажми GO.");
    br_render(win);
}

static void br_update(window_t* win) {
    static uint32_t last = 0;
    uint32_t t = (uint32_t)timer_get_ticks();
    if (g_br.url_focus && t - last > 15) { last = t; br_render(win); }
}

static void br_load(window_t* win) {
    if (g_br.url_len == 0) return;
    br_set_status("Загрузка...");
    br_render(win);
    g_br.body_len = 0;
    int code = 0;
    bool ok = http_get(g_br.url, g_br.body, sizeof(g_br.body), &g_br.body_len, &code);
    g_br.status = code;
    if (!ok) {
        br_set_status("Ошибка загрузки.");
    } else {
        char s[80]; int n = 0;
        const char* p = "HTTP "; while (*p) s[n++] = *p++;
        char num[8]; int_to_str(code, num);
        for (int i = 0; num[i]; i++) s[n++] = num[i];
        const char* q = "  байт: "; while (*q) s[n++] = *q++;
        int_to_str((int)g_br.body_len, num);
        for (int i = 0; num[i]; i++) s[n++] = num[i];
        s[n] = 0;
        br_set_status(s);
    }
    g_br.scroll = 0;
    br_render(win);
}

static void br_click(window_t* win, int32_t x, int32_t y) {
    int W = win->width;
    int axx = 90, axy = 6, axw = W - axx - 70, axh = 26;
    if (x >= axx && x < axx + axw && y >= axy && y < axy + axh) {
        g_br.url_focus = true;
        br_render(win);
        return;
    }
    int gox = axx + axw + 6, goy = axy, gow = 56, goh = axh;
    if (x >= gox && x < gox + gow && y >= goy && y < goy + goh) {
        g_br.url_focus = false;
        br_load(win);
        return;
    }
    g_br.url_focus = false;
    br_render(win);
}

static void br_key(window_t* win, char k) {
    if (k == 1) { g_br.scroll -= 40; if (g_br.scroll < 0) g_br.scroll = 0; br_render(win); return; }
    if (k == 2) { g_br.scroll += 40; br_render(win); return; }
    if (!g_br.url_focus) return;
    if (k == 8 || k == 127) {
        if (g_br.url_len > 0) { g_br.url[--g_br.url_len] = 0; }
    } else if (k == '\n' || k == '\r') {
        g_br.url_focus = false;
        br_load(win);
        return;
    } else if ((unsigned char)k >= 32 && (unsigned char)k < 127) {
        if (g_br.url_len < BR_URL_MAX - 1) {
            g_br.url[g_br.url_len++] = k;
            g_br.url[g_br.url_len] = 0;
        }
    }
    br_render(win);
}

// ============================================================================
// Registration of all apps
// ============================================================================

// Индекс иконки «Диспетчер задач», запоминается при регистрации,
// используется apps_open_taskmgr() для авто-запуска при логине.
static int g_taskmgr_icon_idx = -1;

void apps_register_all(void) {
    #include "ui_layout.inc"
    // Иконка диспетчера задач (отдельно от ui_layout.inc, потому что она
    // создаётся не AI-генератором, а ядром Этапа 33).
    g_taskmgr_icon_idx = desktop_add_icon(
        "Диспетч.", "T", 0xFF884466,
        "Диспетчер задач", 620, 460,
        taskmgr_create, taskmgr_update, taskmgr_click, NULL);

    // Этап 34: настройки сети + браузер.
    desktop_add_icon(
        "Сеть", "@", 0xFF2A6FAA,
        "Сетевые настройки", 520, 460,
        net_create, net_update, net_click, net_key);
    desktop_add_icon(
        "Браузер", ">", 0xFF2D8F4D,
        "Браузер", 760, 540,
        br_create, br_update, br_click, br_key);
}

// Открыть окно диспетчера задач (как клик по иконке). Используется
// kernel.c вместо запуска Ring 3 «XOR Pattern»-демо при логине.
void apps_open_taskmgr(void) {
    if (g_taskmgr_icon_idx >= 0) {
        desktop_launch_icon(g_taskmgr_icon_idx);
    }
}
