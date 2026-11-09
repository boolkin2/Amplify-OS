#include "ui_components.h"
#include "ui_layout.h"
#include "ui_motion.h"

static void rect(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}
static void round_rect(const gfx_target_t* t, int x, int y, int w, int h, int r, uint32_t c) {
    gfx_fill_round_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), ui_uw(r), c);
}
static void text(const gfx_target_t* t, int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    (void)bg;
    gfx_draw_string_transparent(t, ui_ux(x), ui_uy(y), s, fg);
}

void ui_draw_card(const gfx_target_t* t, int x, int y, int w, int h, int radius) {
    const ui_runtime_theme_t* th = ui_theme_current();
    gfx_drop_shadow(t, ui_ux(x), ui_uy(y + 4), ui_uw(w), ui_uh(h), ui_uw(5), th->shadow);
    round_rect(t, x, y, w, h, radius, th->border);
    round_rect(t, x + 1, y + 1, w - 2, h - 2, radius, th->surface);
}

void ui_draw_section_label(const gfx_target_t* t, int x, int y, const char* label) {
    text(t, x, y, label, ui_theme_current()->muted, 0);
}

void ui_draw_button(const gfx_target_t* t, int x, int y, int w, int h,
                    const char* label, bool primary, ui_control_state_t state) {
    const ui_runtime_theme_t* th = ui_theme_current();
    uint32_t bg = th->surface_soft, fg = th->ink, border = th->border;
    if (primary) { bg = th->accent_coral; fg = th->on_accent; border = th->accent_berry; }
    if (state == UI_CONTROL_HOVER) bg = primary ? th->accent_orange : th->surface;
    if (state == UI_CONTROL_PRESSED) bg = primary ? th->accent_berry : th->border;
    if (state == UI_CONTROL_DISABLED) { bg = th->surface_soft; fg = th->muted; }
    round_rect(t, x, y, w, h, 10, border);
    round_rect(t, x + 1, y + 1, w - 2, h - 2, 9, bg);
    int tw = gfx_string_width_ui(label);
    gfx_draw_string_transparent(t, ui_ux(x + (w - tw) / 2), ui_uy(y + (h - 16) / 2), label, fg);
}

void ui_draw_metric(const gfx_target_t* t, int x, int y, int w, int h,
                    const char* value, const char* label, uint8_t progress) {
    const ui_runtime_theme_t* th = ui_theme_current();
    ui_draw_card(t, x, y, w, h, 14);
    text(t, x + 18, y + 16, value, th->ink, 0);
    text(t, x + 18, y + 38, label, th->muted, 0);
    rect(t, x + 18, y + h - 18, w - 36, 3, th->border);
    rect(t, x + 18, y + h - 18, ((w - 36) * progress) / 255, 3, th->accent_coral);
}

static void ring_dot(const gfx_target_t* t, int x, int y, int r, uint32_t color) {
    gfx_fill_round_rect(t, x-r, y-r, r*2+1, r*2+1, r, color);
}
void ui_draw_ring(const gfx_target_t* t, int cx, int cy, int radius, int thickness,
                  uint8_t progress, uint32_t track, uint32_t accent) {
    /* Integer circle sampling keeps this freestanding and cheap. */
    static const int8_t unit[64][2] = {
      {0,-64},{6,-64},{12,-63},{19,-61},{24,-59},{30,-56},{36,-53},{41,-49},
      {45,-45},{49,-41},{53,-36},{56,-30},{59,-24},{61,-19},{63,-12},{64,-6},
      {64,0},{64,6},{63,12},{61,19},{59,24},{56,30},{53,36},{49,41},
      {45,45},{41,49},{36,53},{30,56},{24,59},{19,61},{12,63},{6,64},
      {0,64},{-6,64},{-12,63},{-19,61},{-24,59},{-30,56},{-36,53},{-41,49},
      {-45,45},{-49,41},{-53,36},{-56,30},{-59,24},{-61,19},{-63,12},{-64,6},
      {-64,0},{-64,-6},{-63,-12},{-61,-19},{-59,-24},{-56,-30},{-53,-36},{-49,-41},
      {-45,-45},{-41,-49},{-36,-53},{-30,-56},{-24,-59},{-19,-61},{-12,-63},{-6,-64}
    };
    int dots = ((int)progress * 64) / 255;
    int pr = ui_uw(thickness > 1 ? thickness / 2 : 1);
    for (int i=0;i<64;i++) {
        int px=ui_ux(cx + (unit[i][0]*radius)/64);
        int py=ui_uy(cy + (unit[i][1]*radius)/64);
        ring_dot(t,px,py,pr,i<dots?accent:track);
    }
}

// ----------------------------------------------------------------------------
// Этап F: состояние анимации подсветки левого рейла. При включённом
// reduced motion ui_transition_progress() сразу возвращает 255, поэтому
// переезд вырождается в мгновенный перескок без ветвления здесь.
// ----------------------------------------------------------------------------
static ui_transition_t g_rail_slide;
static int g_rail_from = -1;
static int g_rail_to   = -1;

bool ui_components_motion_active(void) {
    return ui_transition_active(&g_rail_slide);
}

void ui_draw_app_shell(const gfx_target_t* t, int active_slot, int hover_slot,
                       const ui_icon_id_t* icons, int icon_count) {
    const ui_runtime_theme_t* th = ui_theme_current();
    rect(t, 0, 0, 64, 1000, th->surface);
    rect(t, 63, 0, 1, 1000, th->border);
    ui_draw_bitmap_scaled(t, ui_ux(18), ui_uy(20), ui_uw(28), ui_uh(28), ui_logo_small());
    // Этап F: активный слот стоит на месте, а подсветка hover переезжает
    // между слотами за 200 мс (ease-out cubic) вместо мгновенного прыжка.
    if (active_slot >= 0 && active_slot < icon_count)
        round_rect(t, 10, 92 + active_slot*66 - 8, 44, 44, 12, th->accent_coral);
    if (hover_slot != g_rail_to) {
        g_rail_from = g_rail_to;
        g_rail_to   = hover_slot;
        ui_transition_start(&g_rail_slide, 200, 0, UI_EASE_OUT_CUBIC);
    }
    if (g_rail_to >= 0 && g_rail_to < icon_count && g_rail_to != active_slot) {
        uint8_t p  = ui_transition_progress(&g_rail_slide);
        int from   = (g_rail_from >= 0 && g_rail_from < icon_count) ? g_rail_from : g_rail_to;
        int y_from = 92 + from * 66 - 8;
        int y_to   = 92 + g_rail_to * 66 - 8;
        round_rect(t, 10, ui_transition_lerp(y_from, y_to, p), 44, 44, 12, th->surface_soft);
    }
    for (int i=0;i<icon_count;i++) {
        int y=92+i*66;
        ui_draw_bitmap_scaled(t,ui_ux(20),ui_uy(y),ui_uw(24),ui_uh(24),ui_icon(icons[i]));
    }
    for (int i=0;i<3;i++) {
        int y=790+i*58;
        round_rect(t,14,y,36,36,10,i==2?th->accent_coral:th->surface_soft);
        const char* labels[3]={"L","S","+"};
        text(t,28,y+10,labels[i],i==2?th->on_accent:th->muted,0);
    }
    rect(t,64,0,1536,48,th->canvas);
    rect(t,64,47,1536,1,th->border);
}
