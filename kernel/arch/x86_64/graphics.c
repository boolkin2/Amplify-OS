// ============================================================================
// graphics.c — реализация графических примитивов.
// ----------------------------------------------------------------------------
// Все примитивы работают через gfx_target_t. Если target == NULL → используем
// gfx_screen() (VESA framebuffer). Это позволяет одной и той же функцией
// рисовать и в offscreen-буфер doublebuf'а, и в окно, и напрямую в VRAM.
// ============================================================================

#include "graphics.h"
#include "fb.h"
#include "ui_font.h"

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Screen target. Кешируем дескриптор, обновляем при первом запросе.
// ----------------------------------------------------------------------------
static gfx_target_t g_screen = {0, 0, 0, 0};

const gfx_target_t* gfx_screen(void) {
    const aos64_fb_t* st = fb_state();
    if (!st->ok) return 0;
    g_screen.pixels = (uint32_t*)st->vaddr;
    g_screen.width  = (int)st->width;
    g_screen.height = (int)st->height;
    g_screen.pitch  = (int)st->pitch;
    return &g_screen;
}

static const gfx_target_t* resolve(const gfx_target_t* tgt) {
    return tgt ? tgt : gfx_screen();
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static inline uint32_t* row_at(const gfx_target_t* t, int y) {
    return (uint32_t*)((uint8_t*)t->pixels + (size_t)y * (size_t)t->pitch);
}

static inline void clip_rect(const gfx_target_t* t, int* x, int* y, int* w, int* h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > t->width)  *w = t->width  - *x;
    if (*y + *h > t->height) *h = t->height - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

// ----------------------------------------------------------------------------
// Pixel
// ----------------------------------------------------------------------------
void gfx_put_pixel(const gfx_target_t* tgt, int x, int y, uint32_t color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t) return;
    if ((unsigned)x >= (unsigned)t->width || (unsigned)y >= (unsigned)t->height) return;
    row_at(t, y)[x] = color;
}

// ----------------------------------------------------------------------------
// Clear / fill
// ----------------------------------------------------------------------------
void gfx_clear(const gfx_target_t* tgt, uint32_t color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t) return;
    gfx_fill_rect(t, 0, 0, t->width, t->height, color);
}

void gfx_fill_rect(const gfx_target_t* tgt, int x, int y, int w, int h, uint32_t color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t) return;
    clip_rect(t, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;

    // HOTFIX (19.05.2026, KVM perf): qword-страйд (2 пикселя за запись)
    // там где можно. Сразу убирает половину store-инструкций на больших
    // заливках (фон рабочего стола 1024x768 = 786k store -> 393k store).
    // Чувствительно под KVM-on-VMware где каждая store в LFB дороже,
    // чем на голом железе.
    uint64_t color64 = ((uint64_t)color << 32) | color;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = row_at(t, yy);
        int xx = x;
        // Выравнивание на 8 байт.
        if ((((uintptr_t)(row + xx)) & 7) && xx < x + w) {
            row[xx++] = color;
        }
        // Парные пиксели через qword-store.
        uint64_t* row64 = (uint64_t*)(row + xx);
        int pairs = (x + w - xx) / 2;
        for (int i = 0; i < pairs; i++) row64[i] = color64;
        xx += pairs * 2;
        // Хвост: один пиксель если ширина нечётная.
        if (xx < x + w) row[xx] = color;
    }
}

void gfx_stroke_rect(const gfx_target_t* tgt, int x, int y, int w, int h, uint32_t color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t || w <= 0 || h <= 0) return;
    gfx_fill_rect(t, x,         y,         w, 1, color);
    gfx_fill_rect(t, x,         y + h - 1, w, 1, color);
    gfx_fill_rect(t, x,         y,         1, h, color);
    gfx_fill_rect(t, x + w - 1, y,         1, h, color);
}

void gfx_fill_rect_gradient(const gfx_target_t* tgt, int x, int y, int w, int h,
                            uint32_t top_color, uint32_t bot_color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t || w <= 0 || h <= 0) return;

    uint32_t r1 = (top_color >> 16) & 0xFF, g1 = (top_color >>  8) & 0xFF, b1 = top_color & 0xFF;
    uint32_t r2 = (bot_color >> 16) & 0xFF, g2 = (bot_color >>  8) & 0xFF, b2 = bot_color & 0xFF;

    for (int yy = 0; yy < h; yy++) {
        uint32_t k = ((uint32_t)yy * 256u) / (uint32_t)h;
        uint32_t r = (r1 * (256 - k) + r2 * k) / 256;
        uint32_t g = (g1 * (256 - k) + g2 * k) / 256;
        uint32_t b = (b1 * (256 - k) + b2 * k) / 256;
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        gfx_fill_rect(t, x, y + yy, w, 1, c);
    }
}

// ----------------------------------------------------------------------------
// Округлённый прямоугольник.
// ----------------------------------------------------------------------------
void gfx_fill_round_rect(const gfx_target_t* tgt, int x, int y, int w, int h,
                         int radius, uint32_t color) {
    const gfx_target_t* t = resolve(tgt);
    if (!t || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    // Центральная горизонтальная полоса (без углов).
    gfx_fill_rect(t, x,             y + radius, w,                   h - 2 * radius, color);
    // Верх и низ без углов.
    gfx_fill_rect(t, x + radius,    y,          w - 2 * radius,      radius,         color);
    gfx_fill_rect(t, x + radius,    y + h - radius, w - 2 * radius,  radius,         color);

    // 4 угла — круги через циркульный тест.
    int r2 = radius * radius;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int rx = radius - 1 - dx;
            int ry = radius - 1 - dy;
            if (rx * rx + ry * ry <= r2) {
                gfx_put_pixel(t, x + dx,             y + dy,             color);
                gfx_put_pixel(t, x + w - 1 - dx,     y + dy,             color);
                gfx_put_pixel(t, x + dx,             y + h - 1 - dy,     color);
                gfx_put_pixel(t, x + w - 1 - dx,     y + h - 1 - dy,     color);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Текст (этап F): сглаженные атласы ui_font.
//   * gfx_draw_char / gfx_draw_string — консольная сетка 8×16 (mono-гарнитура),
//     метрики совпадают со старым пиксельным шрифтом.
//   * *_transparent / *_scaled — пропорциональная гарнитура Pearl.
// ----------------------------------------------------------------------------
void gfx_draw_char(const gfx_target_t* tgt, int x, int y, char c, uint32_t fg, uint32_t bg) {
    ui_font_draw_cell(tgt, x, y, c, fg, bg, UI_FONT_MONO_14);
}

void gfx_draw_string(const gfx_target_t* tgt, int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    int cx = x;
    while (s && *s) {
        if (*s == '\n') { cx = x; y += AOS64_FONT_H; }
        else { gfx_draw_char(tgt, cx, y, *s, fg, bg); cx += AOS64_FONT_W; }
        s++;
    }
}

void gfx_draw_char_transparent(const gfx_target_t* tgt, int x, int y, char c, uint32_t fg) {
    ui_font_draw_cell(tgt, x, y, c, fg, 0, UI_FONT_MONO_14);
}

void gfx_draw_string_transparent(const gfx_target_t* tgt, int x, int y, const char* s, uint32_t fg) {
    ui_font_draw_baseline(tgt, x, ui_font_baseline_for_box(UI_FONT_14, y, AOS64_FONT_H),
                          s, fg, UI_FONT_14);
}

void gfx_draw_string_scaled(const gfx_target_t* tgt, int x, int y, const char* s,
                            uint32_t fg, int scale) {
    if (scale < 1) scale = 1;
    ui_font_id_t id = ui_font_for_scale(scale);
    ui_font_draw_baseline(tgt, x, ui_font_baseline_for_box(id, y, AOS64_FONT_H * scale),
                          s, fg, id);
}

int gfx_string_width_scaled(const char* s, int scale) {
    if (scale < 1) scale = 1;
    return ui_font_text_width(s, ui_font_for_scale(scale));
}

int gfx_string_width(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n * AOS64_FONT_W;
}

int gfx_string_width_ui(const char* s) {
    return ui_font_text_width(s, UI_FONT_14);
}

// ----------------------------------------------------------------------------
// Blit. Пиксели с alpha=0 пропускаются (упрощённый transparent key).
// ----------------------------------------------------------------------------
void gfx_blit(const gfx_target_t* dst, int dx, int dy,
              const gfx_target_t* src, int sx, int sy, int w, int h) {
    const gfx_target_t* d = resolve(dst);
    if (!d || !src) return;

    // Clip к dst.
    int x0 = dx, y0 = dy, w0 = w, h0 = h;
    if (x0 < 0) { sx -= x0; w0 += x0; x0 = 0; }
    if (y0 < 0) { sy -= y0; h0 += y0; y0 = 0; }
    if (x0 + w0 > d->width)  w0 = d->width  - x0;
    if (y0 + h0 > d->height) h0 = d->height - y0;
    // Clip к src.
    if (sx < 0) { x0 -= sx; w0 += sx; sx = 0; }
    if (sy < 0) { y0 -= sy; h0 += sy; sy = 0; }
    if (sx + w0 > src->width)  w0 = src->width  - sx;
    if (sy + h0 > src->height) h0 = src->height - sy;
    if (w0 <= 0 || h0 <= 0) return;

    for (int yy = 0; yy < h0; yy++) {
        uint32_t* drow = row_at(d, y0 + yy) + x0;
        uint32_t* srow = row_at(src, sy + yy) + sx;
        for (int xx = 0; xx < w0; xx++) {
            uint32_t s = srow[xx];
            if ((s & 0xFF000000u) == 0) continue;     // полностью прозрачный
            if ((s & 0xFF000000u) == 0xFF000000u) {
                drow[xx] = s;                          // непрозрачный
            } else {
                drow[xx] = gfx_blend(drow[xx], s);     // smooth alpha
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Drop shadow — простая 3-итерационная имитация Gaussian blur.
// На 37.6c-2 — рисуем 3 концентрических полупрозрачных rect'а под целью.
// ----------------------------------------------------------------------------
void gfx_drop_shadow(const gfx_target_t* tgt, int x, int y, int w, int h,
                     int offset, uint32_t shadow_argb) {
    const gfx_target_t* t = resolve(tgt);
    if (!t || w <= 0 || h <= 0) return;
    if (offset < 1) offset = 4;

    // 3 слоя: extra-blurry → main → tight, по уменьшающемуся радиусу с
    // увеличивающейся непрозрачностью.
    uint32_t a   = (shadow_argb >> 24) & 0xFF;
    uint32_t rgb = shadow_argb & 0x00FFFFFFu;

    int x0 = x + offset;
    int y0 = y + offset;

    for (int pass = 0; pass < 3; pass++) {
        int pad   = (3 - pass) * 2;
        int alpha = (a * (pass + 1)) / 4;
        if (alpha > 255) alpha = 255;
        uint32_t c = ((uint32_t)alpha << 24) | rgb;

        for (int yy = -pad; yy < h + pad; yy++) {
            int ay = y0 + yy;
            if ((unsigned)ay >= (unsigned)t->height) continue;
            uint32_t* row = row_at(t, ay);
            for (int xx = -pad; xx < w + pad; xx++) {
                int ax = x0 + xx;
                if ((unsigned)ax >= (unsigned)t->width) continue;
                row[ax] = gfx_blend(row[ax], c);
            }
        }
    }
}
