// ============================================================================
// window.c — реализация window_t.
// ============================================================================
#include "window.h"
#include "graphics.h"
#include "heap.h"
#include "serial.h"

#include <stdint.h>
#include <stdbool.h>

static int g_next_id = 1;

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------
static void copy_str(char* dst, const char* src, int cap) {
    int i;
    for (i = 0; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ----------------------------------------------------------------------------
window_t* window_create(const char* title, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;

    window_t* win = (window_t*)kmalloc(sizeof(window_t));
    if (!win) return 0;

    // Аллоцируем content-buffer (width × height пикселей ARGB).
    size_t bytes = (size_t)w * (size_t)h * 4;
    uint32_t* buf = (uint32_t*)kmalloc(bytes);
    if (!buf) { kfree(win); return 0; }

    // Стираем
    for (size_t i = 0; i < bytes / 4; i++) buf[i] = 0xFF1A2438;     // тёмно-синий

    win->id              = g_next_id++;
    copy_str(win->title, title ? title : "(window)", WINDOW_MAX_TITLE);
    win->x               = x;
    win->y               = y;
    win->width           = w;
    win->height          = h;
    win->buffer          = buf;
    win->target.pixels   = buf;
    win->target.width    = w;
    win->target.height   = h;
    win->target.pitch    = w * 4;
    win->visible         = true;
    win->focused         = false;
    win->dragging        = false;
    win->close_requested = false;
    win->drag_offset_x   = 0;
    win->drag_offset_y   = 0;
    win->opacity         = 255;
    win->chrome          = WINDOW_CHROME_CLASSIC;
    win->wants_continuous_redraw = false;
    win->bg_color        = 0xFF1A2438;
    win->accent_color    = 0xFF38598C;
    win->on_draw         = 0;
    win->on_scroll       = 0;
    win->rendered        = false;
    win->on_click        = 0;
    win->on_key          = 0;
    win->user_data       = 0;
    win->owns_user_data  = false;
    win->on_destroy      = 0;
    win->prev            = 0;
    win->next            = 0;

    return win;
}

void window_destroy(window_t* w) {
    if (!w) return;
    // Этап F: полный теардаун — состояние приложения, потом пиксели.
    if (w->on_destroy) w->on_destroy(w);
    if (w->owns_user_data && w->user_data) {
        kfree(w->user_data);
        w->user_data = 0;
    }
    if (w->buffer) kfree(w->buffer);
    kfree(w);
}

void window_fill(window_t* w, uint32_t color) {
    if (!w || !w->buffer) return;
    size_t total = (size_t)w->width * (size_t)w->height;
    for (size_t i = 0; i < total; i++) w->buffer[i] = color;
}

void window_put_pixel(window_t* w, int lx, int ly, uint32_t color) {
    if (!w || !w->buffer) return;
    if ((unsigned)lx >= (unsigned)w->width || (unsigned)ly >= (unsigned)w->height) return;
    w->buffer[(size_t)ly * w->width + lx] = color;
}

// ----------------------------------------------------------------------------
// window_render — отрисовка в target (обычно doublebuf).
// Структура:
//   1. Drop shadow под окном.
//   2. Title bar (accent_color) + текст title + кнопка закрытия.
//   3. Content area: если on_draw задан — вызываем (рисует в win->target);
//      потом blit win->buffer в tgt.
// ----------------------------------------------------------------------------
bool window_hit_close(const window_t* w, int mx, int my) {
    if (!w || w->chrome != WINDOW_CHROME_CLASSIC) return false;
    int bx = w->x + w->width - WINDOW_CLOSE_BTN - 8;
    int by = w->y + (WINDOW_TITLE_HEIGHT - WINDOW_CLOSE_BTN) / 2;
    return mx >= bx && mx < bx + WINDOW_CLOSE_BTN &&
           my >= by && my < by + WINDOW_CLOSE_BTN;
}

bool window_hit_titlebar(const window_t* w, int mx, int my) {
    if (!w || w->chrome != WINDOW_CHROME_CLASSIC) return false;
    return mx >= w->x && mx < w->x + w->width &&
           my >= w->y && my < w->y + WINDOW_TITLE_HEIGHT;
}

bool window_hit_content(const window_t* w, int mx, int my) {
    if (!w) return false;
    int title_h = window_title_height(w);
    // height — это размер content area, общая высота окна = title + height.
    return mx >= w->x && mx < w->x + w->width &&
           my >= w->y + title_h &&
           my <  w->y + title_h + w->height;
}

int window_title_height(const window_t* w) {
    return (!w || w->chrome == WINDOW_CHROME_CLASSIC) ? WINDOW_TITLE_HEIGHT : 0;
}

void window_set_chrome(window_t* w, window_chrome_t chrome) {
    if (w) w->chrome = chrome;
}

bool window_resize(window_t* w, int width, int height) {
    if (!w || width <= 0 || height <= 0) return false;
    if ((size_t)width > ((size_t)-1) / 4 / (size_t)height) return false;
    size_t pixels = (size_t)width * (size_t)height;
    uint32_t* next = (uint32_t*)kmalloc(pixels * 4);
    if (!next) return false;
    for (size_t i = 0; i < pixels; i++) next[i] = w->bg_color;
    if (w->buffer) kfree(w->buffer);
    w->buffer = next;
    w->width = width;
    w->height = height;
    w->target.pixels = next;
    w->target.width = width;
    w->target.height = height;
    w->target.pitch = width * 4;
    return true;
}

bool window_enter_workspace(window_t* w, int screen_w, int screen_h,
                            int inset_left, int inset_top) {
    if (!w || screen_w <= inset_left || screen_h <= inset_top) return false;
    if (!window_resize(w, screen_w - inset_left, screen_h - inset_top)) return false;
    w->x = inset_left;
    w->y = inset_top;
    w->chrome = WINDOW_CHROME_WORKSPACE;
    return true;
}

void window_render(window_t* w, const gfx_target_t* tgt) {
    if (!w || !w->visible || !tgt) return;

    // height = content; полная высота на экране = title + content.
    int title_h = window_title_height(w);
    int total_h = title_h + w->height;

    if (w->chrome != WINDOW_CHROME_CLASSIC) {
        if (w->on_draw) w->on_draw(w, &w->target);
        gfx_blit(tgt, w->x, w->y, &w->target, 0, 0, w->width, w->height);
        return;
    }

    // 1) Тень.
    gfx_drop_shadow(tgt, w->x, w->y, w->width, total_h, 4, GFX_ARGB(120, 0, 0, 0));

    // 2) Title bar.
    uint32_t title_bg = w->focused ? w->accent_color : 0xFF24345A;
    gfx_fill_rect(tgt, w->x, w->y, w->width, WINDOW_TITLE_HEIGHT, title_bg);

    // 3) Title text.
    gfx_draw_string(tgt, w->x + 10, w->y + 6, w->title, 0xFFFFFFFF, title_bg);

    // 4) Кнопка закрытия (красный квадрат с крестом).
    int bx = w->x + w->width - WINDOW_CLOSE_BTN - 8;
    int by = w->y + (WINDOW_TITLE_HEIGHT - WINDOW_CLOSE_BTN) / 2;
    gfx_fill_rect(tgt, bx, by, WINDOW_CLOSE_BTN, WINDOW_CLOSE_BTN, 0xFFFF5050);
    gfx_stroke_rect(tgt, bx, by, WINDOW_CLOSE_BTN, WINDOW_CLOSE_BTN, 0xFF800000);
    for (int i = 3; i < WINDOW_CLOSE_BTN - 3; i++) {
        gfx_put_pixel(tgt, bx + i,                          by + i, 0xFFFFFFFF);
        gfx_put_pixel(tgt, bx + WINDOW_CLOSE_BTN - 1 - i,   by + i, 0xFFFFFFFF);
    }

    // 5) Application контент: callback пишет в win->target.
    if (w->on_draw) w->on_draw(w, &w->target);

    // 6) Blit content под title bar.
    gfx_blit(tgt, w->x, w->y + WINDOW_TITLE_HEIGHT,
             &w->target, 0, 0, w->width, w->height);

    // 7) Border вокруг всего окна.
    gfx_stroke_rect(tgt, w->x, w->y, w->width, total_h, 0xFF60D0F0);
}
