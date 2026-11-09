// ============================================================================
// fb.c — линейный VESA framebuffer + 8x16 текстовая консоль.
// ============================================================================
#include "fb.h"
#include "ui_font.h"
#include "serial.h"

static aos64_fb_t state;

// Состояние "console" — где сейчас курсор для console_putc.
static int      cur_row, cur_col;
static int      cols, rows;
static uint32_t cur_fg = 0xFFFFFFFF;
static uint32_t cur_bg = 0xFF101820;

void fb_init(const mb2_tag_framebuffer_t* tag) {
    state.ok = false;
    if (!tag) {
        serial_puts("[fb] Multiboot2 framebuffer tag missing — fallback VGA text.\n");
        return;
    }
    if (tag->fb_type != 1) {
        serial_puts("[fb] framebuffer type != RGB linear — fallback.\n");
        return;
    }
    if (tag->fb_bpp != 32) {
        serial_puts("[fb] only 32 bpp supported in stage 37.1.\n");
        return;
    }

    state.ok     = true;
    state.phys   = tag->fb_addr;
    state.vaddr  = (uint8_t*)(uintptr_t)tag->fb_addr;
    state.width  = tag->fb_width;
    state.height = tag->fb_height;
    state.pitch  = tag->fb_pitch;
    state.bpp    = tag->fb_bpp;

    cols = state.width  / AOS64_FONT_W;
    rows = state.height / AOS64_FONT_H;
    cur_row = cur_col = 0;

    serial_puts("[fb] linear ");
    serial_print_dec(state.width);
    serial_puts("x");
    serial_print_dec(state.height);
    serial_puts("x");
    serial_print_dec(state.bpp);
    serial_puts(" @ ");
    serial_print_hex(state.phys, 16);
    serial_puts("\n");

    fb_clear(cur_bg);
}

const aos64_fb_t* fb_state(void) { return &state; }

// [41.2] Логический размер экрана после смены режима через virtio-gpu. Сам
// линейный framebuffer прошивки (vaddr) остаётся прежнего размера и на экран
// больше не выводится — по width/height живут границы мыши и раскладка UI.
// Вызывающий гарантирует, что новый режим не больше исходного (см. drm_set_mode).
void fb_set_logical_mode(uint32_t w, uint32_t h) {
    state.width  = w;
    state.height = h;
    state.pitch  = w * 4;
}

void fb_put_pixel(int x, int y, uint32_t color) {
    if (!state.ok) return;
    if ((unsigned)x >= state.width || (unsigned)y >= state.height) return;
    uint32_t* row = (uint32_t*)(state.vaddr + (uint32_t)y * state.pitch);
    row[x] = color;
}

void fb_clear(uint32_t color) {
    if (!state.ok) return;
    for (uint32_t y = 0; y < state.height; y++) {
        uint32_t* row = (uint32_t*)(state.vaddr + y * state.pitch);
        for (uint32_t x = 0; x < state.width; x++) row[x] = color;
    }
}

void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!state.ok) return;
    /* Этап F: та же сетка 8×16, но сглаженная mono-глифа из атласа. */
    ui_font_draw_cell(gfx_screen(), x, y, c, fg, bg, UI_FONT_MONO_14);
}

void fb_draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += AOS64_FONT_H; }
        else { fb_draw_char(cx, y, *s, fg, bg); cx += AOS64_FONT_W; }
        s++;
    }
}

// ----------------------------------------------------------------------------
// Console (cursor-based): println-стиль для удобного вывода.
// ----------------------------------------------------------------------------

// Хук-перехват вывода — для GUI-терминала (см. fb.h).
static fb_console_putc_hook_fn  g_putc_hook  = 0;
static fb_console_reset_hook_fn g_reset_hook = 0;

void fb_console_set_hooks(fb_console_putc_hook_fn putc_hook,
                          fb_console_reset_hook_fn reset_hook) {
    g_putc_hook  = putc_hook;
    g_reset_hook = reset_hook;
}

void fb_console_reset(void) {
    if (g_reset_hook) { g_reset_hook(); return; }
    cur_row = cur_col = 0;
    if (state.ok) fb_clear(cur_bg);
}

void fb_console_set_color(uint32_t fg, uint32_t bg) {
    cur_fg = fg;
    cur_bg = bg;
}

static void console_scroll_if_needed(void) {
    if (!state.ok) return;
    if (cur_row < rows) return;

    // Опт. 37.5d-3a: qword-копирование вместо byte-by-byte. На QEMU TCG —
    // ~8× быстрее, что критично для длинных smoke-тестов.
    uint64_t qwords_per_row = state.pitch / 8;
    for (int y = AOS64_FONT_H; y < (int)state.height; y++) {
        uint64_t* dst = (uint64_t*)(state.vaddr + ((uint64_t)(y - AOS64_FONT_H)) * state.pitch);
        uint64_t* src = (uint64_t*)(state.vaddr + ((uint64_t)y) * state.pitch);
        for (uint64_t x = 0; x < qwords_per_row; x++) dst[x] = src[x];
    }
    // Очищаем последнюю строку — тоже qword'ами (2 пикселя 32-bpp в qword).
    uint64_t bg_q = ((uint64_t)cur_bg << 32) | (uint64_t)cur_bg;
    for (int y = (int)state.height - AOS64_FONT_H; y < (int)state.height; y++) {
        uint64_t* row = (uint64_t*)(state.vaddr + (uint64_t)y * state.pitch);
        uint64_t pixel_qwords = state.width / 2;
        for (uint64_t x = 0; x < pixel_qwords; x++) row[x] = bg_q;
    }
    cur_row = rows - 1;
}

void fb_console_putc(char c) {
    serial_putc(c);                 // дублируем в COM1
    if (g_putc_hook) { g_putc_hook(c); return; }
    if (!state.ok) return;

    if (c == '\n') {
        cur_col = 0; cur_row++;
        console_scroll_if_needed();
        return;
    }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') { cur_col = (cur_col + 8) & ~7; if (cur_col >= cols) { cur_col = 0; cur_row++; } console_scroll_if_needed(); return; }

    if (cur_col >= cols) { cur_col = 0; cur_row++; console_scroll_if_needed(); }
    fb_draw_char(cur_col * AOS64_FONT_W, cur_row * AOS64_FONT_H, c, cur_fg, cur_bg);
    cur_col++;
}

void fb_console_puts(const char* s) {
    while (*s) fb_console_putc(*s++);
}

void fb_console_print_hex(uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    // Префикс "0x" пишет вызывающий.
    for (int i = digits - 1; i >= 0; i--) fb_console_putc(hex[(v >> (i * 4)) & 0xF]);
}

void fb_console_print_dec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { fb_console_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) fb_console_putc(buf[--i]);
}
