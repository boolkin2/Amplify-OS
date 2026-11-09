// ============================================================================
// graphics.h — высокоуровневые графические примитивы для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Все функции принимают опциональный «target buffer» — массив 32-bit ARGB
// пикселей с указанным pitch (в БАЙТАХ). Если target = NULL — рисуем
// напрямую в VESA framebuffer (через fb_state()->vaddr/pitch). Это даёт
// единый API и для doublebuf'а, и для per-window буферов.
// ============================================================================
#ifndef AOS_X86_64_GRAPHICS_H
#define AOS_X86_64_GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Описание целевого буфера. Все примитивы работают через этот дескриптор.
// ----------------------------------------------------------------------------
typedef struct {
    uint32_t* pixels;     // ARGB (xRGB) пиксели
    int       width;
    int       height;
    int       pitch;      // байт на строку (обычно width * 4)
} gfx_target_t;

// Текущий screen target (VESA fb). Возвращает NULL если fb не готов.
const gfx_target_t* gfx_screen(void);

// ----------------------------------------------------------------------------
// Цветовые helpers.
// ----------------------------------------------------------------------------
#define GFX_RGB(r,g,b)   (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define GFX_ARGB(a,r,g,b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

// Smooth alpha-blend src над dst (по pre-multiplied нет — обычная формула).
static inline uint32_t gfx_blend(uint32_t dst, uint32_t src) {
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0)   return dst;
    if (a == 255) return src;
    uint32_t inv = 255 - a;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db =  dst        & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >>  8) & 0xFF;
    uint32_t sb =  src        & 0xFF;
    uint32_t r = (sr * a + dr * inv) / 255;
    uint32_t g = (sg * a + dg * inv) / 255;
    uint32_t b = (sb * a + db * inv) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// ----------------------------------------------------------------------------
// Примитивы. Если target == NULL → screen.
// ----------------------------------------------------------------------------
void gfx_put_pixel       (const gfx_target_t* tgt, int x, int y, uint32_t color);
void gfx_clear           (const gfx_target_t* tgt, uint32_t color);
void gfx_fill_rect       (const gfx_target_t* tgt, int x, int y, int w, int h, uint32_t color);
void gfx_stroke_rect     (const gfx_target_t* tgt, int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect_gradient(const gfx_target_t* tgt, int x, int y, int w, int h,
                             uint32_t top_color, uint32_t bot_color);

// Скруглённый прямоугольник: 4 угла радиуса r закруглены через простой
// circle-test. Не идеальный антиалиасинг, но в 1024×768 выглядит ок.
void gfx_fill_round_rect (const gfx_target_t* tgt, int x, int y, int w, int h,
                          int radius, uint32_t color);

// Текст 8×16 (тот же шрифт, что в fb).
void gfx_draw_char       (const gfx_target_t* tgt, int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_string     (const gfx_target_t* tgt, int x, int y, const char* s, uint32_t fg, uint32_t bg);
int  gfx_string_width    (const char* s);   // = strlen * 8
// Transparent text: background pixels are preserved instead of being painted
// black. Scaled variant is used by the Pearl shell until the TTF atlas lands.
void gfx_draw_char_transparent(const gfx_target_t* tgt, int x, int y, char c, uint32_t fg);
void gfx_draw_string_transparent(const gfx_target_t* tgt, int x, int y, const char* s, uint32_t fg);
void gfx_draw_string_scaled(const gfx_target_t* tgt, int x, int y, const char* s,
                            uint32_t fg, int scale);
int  gfx_string_width_scaled(const char* s, int scale);

// Blit из source-target в dst-target. Прозрачные пиксели (alpha=0) пропускаются.
void gfx_blit            (const gfx_target_t* dst, int dx, int dy,
                          const gfx_target_t* src, int sx, int sy, int w, int h);

// Тень: drop-shadow вниз-вправо от прямоугольника, с быстрым приближением
// блюра (3 итерации box-fill с alpha=...).
void gfx_drop_shadow     (const gfx_target_t* tgt, int x, int y, int w, int h,
                          int offset, uint32_t shadow_argb);

// ----------------------------------------------------------------------------
// Ширина текста пропорциональной гарнитурой (этап F).
//   gfx_string_width()        — моно-сетка 8 px (пара к gfx_draw_string).
//   gfx_string_width_ui()     — пара к gfx_draw_string_transparent().
//   gfx_string_width_scaled() — пара к gfx_draw_string_scaled().
// ----------------------------------------------------------------------------
int gfx_string_width_ui(const char* s);

#endif
