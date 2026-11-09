// ============================================================================
// mind_widget.c — оверлей AMPLIFY MIND (этапы 49, 49.1). Контракт — mind_widget.h.
// ----------------------------------------------------------------------------
// Раскладка (логическая сетка 1600×1000, см. ui_layout):
//   ┌ логотип · AMPLIFY MIND · статус модели ─────────── состояние ┐
//   │ журнал реплик (прокрутка ↑/↓, новые строки — снизу)          │
//   │   ВЫ    — текст пользователя                                 │
//   │   MIND  — ответ модели (появляется по мере генерации)        │
//   │   TOOL  — точный вывод инструмента, моноширинный, на подложке │
//   │ [подтверждение exec, если модель попросила]                  │
//   │ [строка ввода]                                               │
//   └ подсказки клавиш ────────────────────────────────────────────┘
// Шрифты фиксированного пиксельного размера, поэтому перенос строк
// считается в экранных пикселях по advance глифов (как ui_font), за один
// проход. Раскладывается только видимый хвост журнала — даже длинная
// история не выходит за бюджет кадра (CLAUDE.md §5.5).
//
// Этап 49.1:
//   * ответы MIND — простой Markdown: **жирный**, списки "- " / "* " (точка
//     •), заголовки "#" (жирным), `код` (без обратных кавычек). Звёздочки
//     больше не видны как есть. Каждая реплика перед раскладкой проходит
//     через md_process в «экранный» текст + маску стилей (арена на кадр);
//   * режим размышления Qwen3 (think=1): текст между <think> и </think>
//     рисуется приглушённо, сами теги не показываются;
//   * Ctrl+L — раскладка поля ввода EN/RU (ЙЦУКЕН -> CP1251, только в этой
//     панели; системные раскладки — этап 53). Знаки / и ? остаются как в EN,
//     чтобы работали команды и вопросы;
//   * пока Qwen3 считает системный промпт («preparing»), вопрос уже можно
//     отправить — он подождёт.
// ============================================================================
#include "mind_widget.h"
#include "mind.h"
#include "mind_util.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_font.h"
#include "ui_icons.h"
#include "ui_motion.h"
#include "keyboard.h"
#include "timer.h"
#include "serial.h"
#include "ui_fx.h"
#include "compositor.h"

#define PX 250
#define PY 70
#define PW 1100
#define PH 860

// [48.2] Появление и исчезновение панели — через fx_panel (ui_fx.h): панель
// один раз снимается в буфер и приезжает снизу, проявляясь, а не
// перерисовывается целиком (журнал, Markdown) на каждом шаге анимации.
static fx_panel_t      g_fx = { .name = "mind" };
#define g_open (g_fx.shown)
static char            g_input[400];
static int             g_in_len;
static int             g_scroll;          // экранные пиксели от низа журнала
static bool            g_autoload_done;
static bool            g_autoload_pending;
static bool            g_ru;              // раскладка поля ввода: русская (ЙЦУКЕН)

void mind_widget_init(void) {
    g_fx.shown = false;
    g_fx.closing = false;
    g_in_len = 0;
    g_input[0] = 0;
    g_scroll = 0;
}

bool mind_widget_is_open(void) { return g_open; }

void mind_widget_show(void) {
    if (g_open) return;
    g_scroll = 0;
    fx_panel_show(&g_fx, 300);
    serial_puts("[mind] widget open\n");
    // Первое открытие: начинаем грузить модель сразу, пока человек печатает.
    // [48.2] ...но не в эту же миллисекунду: mind_load_begin ищет модели на
    // диске и занимает BSP на 200–300 мс — анимация появления панели шла
    // рывком (6 кадров вместо 20). Запуск — из mind_widget_tick, когда панель
    // доехала.
    if (!g_autoload_done && mind_state() == MIND_ST_EMPTY) {
        g_autoload_done = true;
        g_autoload_pending = true;
    }
}

void mind_widget_hide(void) {
    if (!g_open) return;
    fx_panel_hide(&g_fx, 220);
    serial_puts("[mind] widget close\n");
}

void mind_widget_toggle(void) {
    if (g_open) mind_widget_hide();
    else mind_widget_show();
}

bool mind_widget_active(void) {
    if (fx_panel_moving(&g_fx)) return true;
    if (!g_open) return false;
    mind_state_t st = mind_state();
    return st == MIND_ST_BUSY || st == MIND_ST_LOADING;
}

bool mind_widget_visible(void) { return fx_panel_visible(&g_fx); }

void mind_widget_tick(void) {
    // ...и чистовой кадр после анимации уже показан (frames сброшен).
    if (g_autoload_pending && g_open && !ui_transition_active(&g_fx.tr) && g_fx.frames == 0) {
        g_autoload_pending = false;
        (void)mind_load_begin(0);
    }
    mind_set_ui_busy(fx_motion_active() || compositor_peek_animation_active());
    mind_poll();
}

// ----------------------------------------------------------------------------
// Раскладка журнала.
// ----------------------------------------------------------------------------
enum { LK_LABEL = 0, LK_TEXT, LK_MONO, LK_INFO };

typedef struct {
    int16_t msg;
    uint8_t kind;
    int     start;               // смещение в арене экранного текста
    int     len;
} line_t;

#define MAX_LINES 256
static line_t g_lines[MAX_LINES];
static int    g_nlines;

// ---- экранный текст (арена на кадр) --------------------------------------------
enum { ST_BOLD = 1, ST_MUTED = 2 };
#define ARENA 65536
static char    g_dtext[ARENA];
static uint8_t g_dsty[ARENA];
static int     g_dn;

static void d_put(char c, uint8_t st) {
    if (g_dn < ARENA) { g_dtext[g_dn] = c; g_dsty[g_dn] = st; g_dn++; }
}

// Реплика -> экранный текст. MIND: Markdown и <think>; остальные — как есть.
static int md_process(int mi, int* out_len) {
    const char* s = mind_msg_text(mi);
    int n = (int)m_strlen(s);
    int start = g_dn;
    if (mind_msg_role(mi) != MIND_ROLE_MIND) {
        for (int i = 0; i < n; i++) d_put(s[i], 0);
        *out_len = g_dn - start;
        return start;
    }
    bool bold = false, muted = false, code = false, bol = true, head = false;
    for (int i = 0; i < n;) {
        if (m_starts(s + i, "<think>")) { muted = true; i += 7; while (s[i] == '\n') i++; continue; }
        if (m_starts(s + i, "</think>")) { muted = false; i += 8; while (s[i] == '\n') i++; bol = true; continue; }
        if (bol) {
            head = false;
            int k = i;
            while (s[k] == ' ') k++;
            // "- x" / "* x" / "+ x" -> "• x" (с сохранением отступа)
            if ((s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ') {
                for (int q = i; q < k; q++) d_put(' ', 0);
                d_put('\x95', muted ? ST_MUTED : 0);
                d_put(' ', 0);
                i = k + 2;
                bol = false;
                continue;
            }
            // "# Заголовок" … "#### Заголовок" -> жирная строка
            if (s[k] == '#') {
                int h = k;
                while (s[h] == '#') h++;
                if (s[h] == ' ' && h - k <= 4) { head = true; i = h + 1; bol = false; continue; }
            }
            bol = false;
        }
        if (s[i] == '*' && s[i + 1] == '*') { bold = !bold; i += 2; continue; }
        if (s[i] == '_' && s[i + 1] == '_') { bold = !bold; i += 2; continue; }
        if (s[i] == '`') { code = !code; i++; continue; }
        char c = s[i++];
        if (c == '\n') { bol = true; head = false; }
        uint8_t st = (uint8_t)((muted ? ST_MUTED : 0) | ((bold || head) && !muted ? ST_BOLD : 0));
        (void)code;
        d_put(c, st);
    }
    // Пустые строки подряд -> одна; хвостовые переводы строк — убрать.
    while (g_dn > start && g_dtext[g_dn - 1] == '\n') g_dn--;
    *out_len = g_dn - start;
    return start;
}

static ui_font_id_t kind_font(int k) {
    return k == LK_MONO ? UI_FONT_MONO_14 : k == LK_LABEL || k == LK_INFO ? UI_FONT_12 : UI_FONT_16;
}

static int kind_h(int k) {
    return ui_font_line_height(kind_font(k)) + 2;
}

static int glyph_adv_q(ui_font_id_t id, unsigned char c) {
    const ui_face_t* f = &ui_font_faces[id];
    if (c == '\t') c = ' ';
    if (c < 0x20) c = '?';
    return f->glyphs ? (int)f->glyphs[c - 0x20].advance_q : 32;
}

// Перенос одной реплики: строки в out (max штук), возврат — сколько.
static int wrap_msg(int mi, int kind, int width_px, line_t* out, int max) {
    int n = 0;
    int base = md_process(mi, &n);
    const char* s = g_dtext + base;
    ui_font_id_t f = kind_font(kind);
    int wq = width_px * 4, cnt = 0, pos = 0;
    if (n == 0 && max > 0) { out[0].msg = (int16_t)mi; out[0].kind = (uint8_t)kind; out[0].start = base; out[0].len = 0; return 1; }
    while (pos < n && cnt < max) {
        int ls = pos, w = 0, last_space = -1;
        while (pos < n && s[pos] != '\n') {
            int cw = glyph_adv_q(f, (unsigned char)s[pos]);
            if (w + cw > wq && pos > ls) break;
            if (s[pos] == ' ') last_space = pos;
            w += cw;
            pos++;
        }
        int len;
        if (pos < n && s[pos] != '\n' && last_space > ls) { len = last_space - ls; pos = last_space + 1; }
        else len = pos - ls;
        out[cnt].msg = (int16_t)mi;
        out[cnt].kind = (uint8_t)kind;
        out[cnt].start = base + ls;
        out[cnt].len = len;
        cnt++;
        if (pos < n && s[pos] == '\n') pos++;   // завершающий \n не даёт пустой строки
    }
    return cnt;
}

// Строит g_lines (сверху вниз) для хвоста журнала высотой need_px.
// Идём от последней реплики к первой и складываем строки с конца массива;
// метка роли добавляется, только если реплика поместилась целиком.
static int layout(int width_px, int need_px) {
    static line_t tmp[MAX_LINES];
    g_dn = 0;
    int fill = MAX_LINES;
    int acc = 0;
    for (int mi = mind_msg_count() - 1; mi >= 0 && fill > 0 && acc < need_px; mi--) {
        mind_role_t r = mind_msg_role(mi);
        int kind = r == MIND_ROLE_TOOL ? LK_MONO : r == MIND_ROLE_INFO ? LK_INFO : LK_TEXT;
        int n = wrap_msg(mi, kind, width_px, tmp, MAX_LINES);
        int added = 0;
        for (int k = n - 1; k >= 0 && fill > 0; k--, added++) {
            g_lines[--fill] = tmp[k];
            acc += kind_h(kind);
        }
        if (kind != LK_INFO && added == n && fill > 0) {
            line_t lb = { (int16_t)mi, LK_LABEL, 0, 0 };
            g_lines[--fill] = lb;
            acc += kind_h(LK_LABEL);
        }
        acc += 12;                          // зазор между репликами
    }
    g_nlines = MAX_LINES - fill;
    for (int k = 0; k < g_nlines; k++) g_lines[k] = g_lines[fill + k];
    return acc;
}

// ----------------------------------------------------------------------------
// Отрисовка.
// ----------------------------------------------------------------------------
static void fill_l(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}
static void round_l(const gfx_target_t* t, int x, int y, int w, int h, int r, uint32_t c) {
    gfx_fill_round_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), ui_uw(r), c);
}

static int draw_text_px(const gfx_target_t* t, int x, int base, const char* s, int n,
                        uint32_t color, ui_font_id_t f, bool bold) {
    static char buf[512];
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        buf[i] = (c == '\t') ? ' ' : c;
    }
    buf[n] = 0;
    ui_font_draw_baseline_ex(t, x, base, buf, color, f, 0, bold);
    return ui_font_text_width(buf, f);
}

// Строка экранного текста с разными стилями — отрезками одного стиля.
static int draw_styled(const gfx_target_t* t, int x, int base, int start, int n,
                       uint32_t ink, uint32_t muted, ui_font_id_t f) {
    int i = 0;
    while (i < n) {
        uint8_t st = g_dsty[start + i];
        int k = i;
        while (k < n && g_dsty[start + k] == st) k++;
        x += draw_text_px(t, x, base, g_dtext + start + i, k - i, (st & ST_MUTED) ? muted : ink, f,
                          (st & ST_BOLD) != 0);
        i = k;
    }
    return x;
}

static const char* role_label(mind_role_t r) {
    switch (r) {
        case MIND_ROLE_USER: return "YOU";
        case MIND_ROLE_MIND: return "MIND";
        case MIND_ROLE_TOOL: return "SYSTEM DATA";
        default: return "";
    }
}

static const char* state_label(mind_state_t st) {
    switch (st) {
        case MIND_ST_EMPTY:   return "no model";
        case MIND_ST_LOADING: return "loading";
        case MIND_ST_READY:   return "ready";
        case MIND_ST_BUSY:    return "thinking";
        case MIND_ST_CONFIRM: return "waiting for you";
        default:              return "error";
    }
}

static void mind_panel(const gfx_target_t* t, void* arg);

void mind_widget_draw(const gfx_target_t* t) {
    if (!t || !fx_panel_visible(&g_fx)) return;
    ui_layout_update(t->width, t->height);
    // Экран под панелью приглушается (раньше «затемнение» было сплошной
    // заливкой: gfx_fill_rect альфу не смешивает), панель приезжает снизу.
    fx_panel_frame(&g_fx, t, 0x241F27, 150,
                   ui_ux(PX), ui_uy(PY), ui_uw(PW), ui_uh(PH), ui_uw(18),
                   0, ui_uh(56), mind_panel, 0);
}

// Панель на своём месте. Зовётся и для экрана, и для снимка.
static void mind_panel(const gfx_target_t* t, void* arg) {
    (void)arg;
    const ui_runtime_theme_t* th = ui_theme_current();
    mind_state_t st = mind_state();
    int y0 = PY;
    round_l(t, PX, y0, PW, PH, 18, th->border);
    round_l(t, PX + 1, y0 + 1, PW - 2, PH - 2, 17, th->surface);
    // Тонкая «закатная» полоса сверху — фирменный градиент кнопок Pearl.
    gfx_fill_rect_gradient(t, ui_ux(PX + 18), ui_uy(y0 + 1), ui_uw(PW - 36), ui_uh(3), 0xFFE69D40, 0xFFC45A79);

    // ---- шапка -----------------------------------------------------------------
    const ui_bitmap_t* logo = ui_logo_small();
    if (logo) ui_draw_bitmap_scaled(t, ui_ux(PX + 30), ui_uy(y0 + 24), ui_uw(40), ui_uh(40), logo);
    ui_font_draw_baseline_ex(t, ui_ux(PX + 84), ui_uy(y0 + 46), "AMPLIFY MIND", th->ink, UI_FONT_20, 1, true);
    char status[160];
    mind_status(status, sizeof(status));
    ui_font_draw_baseline_ex(t, ui_ux(PX + 84), ui_uy(y0 + 70), status, th->muted, UI_FONT_12, 0, false);

    const char* sl = state_label(st);
    char chip[40];
    chip[0] = 0;
    m_append(chip, sizeof(chip), sl);
    if (st == MIND_ST_LOADING) { m_append(chip, sizeof(chip), " "); m_append_dec(chip, sizeof(chip), mind_load_progress()); m_append(chip, sizeof(chip), "%"); }
    int cw = ui_font_text_width(chip, UI_FONT_12);
    int cx = ui_ux(PX + PW - 36) - cw;
    uint32_t dot = st == MIND_ST_BUSY ? th->accent_coral : st == MIND_ST_READY ? th->accent_orange :
                   st == MIND_ST_ERROR ? th->accent_berry : th->muted;
    // Пульсирующая точка, пока модель думает.
    if (st == MIND_ST_BUSY && ((timer_get_uptime_ms() / 300) & 1)) dot = th->accent_berry;
    gfx_fill_round_rect(t, cx - ui_uw(22), ui_uy(y0 + 36), ui_uw(10), ui_uw(10), ui_uw(5), dot);
    ui_font_draw_baseline_ex(t, cx, ui_uy(y0 + 45), chip, th->ink, UI_FONT_12, 0, true);
    if (st == MIND_ST_LOADING) {
        int bw = 260;
        fill_l(t, PX + PW - 36 - bw, y0 + 60, bw, 4, th->surface_soft);
        fill_l(t, PX + PW - 36 - bw, y0 + 60, bw * mind_load_progress() / 100, 4, th->accent_coral);
    }
    fill_l(t, PX, y0 + 92, PW, 1, th->border);

    // ---- журнал ------------------------------------------------------------------
    const int TX = PX + 40, TW = PW - 80, TTOP = y0 + 108;
    bool confirm = st == MIND_ST_CONFIRM;
    const int TBOT = y0 + PH - (confirm ? 176 : 124);
    int sx = ui_ux(TX), sw = ui_uw(TW), stop = ui_uy(TTOP), sbot = ui_uy(TBOT);
    int view_h = sbot - stop;
    int total = layout(sw - 24, view_h + g_scroll + 64);
    if (g_scroll > total - view_h) g_scroll = total - view_h > 0 ? total - view_h : 0;

    if (mind_msg_count() == 0) {
        ui_font_draw_baseline_ex(t, sx, stop + 40, "Ask anything about this computer.", th->ink, UI_FONT_16, 0, true);
        ui_font_draw_baseline_ex(t, sx, stop + 68,
            "MIND runs a local language model on the CPU cores of this machine - nothing leaves the computer.",
            th->muted, UI_FONT_14, 0, false);
        ui_font_draw_baseline_ex(t, sx, stop + 104, "Try:  /ps    /ls /disk    /logs    what processes are running?",
            th->muted, UI_FONT_14, 0, false);
        ui_font_draw_baseline_ex(t, sx, stop + 128, "Type /help for all commands.", th->muted, UI_FONT_14, 0, false);
    }

    // Снизу вверх: последняя строка у нижнего края области.
    // y каждой строки: зазор 12 между разными репликами.
    static int heights[MAX_LINES], ys[MAX_LINES];
    int y = sbot + g_scroll;
    for (int k = g_nlines - 1; k >= 0; k--) {
        heights[k] = kind_h(g_lines[k].kind);
        if (k < g_nlines - 1 && g_lines[k].msg != g_lines[k + 1].msg) y -= 12;
        y -= heights[k];
        ys[k] = y;
    }
    int last_mind = -1;
    if (st == MIND_ST_BUSY)
        for (int i = mind_msg_count() - 1; i >= 0; i--) if (mind_msg_role(i) == MIND_ROLE_MIND) { last_mind = i; break; }
    for (int k = 0; k < g_nlines; k++) {
        line_t* L = &g_lines[k];
        int ly = ys[k], lh = heights[k];
        if (ly + lh < stop || ly > sbot) continue;
        mind_role_t role = mind_msg_role(L->msg);
        ui_font_id_t f = kind_font(L->kind);
        int base = ly + ui_font_ascent(f) + 1;
        if (ly < stop || ly + lh > sbot + 2) continue;          // частично видимые не рисуем
        if (L->kind == LK_MONO) gfx_fill_rect(t, sx, ly - 1, sw, lh + 1, th->surface_soft);
        if (L->kind == LK_LABEL) {
            uint32_t c = role == MIND_ROLE_USER ? th->accent_coral : role == MIND_ROLE_MIND ? th->accent_berry : th->accent_orange;
            gfx_fill_rect(t, sx, ly + 3, ui_uw(3), lh - 6, c);
            ui_font_draw_baseline_ex(t, sx + ui_uw(10), base, role_label(role), c, UI_FONT_12, 1, true);
            continue;
        }
        uint32_t col = L->kind == LK_INFO ? th->muted : th->ink;
        int x = sx + (L->kind == LK_MONO ? ui_uw(12) : ui_uw(10));
        int xe = draw_styled(t, x, base, L->start, L->len, col, th->muted, f);
        bool last_line = (k == g_nlines - 1) || g_lines[k + 1].msg != L->msg;
        if (last_line && L->msg == last_mind && ((timer_get_uptime_ms() / 400) & 1))
            gfx_fill_rect(t, xe + 2, base - ui_font_ascent(f) + 2, ui_uw(8), ui_font_ascent(f), th->accent_coral);
    }
    if (g_scroll > 0) {
        ui_font_draw_baseline_ex(t, ui_ux(PX + PW - 190), ui_uy(TTOP) + 14, "scrolled - Down to return", th->muted, UI_FONT_12, 0, false);
    }

    // ---- подтверждение exec ---------------------------------------------------------
    if (confirm) {
        int by = y0 + PH - 170;
        round_l(t, PX + 24, by, PW - 48, 40, 10, th->accent_coral);
        char q[320];
        q[0] = 0;
        m_append(q, sizeof(q), "MIND wants to run:  ");
        const char* pe = mind_pending_exec();
        m_append(q, sizeof(q), pe ? pe : "");
        m_append(q, sizeof(q), "      Enter - allow    Esc - deny");
        ui_font_draw_baseline_ex(t, ui_ux(PX + 44), ui_uy(by + 26), q, th->on_accent, UI_FONT_14, 0, true);
    }

    // ---- строка ввода -----------------------------------------------------------------
    int iy = y0 + PH - 112;
    bool warm = st == MIND_ST_BUSY && mind_warming();
    round_l(t, PX + 24, iy, PW - 48, 56, 12, st == MIND_ST_BUSY && !warm ? th->border : th->accent_coral);
    round_l(t, PX + 26, iy + 2, PW - 52, 52, 11, th->surface);
    int ibase = ui_uy(iy + 36);
    int ix = ui_ux(PX + 48);
    if (g_in_len == 0) {
        const char* ph = warm ? "Preparing the model - you can already type" :
                         st == MIND_ST_BUSY ? "MIND is answering...  Esc - stop" :
                         st == MIND_ST_LOADING ? "Loading the model - you can already type" :
                         g_ru ? "\xC7\xE0\xE4\xE0\xE9\xF2\xE5 \xE2\xEE\xEF\xF0\xEE\xF1 MIND (Ctrl+L - EN)" :
                         "Ask MIND, or type /help   (Ctrl+L - RU)";
        ui_font_draw_baseline_ex(t, ix, ibase, ph, th->muted, UI_FONT_16, 0, false);
    } else {
        // Показываем хвост, если строка шире поля.
        const char* s = g_input;
        int maxw = ui_uw(PW - 110);
        while (*s && ui_font_text_width(s, UI_FONT_16) > maxw) s++;
        ui_font_draw_baseline_ex(t, ix, ibase, s, th->ink, UI_FONT_16, 0, false);
        ix += ui_font_text_width(s, UI_FONT_16);
    }
    if (((timer_get_uptime_ms() / 500) & 1) == 0 && (st != MIND_ST_BUSY || warm))
        gfx_fill_rect(t, ix + 2, ibase - 16, 2, 20, th->accent_coral);
    // Индикатор раскладки поля ввода.
    {
        const char* lay = g_ru ? "RU" : "EN";
        int lw = ui_font_text_width(lay, UI_FONT_12);
        ui_font_draw_baseline_ex(t, ui_ux(PX + PW - 44) - lw, ui_uy(iy + 33), lay,
                                 g_ru ? th->accent_coral : th->muted, UI_FONT_12, 1, true);
    }

    ui_font_draw_baseline_ex(t, ui_ux(PX + 30), ui_uy(y0 + PH - 22),
        "Enter ask  \xB7  Esc close / stop  \xB7  Up/Down scroll  \xB7  Ctrl+L EN/RU  \xB7  /help commands  \xB7  runs locally",
        th->muted, UI_FONT_12, 0, false);
}

// ----------------------------------------------------------------------------
// Ввод.
// ----------------------------------------------------------------------------
// ЙЦУКЕН: клавиша US-раскладки -> буква CP1251. Строки ниже — пары
// «латиница/знак -> кириллица» (литералы в исходнике UTF-8, GCC кладёт их
// в CP1251, см. CLAUDE.md §5.1).
static char ru_key(char c) {
    static const char en[] = "qwertyuiop[]asdfghjkl;'zxcvbnm,.`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>~";
    static const char ru[] = "йцукенгшщзхъфывапролджэячсмитьбюёЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮЁ";
    for (int i = 0; en[i]; i++) if (en[i] == c) return ru[i];
    return c;
}

bool mind_widget_handle_key(char c) {
    if (!g_open) return false;
    mind_state_t st = mind_state();
    if (c == KBD_KEY_ESC) {
        if (st == MIND_ST_CONFIRM) mind_confirm(false);
        else if (st == MIND_ST_BUSY) mind_cancel();
        else mind_widget_hide();
        return true;
    }
    if (c == 3) { mind_cancel(); return true; }                      // Ctrl+C
    if (c == 12) {                                                   // Ctrl+L — EN/RU
        g_ru = !g_ru;
        serial_puts(g_ru ? "[mind] layout RU\n" : "[mind] layout EN\n");
        return true;
    }
    if (c == KBD_KEY_UP) { g_scroll += 60; return true; }
    if (c == KBD_KEY_DOWN) { g_scroll -= 60; if (g_scroll < 0) g_scroll = 0; return true; }
    if (c == '\n') {
        if (st == MIND_ST_CONFIRM) { mind_confirm(true); return true; }
        if (g_in_len > 0 && (st != MIND_ST_BUSY || mind_warming())) {
            g_scroll = 0;
            (void)mind_submit(g_input);
            g_in_len = 0;
            g_input[0] = 0;
        }
        return true;
    }
    if (c == '\b') {
        if (g_in_len > 0) g_input[--g_in_len] = 0;
        return true;
    }
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F && g_in_len + 1 < (int)sizeof(g_input)) {
        g_input[g_in_len++] = g_ru ? ru_key(c) : c;
        g_input[g_in_len] = 0;
    }
    return true;                         // пока панель открыта, клавиши — её
}

bool mind_widget_handle_click(int sx, int sy) {
    if (!g_open) return false;
    int x = ui_lx(sx), y = ui_ly(sy);
    if (x < PX || x > PX + PW || y < PY || y > PY + PH) mind_widget_hide();
    return true;
}
