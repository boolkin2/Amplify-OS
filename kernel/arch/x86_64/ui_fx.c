// ============================================================================
// ui_fx.c — быстрые пиксельные операции и снимки для анимаций (см. ui_fx.h).
// ============================================================================
#include "ui_fx.h"
#include "ui_theme.h"
#include "framestat.h"
#include "heap.h"
#include "timer.h"
#include "serial.h"
#include "wm.h"

#include <emmintrin.h>

// ---------------------------------------------------------------------------
// Смешивание строки: dst = (src*a + dst*(256-a)) >> 8, a = 0..256.
// keyed — пиксели src с альфой 0 оставляют dst как есть.
// ---------------------------------------------------------------------------
static void blend_row(uint32_t* dst, const uint32_t* src, int n, int a, bool keyed) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i va   = _mm_set1_epi16((short)a);
    const __m128i vi   = _mm_set1_epi16((short)(256 - a));
    const __m128i opq  = _mm_set1_epi32((int)0xFF000000u);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i s = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i d = _mm_loadu_si128((const __m128i*)(dst + i));
        __m128i s_lo = _mm_unpacklo_epi8(s, zero), s_hi = _mm_unpackhi_epi8(s, zero);
        __m128i d_lo = _mm_unpacklo_epi8(d, zero), d_hi = _mm_unpackhi_epi8(d, zero);
        __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(s_lo, va), _mm_mullo_epi16(d_lo, vi)), 8);
        __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(s_hi, va), _mm_mullo_epi16(d_hi, vi)), 8);
        __m128i out = _mm_or_si128(_mm_packus_epi16(lo, hi), opq);
        if (keyed) {
            // Маска «альфа src == 0» на весь пиксель.
            __m128i m = _mm_cmpeq_epi32(_mm_srli_epi32(s, 24), zero);
            out = _mm_or_si128(_mm_andnot_si128(m, out), _mm_and_si128(m, d));
        }
        _mm_storeu_si128((__m128i*)(dst + i), out);
    }
    for (; i < n; i++) {
        uint32_t s = src[i], d = dst[i];
        if (keyed && (s >> 24) == 0) continue;
        uint32_t rb = (((s & 0x00FF00FFu) * (uint32_t)a + (d & 0x00FF00FFu) * (uint32_t)(256 - a)) >> 8) & 0x00FF00FFu;
        uint32_t g  = (((s & 0x0000FF00u) * (uint32_t)a + (d & 0x0000FF00u) * (uint32_t)(256 - a)) >> 8) & 0x0000FF00u;
        dst[i] = 0xFF000000u | rb | g;
    }
}

// Копия строки без смешивания (opacity == 255).
static void copy_row(uint32_t* dst, const uint32_t* src, int n, bool keyed) {
    if (!keyed) { for (int i = 0; i < n; i++) dst[i] = src[i]; return; }
    for (int i = 0; i < n; i++) if (src[i] >> 24) dst[i] = src[i];
}

static inline int op_to_256(uint8_t opacity) { return ((int)opacity * 256 + 127) / 255; }

void fx_dim(const gfx_target_t* t, int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
    if (!t || !t->pixels || alpha == 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->width)  w = t->width  - x;
    if (y + h > t->height) h = t->height - y;
    if (w <= 0 || h <= 0) return;
    int a = op_to_256(alpha);
    // Слагаемое цвета постоянно — считаем один раз: c*a по каналам в 16 бит.
    uint32_t cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    const __m128i zero = _mm_setzero_si128();
    const __m128i vc = _mm_set_epi16(0, (short)(cr * a), (short)(cg * a), (short)(cb * a),
                                     0, (short)(cr * a), (short)(cg * a), (short)(cb * a));
    const __m128i vi  = _mm_set1_epi16((short)(256 - a));
    const __m128i opq = _mm_set1_epi32((int)0xFF000000u);
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = (uint32_t*)((uint8_t*)t->pixels + (size_t)yy * t->pitch) + x;
        int i = 0;
        for (; i + 4 <= w; i += 4) {
            __m128i d = _mm_loadu_si128((const __m128i*)(row + i));
            __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), vi), vc), 8);
            __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), vi), vc), 8);
            _mm_storeu_si128((__m128i*)(row + i), _mm_or_si128(_mm_packus_epi16(lo, hi), opq));
        }
        for (; i < w; i++) {
            uint32_t d = row[i];
            uint32_t r = (((d >> 16) & 0xFF) * (uint32_t)(256 - a) + cr * (uint32_t)a) >> 8;
            uint32_t g = (((d >>  8) & 0xFF) * (uint32_t)(256 - a) + cg * (uint32_t)a) >> 8;
            uint32_t b = (( d        & 0xFF) * (uint32_t)(256 - a) + cb * (uint32_t)a) >> 8;
            row[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void fx_blit(const gfx_target_t* dst, int dx, int dy,
             const uint32_t* src, int src_stride, int w, int h,
             uint8_t opacity, bool keyed) {
    if (!dst || !dst->pixels || !src || opacity == 0) return;
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > dst->width)  w = dst->width  - dx;
    if (dy + h > dst->height) h = dst->height - dy;
    if (w <= 0 || h <= 0) return;
    int a = op_to_256(opacity);
    for (int yy = 0; yy < h; yy++) {
        uint32_t* drow = (uint32_t*)((uint8_t*)dst->pixels + (size_t)(dy + yy) * dst->pitch) + dx;
        const uint32_t* srow = src + (size_t)(sy + yy) * src_stride + sx;
        if (opacity == 255) copy_row(drow, srow, w, keyed);
        else                blend_row(drow, srow, w, a, keyed);
    }
}

#define FX_MAX_W 4096
void fx_blit_scaled(const gfx_target_t* dst, int dx, int dy, int dw, int dh,
                    const uint32_t* src, int src_stride, int sw, int sh,
                    uint8_t opacity, bool keyed) {
    if (!dst || !dst->pixels || !src || opacity == 0) return;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (dw == sw && dh == sh) { fx_blit(dst, dx, dy, src, src_stride, sw, sh, opacity, keyed); return; }
    // Видимая часть по X.
    int x0 = dx < 0 ? -dx : 0;
    int x1 = dx + dw > dst->width ? dst->width - dx : dw;
    if (x1 - x0 <= 0 || x1 - x0 > FX_MAX_W) return;
    // Таблица «пиксель назначения -> пиксель источника» одна на кадр, строка
    // источника собирается во временный ряд и смешивается тем же SSE-циклом.
    static uint16_t xmap[FX_MAX_W];
    static uint32_t line[FX_MAX_W];
    int n = x1 - x0;
    for (int i = 0; i < n; i++) {
        int s = (int)(((int64_t)(x0 + i) * sw) / dw);
        xmap[i] = (uint16_t)(s >= sw ? sw - 1 : s);
    }
    int a = op_to_256(opacity);
    for (int yy = 0; yy < dh; yy++) {
        int ty = dy + yy;
        if (ty < 0 || ty >= dst->height) continue;
        int s = (int)(((int64_t)yy * sh) / dh);
        if (s >= sh) s = sh - 1;
        const uint32_t* srow = src + (size_t)s * src_stride;
        for (int i = 0; i < n; i++) line[i] = srow[xmap[i]];
        uint32_t* drow = (uint32_t*)((uint8_t*)dst->pixels + (size_t)ty * dst->pitch) + dx + x0;
        if (opacity == 255) copy_row(drow, line, n, keyed);
        else                blend_row(drow, line, n, a, keyed);
    }
}

// ---------------------------------------------------------------------------
// Запасной буфер и снимки.
// ---------------------------------------------------------------------------
static gfx_target_t g_scratch = {0, 0, 0, 0};
static const void*  g_owner = 0;
static bool         g_no_mem = false;

bool fx_capture_begin(const void* owner, const gfx_target_t* screen, gfx_target_t* out) {
    if (!screen || g_no_mem) return false;
    if (g_owner && g_owner != owner) return false;
    if (g_scratch.pixels && (g_scratch.width != screen->width || g_scratch.height != screen->height)) {
        kfree(g_scratch.pixels);                    // режим экрана сменили
        g_scratch.pixels = 0;
    }
    if (!g_scratch.pixels) {
        g_scratch.pixels = (uint32_t*)kmalloc((size_t)screen->width * (size_t)screen->height * 4);
        if (!g_scratch.pixels) {
            g_no_mem = true;
            serial_puts("[fx] scratch buffer: kmalloc failed, animations draw directly\n");
            return false;
        }
        g_scratch.width  = screen->width;
        g_scratch.height = screen->height;
        g_scratch.pitch  = screen->width * 4;
    }
    g_owner = owner;
    *out = g_scratch;
    return true;
}

void fx_capture_end(const void* owner, fx_sprite_t* sp, int x, int y, int w, int h, int radius) {
    if (g_owner != owner || !sp) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_scratch.width)  w = g_scratch.width  - x;
    if (y + h > g_scratch.height) h = g_scratch.height - y;
    if (w <= 0 || h <= 0) { sp->valid = false; return; }
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    // Всё внутри панели — непрозрачное; за скруглёнными углами — альфа 0
    // (тот же циркульный тест, что в gfx_fill_round_rect).
    int stride = g_scratch.width;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = g_scratch.pixels + (size_t)(y + yy) * stride + x;
        for (int xx = 0; xx < w; xx++) row[xx] |= 0xFF000000u;
    }
    int r2 = radius * radius;
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int rx = radius - 1 - dx, ry = radius - 1 - dy;
            if (rx * rx + ry * ry <= r2) continue;
            g_scratch.pixels[(size_t)(y + dy) * stride + x + dx] = 0;
            g_scratch.pixels[(size_t)(y + dy) * stride + x + w - 1 - dx] = 0;
            g_scratch.pixels[(size_t)(y + h - 1 - dy) * stride + x + dx] = 0;
            g_scratch.pixels[(size_t)(y + h - 1 - dy) * stride + x + w - 1 - dx] = 0;
        }
    }
    sp->x = x; sp->y = y; sp->w = w; sp->h = h;
    sp->valid = true;
}

void fx_sprite_draw(const void* owner, const fx_sprite_t* sp, const gfx_target_t* dst,
                    int dx, int dy, int scale, uint8_t opacity) {
    if (g_owner != owner || !sp || !sp->valid) return;
    const uint32_t* src = g_scratch.pixels + (size_t)sp->y * g_scratch.width + sp->x;
    if (scale == 1000) {
        fx_blit(dst, sp->x + dx, sp->y + dy, src, g_scratch.width, sp->w, sp->h, opacity, true);
        return;
    }
    int dw = sp->w * scale / 1000, dh = sp->h * scale / 1000;
    fx_blit_scaled(dst, sp->x + dx + (sp->w - dw) / 2, sp->y + dy + (sp->h - dh) / 2, dw, dh,
                   src, g_scratch.width, sp->w, sp->h, opacity, true);
}

void fx_release(const void* owner) {
    if (g_owner == owner) g_owner = 0;
}

void fx_log_anim(const char* kind, const char* name, const char* phase,
                 uint32_t frames, uint32_t ms) {
    if (!fs_serial()) return;
    serial_puts("[anim] "); serial_puts(kind); serial_puts(" "); serial_puts(name);
    serial_puts(" "); serial_puts(phase);
    serial_puts(" frames="); serial_print_dec(frames);
    serial_puts(" ms=");     serial_print_dec(ms);
    serial_puts(" fps=");    serial_print_dec(ms ? (uint64_t)frames * 1000 / ms : 0);
    serial_puts("\n");
}

// ---------------------------------------------------------------------------
// Оверлеи.
// ---------------------------------------------------------------------------
// Статистика анимации: длительность = от начала до ПОСЛЕДНЕГО её кадра плюс
// один период кадра. Мерить «до момента, когда заметили конец» нельзя: сразу
// после появления панель может занять процессор своим делом (MIND начинает
// искать модель на диске — 200 мс), и это время не относится к анимации.
#define WM_FX_FRAME_MS 16

// Панели, которые хоть раз показывались: по ним WM узнаёт, идёт ли сейчас
// переход какого-нибудь оверлея (fx_motion_active).
#define FX_MAX_PANELS 8
static fx_panel_t* g_panels[FX_MAX_PANELS];
static int         g_npanels = 0;

static void panel_register(fx_panel_t* p) {
    for (int i = 0; i < g_npanels; i++) if (g_panels[i] == p) return;
    if (g_npanels < FX_MAX_PANELS) g_panels[g_npanels++] = p;
}

bool fx_motion_active(void) {
    for (int i = 0; i < g_npanels; i++)
        if ((g_panels[i]->shown || g_panels[i]->closing) && g_panels[i]->tr.running) return true;
    return false;
}

void fx_panel_show(fx_panel_t* p, uint32_t ms) {
    panel_register(p);
    p->shown = true;
    p->closing = false;
    p->sp.valid = false;
    p->frames = 0;
    p->t0 = p->t_last = timer_get_uptime_ms();
    ui_transition_start(&p->tr, ms, 0, UI_EASE_OUT_CUBIC);
}

void fx_panel_hide(fx_panel_t* p, uint32_t ms) {
    if (!p->shown) return;
    p->shown = false;
    p->sp.valid = false;                 // содержимое могло измениться — снять заново
    p->frames = 0;
    p->t0 = p->t_last = timer_get_uptime_ms();
    if (ui_theme_reduced_motion()) { p->closing = false; fx_release(p); wm_invalidate(); return; }
    p->closing = true;
    ui_transition_start(&p->tr, ms, 0, UI_EASE_OUT_CUBIC);
}

bool fx_panel_visible(const fx_panel_t* p) { return p->shown || p->closing; }

bool fx_panel_moving(fx_panel_t* p) {
    bool run = ui_transition_active(&p->tr);
    if (p->closing && !run) {
        // Исчезновение доиграло: оверлея больше нет, нужен кадр без него.
        p->closing = false;
        p->sp.valid = false;
        fx_release(p);
        fx_log_anim("panel", p->name ? p->name : "?", "close", p->frames,
                    (uint32_t)(p->t_last - p->t0 + WM_FX_FRAME_MS));
        wm_invalidate();
        return true;
    }
    if (p->shown && !run && p->frames) {
        // Появление доиграло, а последний кадр анимации был ещё «в пути»
        // (панель на пиксель не на месте, чуть прозрачная): просим ещё один,
        // уже чистовой кадр сразу, а не через 100 мс по таймеру обновления.
        wm_invalidate();
        return true;
    }
    return (p->shown || p->closing) && run;
}

uint8_t fx_panel_level(fx_panel_t* p) {
    uint8_t v = ui_transition_progress(&p->tr);
    return p->closing ? (uint8_t)(255 - v) : (p->shown ? v : 0);
}

// Переход не идёт. Если это только что закончившееся ПОЯВЛЕНИЕ — подвести
// итог и вернуться к обычной отрисовке. Исчезновение здесь не закрываем: его
// итог подводит fx_panel_moving. Иначе переход, истёкший между проверкой в
// цикле WM и этим кадром, терял счётчик кадров (в статистике выходило
// «frames=0») и снимок раньше времени.
static void open_done(fx_panel_t* p) {
    if (!p->shown || (!p->sp.valid && !p->frames)) return;
    if (p->frames)
        fx_log_anim("panel", p->name ? p->name : "?", "open", p->frames,
                    (uint32_t)(p->t_last - p->t0 + WM_FX_FRAME_MS));
    p->sp.valid = false;
    p->frames = 0;
    fx_release(p);
}

void fx_layer_frame(fx_panel_t* p, const gfx_target_t* t, fx_panel_draw_fn draw, void* arg) {
    if (!fx_panel_visible(p)) return;
    bool running = ui_transition_active(&p->tr);
    uint8_t lv = fx_panel_level(p);
    if (running) {
        p->frames++;
        p->t_last = timer_get_uptime_ms();
        if (!p->sp.valid) {
            gfx_target_t scr;
            if (fx_capture_begin(p, t, &scr)) {
                // Основа снимка — сцена этого кадра: слой рисуется поверх неё.
                size_t qwords = ((size_t)t->height * (size_t)t->pitch) / 8;
                void* d = scr.pixels; const void* s = t->pixels;
                __asm__ __volatile__("rep movsq" : "+D"(d), "+S"(s), "+c"(qwords) : : "memory");
                draw(&scr, arg);
                fx_capture_end(p, &p->sp, 0, 0, t->width, t->height, 0);
            }
        }
        if (p->sp.valid) {
            int inv = 255 - lv;
            fx_sprite_draw(p, &p->sp, t, 0, 0, 1000 + 50 * inv / 255, lv);
            return;
        }
    } else open_done(p);
    if (p->shown) draw(t, arg);
}

void fx_panel_frame(fx_panel_t* p, const gfx_target_t* t,
                    uint32_t dim_rgb, uint8_t dim_alpha,
                    int x, int y, int w, int h, int radius,
                    int slide_x, int slide_y,
                    fx_panel_draw_fn draw, void* arg) {
    if (!fx_panel_visible(p)) return;
    bool running = ui_transition_active(&p->tr);
    uint8_t lv = fx_panel_level(p);
    if (dim_alpha) fx_dim(t, 0, 0, t->width, t->height, dim_rgb, (uint8_t)((uint32_t)dim_alpha * lv / 255));

    if (running) {
        p->frames++;
        p->t_last = timer_get_uptime_ms();
        if (!p->sp.valid) {
            gfx_target_t scr;
            if (fx_capture_begin(p, t, &scr)) {
                draw(&scr, arg);
                fx_capture_end(p, &p->sp, x, y, w, h, radius);
            }
        }
        if (p->sp.valid) {
            // Панель приезжает из (slide_x, slide_y), чуть уменьшенной и
            // прозрачной: сдвиг и масштаб — по плавной кривой, прозрачность
            // набирается быстрее (к 60 % пути панель уже плотная).
            int inv = 255 - lv;
            uint32_t op = (uint32_t)lv * 5 / 3;
            if (op > 255) op = 255;
            fx_sprite_draw(p, &p->sp, t, slide_x * inv / 255, slide_y * inv / 255,
                           1000 - 40 * inv / 255, (uint8_t)op);
            return;
        }
    } else open_done(p);
    if (p->shown) draw(t, arg);
}
