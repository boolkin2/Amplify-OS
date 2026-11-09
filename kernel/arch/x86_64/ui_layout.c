#include "ui_layout.h"
#include <stdint.h>

static ui_layout_t g_layout = { UI_LOGICAL_W, UI_LOGICAL_H, 1 << 16, 0, 0 };

void ui_layout_update(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return;
    int64_t sx = ((int64_t)screen_w << 16) / UI_LOGICAL_W;
    int64_t sy = ((int64_t)screen_h << 16) / UI_LOGICAL_H;
    int64_t scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    g_layout.screen_w = screen_w;
    g_layout.screen_h = screen_h;
    g_layout.scale_fp = (int)scale;
    int used_w = (int)(((int64_t)UI_LOGICAL_W * scale) >> 16);
    int used_h = (int)(((int64_t)UI_LOGICAL_H * scale) >> 16);
    g_layout.offset_x = (screen_w - used_w) / 2;
    g_layout.offset_y = (screen_h - used_h) / 2;
}

const ui_layout_t* ui_layout_get(void) { return &g_layout; }

static int scale_value(int value) {
    return (int)(((int64_t)value * g_layout.scale_fp + 0x8000) >> 16);
}

int ui_ux(int x) { return g_layout.offset_x + scale_value(x); }
int ui_uy(int y) { return g_layout.offset_y + scale_value(y); }
int ui_uw(int w) { int v = scale_value(w); return w > 0 && v < 1 ? 1 : v; }
int ui_uh(int h) { int v = scale_value(h); return h > 0 && v < 1 ? 1 : v; }

static int unscale_value(int value) {
    if (g_layout.scale_fp <= 0) return value;
    int64_t numerator = (int64_t)value << 16;
    /* Nearest logical pixel prevents a forward-mapped hitbox edge from
       falling one pixel outside because of fixed-point truncation. */
    if (numerator >= 0) numerator += g_layout.scale_fp / 2;
    else numerator -= g_layout.scale_fp / 2;
    return (int)(numerator / g_layout.scale_fp);
}
int ui_lx(int x) { return unscale_value(x - g_layout.offset_x); }
int ui_ly(int y) { return unscale_value(y - g_layout.offset_y); }

bool ui_hit_logical(int sx, int sy, int x, int y, int w, int h) {
    int lx = ui_lx(sx), ly = ui_ly(sy);
    return lx >= x && lx < x + w && ly >= y && ly < y + h;
}
