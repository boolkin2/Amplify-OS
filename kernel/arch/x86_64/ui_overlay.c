// ============================================================================
// ui_overlay.c — шторка, модальное окно и всплывающее сообщение (toast).
// ----------------------------------------------------------------------------
// [48.2] Появление и исчезновение — через fx_panel (ui_fx.h): панель снимается
// один раз и анимируется картинкой. Шторка выезжает справа, модальное окно и
// toast поднимаются снизу; при закрытии — обратным ходом (раньше оверлей
// пропадал мгновенно). Затемнение — fx_dim вместо попиксельного
// compositor_fill_round_rect_aa на весь экран (на 1080p — десятки миллисекунд).
// ============================================================================
#include "ui_overlay.h"
#include "ui_motion.h"
#include "ui_theme.h"
#include "ui_fx.h"
#include "compositor.h"
#include "timer.h"

static struct {
    ui_overlay_kind_t kind;          // что показано (сохраняется, пока панель исчезает)
    char title[48];
    char body[192];
    uint64_t hide_at;
    fx_panel_t fx;
} g_overlay = { .fx = { .name = "overlay" } };

static void copy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

void ui_overlay_init(void) {
    g_overlay.kind = UI_OVERLAY_NONE;
    g_overlay.hide_at = 0;
    g_overlay.fx.shown = false;
    g_overlay.fx.closing = false;
}

static void show(ui_overlay_kind_t k, const char* t, const char* b) {
    g_overlay.kind = k;
    copy(g_overlay.title, t, sizeof(g_overlay.title));
    copy(g_overlay.body, b, sizeof(g_overlay.body));
    g_overlay.hide_at = 0;
    fx_panel_show(&g_overlay.fx, k == UI_OVERLAY_TOAST ? 260 : 300);
}

void ui_overlay_show_drawer(const char* t, const char* b) { show(UI_OVERLAY_DRAWER, t, b); }
void ui_overlay_show_modal(const char* t, const char* b)  { show(UI_OVERLAY_MODAL, t, b); }
void ui_overlay_show_toast(const char* t, unsigned ms) {
    show(UI_OVERLAY_TOAST, "", t);
    g_overlay.hide_at = timer_get_uptime_ms() + (ms ? ms : 2400);
}

void ui_overlay_dismiss(void) {
    g_overlay.hide_at = 0;
    fx_panel_hide(&g_overlay.fx, 200);
}

bool ui_overlay_is_open(void) { return g_overlay.fx.shown; }
bool ui_overlay_visible(void) { return fx_panel_visible(&g_overlay.fx); }

bool ui_overlay_motion_active(void) {
    // Срок toast'а проверяем здесь, а не в отрисовке: цикл WM спрашивает об
    // этом на каждом проходе, и сообщение уходит вовремя, даже если кадров
    // сцены сейчас нет.
    if (g_overlay.fx.shown && g_overlay.hide_at && timer_get_uptime_ms() >= g_overlay.hide_at)
        ui_overlay_dismiss();
    return fx_panel_moving(&g_overlay.fx);
}

static void geometry(const gfx_target_t* t, int* x, int* y, int* w, int* h) {
    if (g_overlay.kind == UI_OVERLAY_TOAST) {
        *w = 360; *h = 56; *x = (t->width - *w) / 2; *y = t->height - 90;
    } else if (g_overlay.kind == UI_OVERLAY_DRAWER) {
        *w = 420; *h = t->height - 32; *x = t->width - *w - 16; *y = 16;
    } else {
        *w = 520; *h = 260; *x = (t->width - *w) / 2; *y = (t->height - *h) / 2;
    }
}

static void overlay_panel(const gfx_target_t* t, void* arg) {
    (void)arg;
    const ui_runtime_theme_t* th = ui_theme_current();
    int x, y, w, h;
    geometry(t, &x, &y, &w, &h);
    if (g_overlay.kind == UI_OVERLAY_TOAST) {
        gfx_fill_round_rect(t, x, y, w, h, 14, th->border);
        gfx_fill_round_rect(t, x + 1, y + 1, w - 2, h - 2, 13, th->surface);
        gfx_draw_string_transparent(t, x + 20, y + 20, g_overlay.body, th->ink);
        return;
    }
    gfx_fill_round_rect(t, x, y, w, h, 18, th->border);
    gfx_fill_round_rect(t, x + 1, y + 1, w - 2, h - 2, 17, th->surface);
    gfx_draw_string_scaled(t, x + 24, y + 24, g_overlay.title, th->ink, 2);
    gfx_draw_string_transparent(t, x + 24, y + 72, g_overlay.body, th->muted);
    gfx_fill_round_rect(t, x + w - 52, y + 18, 34, 34, 10, th->surface_soft);
    gfx_draw_string_transparent(t, x + w - 39, y + 27, "X", th->ink);
}

void ui_overlay_draw(const gfx_target_t* t) {
    if (!t || g_overlay.kind == UI_OVERLAY_NONE || !fx_panel_visible(&g_overlay.fx)) return;
    int x, y, w, h;
    geometry(t, &x, &y, &w, &h);
    bool toast = g_overlay.kind == UI_OVERLAY_TOAST;
    bool drawer = g_overlay.kind == UI_OVERLAY_DRAWER;
    fx_panel_frame(&g_overlay.fx, t, 0x1E141F, toast ? 0 : 110,
                   x, y, w, h, toast ? 14 : 18,
                   drawer ? w / 2 : 0, drawer ? 0 : 40,
                   overlay_panel, 0);
}

bool ui_overlay_handle_click(int x, int y) {
    (void)x; (void)y;
    if (!ui_overlay_is_open()) return false;
    ui_overlay_dismiss();
    return true;
}

bool ui_overlay_handle_key(char c) {
    if (!ui_overlay_is_open()) return false;
    if (c == 27) { ui_overlay_dismiss(); return true; }
    return g_overlay.kind != UI_OVERLAY_TOAST;
}
