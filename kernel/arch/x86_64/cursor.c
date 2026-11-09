// ============================================================================
// cursor.c — реализация графического курсора.
// ----------------------------------------------------------------------------
// 12×19 ASCII-art спрайт:
//   X = чёрный outline
//   . = белый fill
//   ' ' = transparent (не рисуем)
// ============================================================================
#include "cursor.h"
#include "graphics.h"
#include <stddef.h>
#include <stdbool.h>

static const char* g_sprite[CURSOR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X..........X",
    "X......XXXXX",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "      X..X  ",
    "       XX   ",
};

void cursor_draw(const gfx_target_t* tgt, int x, int y) {
    for (int dy = 0; dy < CURSOR_H; dy++) {
        const char* row = g_sprite[dy];
        for (int dx = 0; dx < CURSOR_W; dx++) {
            char c = row[dx];
            if (c == 'X')      gfx_put_pixel(tgt, x + dx, y + dy, 0xFF000000);
            else if (c == '.') gfx_put_pixel(tgt, x + dx, y + dy, 0xFFFFFFFF);
        }
    }
}

// ----------------------------------------------------------------------------
// [41.2] Save-under: фон под курсором хранится в g_under (обрезанный по
// границам буфера прямоугольник g_ux,g_uy,g_uw,g_uh).
// ----------------------------------------------------------------------------
static uint32_t g_under[CURSOR_W * CURSOR_H];
static int      g_ux, g_uy, g_uw, g_uh;
static bool     g_shown = false;

void cursor_overlay_forget(void) { g_shown = false; }

bool cursor_overlay_rect(int* x, int* y, int* w, int* h) {
    if (!g_shown) return false;
    *x = g_ux; *y = g_uy; *w = g_uw; *h = g_uh;
    return true;
}

void cursor_overlay_hide(const gfx_target_t* tgt) {
    if (!g_shown || !tgt || !tgt->pixels) return;
    for (int dy = 0; dy < g_uh; dy++) {
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)(g_uy + dy) * tgt->pitch) + g_ux;
        for (int dx = 0; dx < g_uw; dx++) row[dx] = g_under[dy * CURSOR_W + dx];
    }
    g_shown = false;
}

void cursor_overlay_show(const gfx_target_t* tgt, int x, int y) {
    if (!tgt || !tgt->pixels) return;
    // Обрезка по буферу: у правого/нижнего края виден только кусок спрайта.
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + CURSOR_W, y1 = y + CURSOR_H;
    if (x1 > tgt->width)  x1 = tgt->width;
    if (y1 > tgt->height) y1 = tgt->height;
    if (x1 <= x0 || y1 <= y0) { g_shown = false; return; }
    g_ux = x0; g_uy = y0; g_uw = x1 - x0; g_uh = y1 - y0;
    for (int dy = 0; dy < g_uh; dy++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)tgt->pixels + (size_t)(g_uy + dy) * tgt->pitch) + g_ux;
        for (int dx = 0; dx < g_uw; dx++) g_under[dy * CURSOR_W + dx] = row[dx];
    }
    g_shown = true;
    cursor_draw(tgt, x, y);
}

void cursor_render_argb(uint32_t* buf, int size) {
    for (int i = 0; i < size * size; i++) buf[i] = 0;          // прозрачно
    for (int dy = 0; dy < CURSOR_H && dy < size; dy++) {
        const char* row = g_sprite[dy];
        for (int dx = 0; dx < CURSOR_W && dx < size; dx++) {
            if (row[dx] == 'X')      buf[dy * size + dx] = 0xFF000000;
            else if (row[dx] == '.') buf[dy * size + dx] = 0xFFFFFFFF;
        }
    }
}
