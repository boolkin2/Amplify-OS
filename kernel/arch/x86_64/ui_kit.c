// ============================================================================
// ui_kit.c — элементы управления Pearl в координатах окна (см. ui_kit.h).
// ============================================================================
#include "ui_kit.h"
#include "mouse.h"
#include "timer.h"

int pk_text(const gfx_target_t* t, int x, int y, const char* s, uint32_t color, ui_font_id_t f) {
    return ui_font_draw_baseline_ex(t, x, y + ui_font_ascent(f), s, color, f, 0, false);
}

int pk_text_bold(const gfx_target_t* t, int x, int y, const char* s, uint32_t color, ui_font_id_t f) {
    return ui_font_draw_baseline_ex(t, x, y + ui_font_ascent(f), s, color, f, 0, true);
}

void pk_text_clip(const gfx_target_t* t, int x, int y, int max_w, const char* s,
                  uint32_t color, ui_font_id_t f, bool bold) {
    if (max_w <= 0 || !s) return;
    if (ui_font_text_width(s, f) <= max_w) {
        ui_font_draw_baseline_ex(t, x, y + ui_font_ascent(f), s, color, f, 0, bold);
        return;
    }
    // Не влезает: режем по символам и ставим «...». Строки интерфейса короткие,
    // поэтому простой перебор длины дешевле любого бинарного поиска.
    static char buf[256];
    int n = 0;
    while (s[n] && n < (int)sizeof(buf) - 4) n++;
    while (n > 0) {
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = '.'; buf[n+1] = '.'; buf[n+2] = '.'; buf[n+3] = 0;
        if (ui_font_text_width(buf, f) <= max_w) break;
        n--;
    }
    if (n > 0) ui_font_draw_baseline_ex(t, x, y + ui_font_ascent(f), buf, color, f, 0, bold);
}

void pk_label(const gfx_target_t* t, int x, int y, const char* s) {
    ui_font_draw_baseline_ex(t, x, y + ui_font_ascent(UI_FONT_12), s,
                             ui_theme_current()->muted, UI_FONT_12, 1, false);
}

void pk_card(const gfx_target_t* t, pk_rect_t r) {
    const ui_runtime_theme_t* th = ui_theme_current();
    gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 14, th->border);
    gfx_fill_round_rect(t, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 13, th->surface);
}

void pk_button(const gfx_target_t* t, pk_rect_t r, const char* label, bool primary, pk_state_t st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    uint32_t bg = th->surface_soft, fg = th->ink, border = th->border;
    if (primary) { bg = th->accent_coral; fg = th->on_accent; border = th->accent_berry; }
    if (st == PK_HOVER)    bg = primary ? th->accent_orange : th->surface;
    if (st == PK_ACTIVE)   bg = primary ? th->accent_berry  : th->border;
    if (st == PK_DISABLED) { bg = th->surface_soft; fg = th->muted; border = th->border; }
    gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, border);
    gfx_fill_round_rect(t, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, bg);
    ui_font_id_t f = UI_FONT_14;
    int tw = ui_font_text_width(label, f);
    int ty = r.y + (r.h - ui_font_line_height(f)) / 2;
    ui_font_draw_baseline_ex(t, r.x + (r.w - tw) / 2, ty + ui_font_ascent(f), label, fg, f, 0, primary);
}

void pk_field(const gfx_target_t* t, pk_rect_t r, const char* text, const char* placeholder,
              bool focused, bool error, bool disabled, ui_font_id_t f) {
    const ui_runtime_theme_t* th = ui_theme_current();
    uint32_t border = error ? th->accent_berry : focused ? th->accent_coral : th->border;
    uint32_t bg = disabled ? th->surface_soft : th->canvas;
    gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 9, border);
    // Рамка в фокусе и при ошибке — толще: её должно быть видно сразу.
    int bw = (focused || error) ? 2 : 1;
    gfx_fill_round_rect(t, r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw, 9 - bw, bg);
    int ty = r.y + (r.h - ui_font_line_height(f)) / 2;
    int tx = r.x + 12;
    int tw = 0;
    if (text && text[0]) {
        // Длинный текст (адрес страницы) показываем хвостом: виден конец, где
        // идёт ввод.
        const char* s = text;
        int max_w = r.w - 28;
        while (*s && ui_font_text_width(s, f) > max_w) s++;
        tw = ui_font_draw_baseline_ex(t, tx, ty + ui_font_ascent(f), s,
                                      disabled ? th->muted : th->ink, f, 0, false);
        tw = ui_font_text_width(s, f);
    } else if (placeholder && !focused) {
        ui_font_draw_baseline_ex(t, tx, ty + ui_font_ascent(f), placeholder, th->muted, f, 0, false);
    }
    if (focused && !disabled && ((timer_get_uptime_ms() / 500) & 1) == 0)
        gfx_fill_rect(t, tx + tw + 1, r.y + 8, 2, r.h - 16, th->accent_coral);
}

void pk_segment(const gfx_target_t* t, pk_rect_t r, const char* label, bool selected, pk_state_t st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    uint32_t bg = selected ? th->accent_coral : (st == PK_HOVER ? th->surface : th->surface_soft);
    uint32_t fg = selected ? th->on_accent : th->ink;
    gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, selected ? th->accent_berry : th->border);
    gfx_fill_round_rect(t, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, bg);
    ui_font_id_t f = UI_FONT_14;
    int tw = ui_font_text_width(label, f);
    int ty = r.y + (r.h - ui_font_line_height(f)) / 2;
    ui_font_draw_baseline_ex(t, r.x + (r.w - tw) / 2, ty + ui_font_ascent(f), label, fg, f, 0, selected);
}

void pk_chip(const gfx_target_t* t, int x, int y, const char* label, uint32_t dot) {
    const ui_runtime_theme_t* th = ui_theme_current();
    ui_font_id_t f = UI_FONT_12;
    int tw = ui_font_text_width(label, f);
    int w = tw + 36, h = 26;
    gfx_fill_round_rect(t, x, y, w, h, 13, th->border);
    gfx_fill_round_rect(t, x + 1, y + 1, w - 2, h - 2, 12, th->surface_soft);
    gfx_fill_round_rect(t, x + 10, y + 9, 8, 8, 4, dot);
    ui_font_draw_baseline_ex(t, x + 24, y + (h - ui_font_line_height(f)) / 2 + ui_font_ascent(f),
                             label, th->ink, f, 0, true);
}

void pk_pointer(const window_t* w, int* lx, int* ly, bool* left_down) {
    const mouse_state_t* m = mouse_get_state();
    if (lx) *lx = m->x - w->x;
    if (ly) *ly = m->y - w->y - window_title_height(w);
    if (left_down) *left_down = (m->buttons & MOUSE_BTN_LEFT) != 0;
}

pk_state_t pk_pointer_state(const window_t* w, pk_rect_t r, bool disabled) {
    if (disabled) return PK_DISABLED;
    int lx, ly; bool down;
    pk_pointer(w, &lx, &ly, &down);
    // Подсветка — только у активного окна: под открытой панелью MIND кнопки
    // окна за ней «оживать» не должны.
    if (!w->focused || !pk_hit(r, lx, ly)) return PK_IDLE;
    return down ? PK_ACTIVE : PK_HOVER;
}
