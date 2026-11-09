// ============================================================================
// ui_font.c — рендерер сглаженного текста (этап F).
// ----------------------------------------------------------------------------
// Работает поверх офлайн-атласов из ui_font_data.c: 4-битное покрытие,
// биринги, advance в 1/4 px, разреженный кернинг. Без аллокаций и без
// растеризации в рантайме — безопасно для раннего boot’а и для IRQ-контекста.
// ============================================================================
#include "ui_font.h"

#define UI_FONT_FIRST_CP 0x20
#define UI_FONT_GLYPHS   224      /* 0x20..0xFF */

static const ui_face_t* face_of(ui_font_id_t id) {
    if ((int)id < 0 || (int)id >= (int)UI_FONT_COUNT) id = UI_FONT_14;
    return &ui_font_faces[id];
}

static const gfx_target_t* resolve(const gfx_target_t* t) {
    return t ? t : gfx_screen();
}

// ----------------------------------------------------------------------------
// Unicode → CP1251 (только то, что реально нужно интерфейсу).
// ----------------------------------------------------------------------------
static int unicode_to_cp1251(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    if (cp >= 0x0410 && cp <= 0x044F) return (int)(cp - 0x0410 + 0xC0);   /* А..я */
    switch (cp) {
        case 0x0401: return 0xA8;   /* Ё */
        case 0x0451: return 0xB8;   /* ё */
        case 0x0404: return 0xAA;
        case 0x0454: return 0xBA;
        case 0x0406: return 0xB2;
        case 0x0456: return 0xB3;
        case 0x0407: return 0xAF;
        case 0x0457: return 0xBF;
        case 0x0490: return 0xA5;
        case 0x0491: return 0xB4;
        case 0x00A0: return 0xA0;
        case 0x00A9: return 0xA9;
        case 0x00AB: return 0xAB;
        case 0x00BB: return 0xBB;
        case 0x00B0: return 0xB0;
        case 0x00B1: return 0xB1;
        case 0x00B5: return 0xB5;
        case 0x00B7: return 0xB7;
        case 0x2013: return 0x96;
        case 0x2014: return 0x97;
        case 0x2018: return 0x91;
        case 0x2019: return 0x92;
        case 0x201C: return 0x93;
        case 0x201D: return 0x94;
        case 0x201E: return 0x84;
        case 0x2022: return 0x95;
        case 0x2026: return 0x85;
        case 0x20AC: return 0x88;
        case 0x2122: return 0x99;
        default: return -1;
    }
}

// Снимает один символ: сначала пробует UTF-8, иначе трактует байт как CP1251.
static int decode_next(const unsigned char** ps) {
    const unsigned char* s = *ps;
    unsigned char b0 = s[0];
    if (b0 < 0x80) { *ps = s + 1; return (int)b0; }
    if ((b0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        int m = unicode_to_cp1251(cp);
        if (m >= 0) { *ps = s + 2; return m; }
    } else if ((b0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        int m = unicode_to_cp1251(cp);
        if (m >= 0) { *ps = s + 3; return m; }
    }
    *ps = s + 1;
    return (int)b0;
}

// ----------------------------------------------------------------------------
// Доступ к покрытию и кернингу.
// ----------------------------------------------------------------------------
static inline uint8_t nib(const unsigned char* p, int stride, int x, int y) {
    unsigned char b = p[(size_t)y * (size_t)stride + (size_t)(x >> 1)];
    return (x & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
}

static int kern_delta(const ui_face_t* f, int a, int b) {
    if (!f->kern || f->kern_count <= 0) return 0;
    int lo = 0, hi = f->kern_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const ui_kern_t* k = &f->kern[mid];
        if ((int)k->first < a || ((int)k->first == a && (int)k->second < b)) lo = mid + 1;
        else if ((int)k->first == a && (int)k->second == b) return (int)k->delta_q;
        else hi = mid - 1;
    }
    return 0;
}

static void blit_glyph(const gfx_target_t* t, const ui_face_t* f, const ui_glyph_t* g,
                       int x, int baseline, uint32_t color, bool bold) {
    if (!g->w || !g->h) return;
    int stride = ((int)g->w + 1) >> 1;
    const unsigned char* src = f->blob + g->offset;
    int gx = x + (int)g->left;
    int gy = baseline - (int)g->top;
    uint32_t rgb = color & 0x00FFFFFFu;
    uint32_t base_a = (color >> 24) & 0xFFu;
    if (base_a == 0) base_a = 255;
    int cols = (int)g->w + (bold ? 1 : 0);
    for (int row = 0; row < (int)g->h; row++) {
        int py = gy + row;
        if (py < 0 || py >= t->height) continue;
        uint32_t* dst = (uint32_t*)((uint8_t*)t->pixels + (size_t)py * (size_t)t->pitch);
        for (int col = 0; col < cols; col++) {
            int px = gx + col;
            if (px < 0 || px >= t->width) continue;
            uint8_t cov = (col < (int)g->w) ? nib(src, stride, col, row) : 0;
            if (bold && col > 0) {
                uint8_t prev = nib(src, stride, col - 1, row);
                if (prev > cov) cov = prev;
            }
            if (!cov) continue;
            uint32_t alpha = (uint32_t)cov * 17u;
            if (alpha > 255u) alpha = 255u;
            alpha = alpha * base_a / 255u;
            if (!alpha) continue;
            dst[px] = gfx_blend(dst[px], (alpha << 24) | rgb);
        }
    }
}

// Единый проход: измеряет (draw=false) либо рисует строку.
static int run_text(const gfx_target_t* t, int x, int baseline, const char* s,
                    uint32_t color, ui_font_id_t id, int tracking, bool bold, bool draw) {
    const ui_face_t* f = face_of(id);
    if (!s || !f->glyphs) return 0;
    const unsigned char* p = (const unsigned char*)s;
    int pen_q = 0, widest_q = 0, prev = -1;
    int line = f->line_h ? (int)f->line_h : (int)f->px;
    while (*p) {
        int code = decode_next(&p);
        if (code == '\n') {
            if (pen_q > widest_q) widest_q = pen_q;
            pen_q = 0; prev = -1; baseline += line;
            continue;
        }
        if (code == '\t') code = ' ';
        if (code < UI_FONT_FIRST_CP || code > 0xFF) code = '?';
        const ui_glyph_t* g = &f->glyphs[code - UI_FONT_FIRST_CP];
        if (prev >= 0 && !f->mono_adv) pen_q += kern_delta(f, prev, code);
        if (draw && t) blit_glyph(t, f, g, x + (pen_q >> 2), baseline, color, bold);
        pen_q += (int)g->advance_q;
        if (tracking) pen_q += tracking * 4;
        prev = code;
    }
    if (tracking && pen_q >= tracking * 4) pen_q -= tracking * 4;   /* без хвостового трекинга */
    if (pen_q > widest_q) widest_q = pen_q;
    return (widest_q + 3) >> 2;
}

// ----------------------------------------------------------------------------
// Публичный API.
// ----------------------------------------------------------------------------
int ui_font_line_height(ui_font_id_t id) { return (int)face_of(id)->line_h; }
int ui_font_ascent(ui_font_id_t id)      { return (int)face_of(id)->ascent; }
int ui_font_cap_height(ui_font_id_t id)  { return ((int)face_of(id)->px * 72) / 100; }

int ui_font_text_width(const char* s, ui_font_id_t id) {
    return run_text(0, 0, 0, s, 0, id, 0, false, false);
}

int ui_font_text_width_tracked(const char* s, ui_font_id_t id, int tracking) {
    return run_text(0, 0, 0, s, 0, id, tracking, false, false);
}

int ui_font_baseline_for_box(ui_font_id_t id, int y_top, int box_h) {
    const ui_face_t* f = face_of(id);
    return y_top + (int)f->ascent + (box_h - (int)f->line_h) / 2;
}

ui_font_id_t ui_font_for_scale(int scale) {
    if (scale <= 1) return UI_FONT_14;
    if (scale == 2) return UI_FONT_28;
    return UI_FONT_44;
}

int ui_font_draw_baseline(const gfx_target_t* t, int x, int baseline, const char* s,
                          uint32_t color, ui_font_id_t id) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return 0;
    return run_text(tg, x, baseline, s, color, id, 0, false, true);
}

int ui_font_draw_baseline_ex(const gfx_target_t* t, int x, int baseline, const char* s,
                             uint32_t color, ui_font_id_t id, int tracking, bool bold) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return 0;
    return run_text(tg, x, baseline, s, color, id, tracking, bold, true);
}

int ui_font_draw(const gfx_target_t* t, int x, int y_top, const char* s,
                 uint32_t color, ui_font_id_t id) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return 0;
    const ui_face_t* f = face_of(id);
    return run_text(tg, x, y_top + (int)f->ascent, s, color, id, 0, false, true);
}

int ui_font_draw_tracked(const gfx_target_t* t, int x, int y_top, const char* s,
                         uint32_t color, ui_font_id_t id, int tracking) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return 0;
    const ui_face_t* f = face_of(id);
    return run_text(tg, x, y_top + (int)f->ascent, s, color, id, tracking, false, true);
}

int ui_font_draw_bold(const gfx_target_t* t, int x, int y_top, const char* s,
                      uint32_t color, ui_font_id_t id) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return 0;
    const ui_face_t* f = face_of(id);
    return run_text(tg, x, y_top + (int)f->ascent, s, color, id, 0, true, true);
}

void ui_font_draw_cell(const gfx_target_t* t, int x, int y, char c,
                       uint32_t fg, uint32_t bg, ui_font_id_t id) {
    const gfx_target_t* tg = resolve(t);
    if (!tg) return;
    const ui_face_t* f = face_of(id);
    int cw = f->mono_adv ? (int)f->mono_adv : AOS64_FONT_W;
    int chh = AOS64_FONT_H;
    if (((bg >> 24) & 0xFFu) != 0) gfx_fill_rect(tg, x, y, cw, chh, bg);
    int code = (int)(unsigned char)c;
    if (code < UI_FONT_FIRST_CP || code > 0xFF) return;
    int baseline = ui_font_baseline_for_box(id, y, chh);
    blit_glyph(tg, f, &f->glyphs[code - UI_FONT_FIRST_CP], x, baseline, fg, false);
}
