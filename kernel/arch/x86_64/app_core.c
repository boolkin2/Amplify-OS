// ============================================================================
// app_core.c — Core, «системная призма» AMPLIFY-OS (этап F).
// ----------------------------------------------------------------------------
// Core заменил собой меню «Пуск»: сетка плиток строится из реестра
// desktop_add_icon (desktop.c), поэтому ни одно приложение не может
// «потеряться» — что зарегистрировано, то и показано.
//
//   * 3 × 3 плитки на страницу + пагинация (клавиши, кнопки, цифры 1..9).
//   * Правая колонка — живая телеметрия: CPU (cpu_stats), память (pmm),
//     Strata (vfs), аптайм (timer), активные призмы (task).
//   * Появление плиток анимировано (ui_motion, 200 мс ease-out, шаг 25 мс);
//     при reduced motion анимация автоматически выключается.
//   * Reboot/Shutdown требуют подтверждения вторым нажатием.
// ============================================================================
#include "apps.h"
#include "app_stage_e.h"
#include "desktop.h"
#include "heap.h"
#include "pmm.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"
#include "cpu_stats.h"
#include "keyboard.h"
#include "ui_motion.h"

#define CORE_COLS      3
#define CORE_ROWS      3
#define CORE_PER_PAGE  (CORE_COLS * CORE_ROWS)
#define CORE_GRID_X    78
#define CORE_GRID_Y    108
#define CORE_CARD_H    200
#define CORE_ROW_STEP  220
#define CORE_GAP       20
#define CORE_SIDE_W    290
#define CORE_SIDE_PAD  70

typedef struct {
    const char*  key;      // имя в реестре desktop
    const char*  label;    // как показываем в Core
    const char*  sub;      // подпись
    ui_icon_id_t icon;
} core_meta_t;

static const core_meta_t META[] = {
    {"About",     "About",     "the prism story",         UI_ICON_AMPLIFY},
    {"Files",     "Strata",    "files and capacity",      UI_ICON_FILES},
    {"Notes",     "Thread",    "notes that keep context", UI_ICON_NOTES},
    {"Calc",      "Calc",      "quick arithmetic",        UI_ICON_STORE},
    {"Term",      "Terminal",  "amplify shell",           UI_ICON_TERMINAL},
    {"Paint",     "Paint",     "sketch on glass",         UI_ICON_GALLERY},
    {"Embers",    "Embers",    "ambient particles",       UI_ICON_MUSIC},
    {"Browser",   "Browser",   "the open web",            UI_ICON_BROWSER},
    {"3D \u043a\u0443\u0431",    "GL Cube",   "software renderer",       UI_ICON_GALLERY},
    {"Amply",     "Amply",     "packages and prisms",     UI_ICON_STORE},
    {"Sandbox",   "Sandbox",   "isolated prisms",         UI_ICON_FOLDER},
    {"Info",      "Amplifier", "system telemetry",        UI_ICON_SETTINGS},
    {"Signal",    "Signal",    "messages in one lane",    UI_ICON_MAIL},
    {"Arc",       "Arc",       "day as one turn",         UI_ICON_CALENDAR},
    {"Resonance", "Resonance", "spatial sound",           UI_ICON_MUSIC},
    {"Core",      "Core",      "system prism",            UI_ICON_SETTINGS},
    {"Facet",     "Facet",     "warm gradient",           UI_ICON_GALLERY},
    {"Mesh",      "Mesh",      "devices linked",          UI_ICON_AMPLIFY},
    {"Capacity",  "Capacity",  "space and strata",        UI_ICON_FILES},
    {"Clock",     "Clock",     "time and alarms",         UI_ICON_CALENDAR},
    {"Matrix",    "Matrix",    "rain of glyphs",          UI_ICON_GALLERY},
    {"Tasks",     "Prisms",    "processes and load",      UI_ICON_SETTINGS},
    {"Reboot",    "Reboot",    "warm restart",            UI_ICON_AMPLIFY},
    {"Shutdown",  "Shutdown",  "close every prism",       UI_ICON_TRASH},
};
#define META_N ((int)(sizeof(META) / sizeof(META[0])))

typedef struct {
    int  selected;
    int  page;
    int  confirm;                       // индекс плитки, ждущей подтверждения
    char status[64];
    ui_transition_t tile[CORE_PER_PAGE];
} core_t;

// ---------------------------------------------------------------- utilities
static bool eq(const char* a, const char* b) {
    int i = 0;
    for (; a && b && a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return a && b && a[i] == 0 && b[i] == 0;
}

static const core_meta_t* meta_of(const char* key) {
    for (int i = 0; i < META_N; i++) if (eq(META[i].key, key)) return &META[i];
    return 0;
}

static const char* label_of(const desktop_icon_t* ic) {
    const core_meta_t* m = meta_of(ic->name);
    return m ? m->label : ic->name;
}

static const char* sub_of(const desktop_icon_t* ic) {
    const core_meta_t* m = meta_of(ic->name);
    return m ? m->sub : "prism";
}

static ui_icon_id_t icon_of(const desktop_icon_t* ic) {
    const core_meta_t* m = meta_of(ic->name);
    if (m) return m->icon;
    if (ic->icon_id >= 0 && ic->icon_id < UI_ICON_COUNT) return (ui_icon_id_t)ic->icon_id;
    return UI_ICON_AMPLIFY;
}

static bool is_danger(const desktop_icon_t* ic) {
    return eq(ic->name, "Reboot") || eq(ic->name, "Shutdown");
}

static void right_text(const gfx_target_t* t, int right, int y, const char* s, uint32_t c) {
    se_text(t, right - gfx_string_width_ui(s), y, s, c);
}

static void pct_text(char* out, uint32_t pct) {
    se_num(out, pct);
    int n = se_len(out);
    out[n] = '%';
    out[n + 1] = 0;
}

// «3 h 07 m» / «12 m 30 s» — аптайм человеческим языком.
static void uptime_text(char* out) {
    uint64_t sec = timer_get_uptime_ms() / 1000;
    char a[16], b[16];
    if (sec >= 3600) { se_num(a, sec / 3600); se_num(b, (sec / 60) % 60); }
    else             { se_num(a, sec / 60);   se_num(b, sec % 60); }
    int n = 0;
    for (int i = 0; a[i]; i++) out[n++] = a[i];
    out[n++] = ' ';
    out[n++] = (sec >= 3600) ? 'h' : 'm';
    out[n++] = ' ';
    for (int i = 0; b[i]; i++) out[n++] = b[i];
    out[n++] = ' ';
    out[n++] = (sec >= 3600) ? 'm' : 's';
    out[n] = 0;
}

static uint32_t cpu_load_pct(void) {
    uint64_t busy = 0, idle = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!cpu_stats[i].online) continue;
        busy += cpu_stats[i].busy_ticks;
        idle += cpu_stats[i].idle_ticks;
    }
    uint64_t total = busy + idle;
    if (!total) return 0;
    return (uint32_t)(busy * 100 / total);
}

static uint32_t mem_used_pct(void) {
    pmm_stats_t p = pmm_get_stats();
    uint64_t total = p.used_pages + p.free_pages;
    if (!total) return 0;
    return (uint32_t)(p.used_pages * 100 / total);
}

// Маленький шеврон «открыть».
static void chevron(const gfx_target_t* t, int x, int y, uint32_t c) {
    for (int i = 0; i < 4; i++) {
        gfx_fill_rect(t, x + i, y + i, 2, 2, c);
        gfx_fill_rect(t, x + i, y + 6 - i, 2, 2, c);
    }
}

// --------------------------------------------------------------- navigation
static int page_count(void) {
    int n = desktop_icon_count();
    if (n <= 0) return 1;
    return (n + CORE_PER_PAGE - 1) / CORE_PER_PAGE;
}

static void restart_anim(core_t* s) {
    for (int i = 0; i < CORE_PER_PAGE; i++)
        ui_transition_start(&s->tile[i], 200, (uint32_t)(i * 25), UI_EASE_OUT_CUBIC);
}

static void go_page(core_t* s, int page) {
    int pages = page_count();
    if (page < 0) page = pages - 1;
    if (page >= pages) page = 0;
    if (page == s->page) return;
    s->page = page;
    s->confirm = -1;
    restart_anim(s);
}

static void select_index(core_t* s, int index) {
    int n = desktop_icon_count();
    if (n <= 0) return;
    if (index < 0) index = n - 1;
    if (index >= n) index = 0;
    s->selected = index;
    if (index != s->confirm) s->confirm = -1;
    go_page(s, index / CORE_PER_PAGE);
}

static void launch_index(core_t* s, int index) {
    const desktop_icon_t* ic = desktop_icon_at(index);
    if (!ic) return;
    if (is_danger(ic) && s->confirm != index) {
        s->confirm = index;
        se_copy(s->status, "press again to confirm", 64);
        return;
    }
    s->confirm = -1;
    if (eq(ic->name, "Core")) {
        se_copy(s->status, "Core is already open", 64);
        return;
    }
    char msg[64];
    se_copy(msg, "opening ", 64);
    int n = se_len(msg);
    const char* lbl = label_of(ic);
    for (int i = 0; lbl[i] && n < 62; i++) msg[n++] = lbl[i];
    msg[n] = 0;
    se_copy(s->status, msg, 64);
    if (ic->on_click) ic->on_click();
}

// -------------------------------------------------------------------- paint
static void draw_tile(const gfx_target_t* t, core_t* s, const ui_runtime_theme_t* h,
                      int slot, int index, int x, int y, int cw) {
    const desktop_icon_t* ic = desktop_icon_at(index);
    if (!ic) return;

    uint8_t  p       = ui_transition_progress(&s->tile[slot]);
    int      dy      = ui_transition_lerp(16, 0, p);
    bool     active  = (index == s->selected);
    bool     danger  = is_danger(ic);
    uint32_t surface = ui_lerp(h->canvas, h->surface, p);
    uint32_t border  = ui_lerp(h->canvas, active ? h->accent_coral : h->border, p);
    uint32_t ink     = ui_lerp(h->canvas, h->ink, p);
    uint32_t muted   = ui_lerp(h->canvas, h->muted, p);

    y += dy;
    if (active) gfx_drop_shadow(t, x, y + 6, cw, CORE_CARD_H, 16, h->shadow);
    se_card(t, x, y, cw, CORE_CARD_H, surface, border);

    // Плитка иконки: акцентный градиент для выбранной, мягкая заливка иначе.
    uint32_t tile_a = active ? h->accent_orange : h->surface_soft;
    uint32_t tile_b = active ? (danger ? h->accent_berry : h->accent_coral) : h->surface_soft;
    gfx_fill_round_rect(t, x + 24, y + 24, 56, 56, 16, ui_lerp(h->canvas, tile_a, p));
    gfx_fill_rect_gradient(t, x + 26, y + 26, 52, 52,
                           ui_lerp(h->canvas, tile_a, p), ui_lerp(h->canvas, tile_b, p));
    ui_draw_bitmap_alpha(t, x + 37, y + 37, 30, 30, ui_icon(icon_of(ic)), p);

    se_bold(t, x + 24, y + 100, label_of(ic), ink);
    se_text(t, x + 24, y + 130, sub_of(ic), muted);

    const char* action = (s->confirm == index) ? "CONFIRM" : "OPEN";
    uint32_t    acol   = (s->confirm == index) ? ui_lerp(h->canvas, h->accent_berry, p) : muted;
    se_text(t, x + 24, y + 166, action, acol);
    chevron(t, x + 24 + gfx_string_width_ui(action) + 10, y + 170, acol);

    // Индекс для быстрого запуска (1..9).
    char num[4];
    se_num(num, (uint64_t)(slot + 1));
    right_text(t, x + cw - 24, y + 24, num, ui_lerp(h->canvas, h->border, p));
}

static void draw_side(const gfx_target_t* t, window_t* w, const ui_runtime_theme_t* h) {
    int x = w->width - (CORE_SIDE_W + 40);
    int card_h = w->height - CORE_GRID_Y - 42;
    se_card(t, x, CORE_GRID_Y, CORE_SIDE_W, card_h, h->surface, h->border);

    se_text(t, x + 26, CORE_GRID_Y + 28, "AMPLIFY-OS", h->muted);
    ui_draw_bitmap_scaled(t, x + 26, CORE_GRID_Y + 64, 72, 72, ui_logo_big());
    se_bold(t, x + 26, CORE_GRID_Y + 162, "CHRT core 1.0", h->ink);
    se_text(t, x + 26, CORE_GRID_Y + 192, "build 2026.09 - prism kernel", h->muted);

    int bar_w = CORE_SIDE_W - 52, right = x + 26 + bar_w;
    char buf[24];

    uint32_t cpu = cpu_load_pct();
    se_text(t, x + 26, CORE_GRID_Y + 248, "CORE LOAD", h->muted);
    pct_text(buf, cpu);
    right_text(t, right, CORE_GRID_Y + 248, buf, h->ink);
    se_progress(t, x + 26, CORE_GRID_Y + 276, bar_w, (uint8_t)(cpu * 255 / 100),
                h->border, h->accent_berry);

    uint32_t mem = mem_used_pct();
    se_text(t, x + 26, CORE_GRID_Y + 312, "MEMORY", h->muted);
    pct_text(buf, mem);
    right_text(t, right, CORE_GRID_Y + 312, buf, h->ink);
    se_progress(t, x + 26, CORE_GRID_Y + 340, bar_w, (uint8_t)(mem * 255 / 100),
                h->border, h->accent_berry);

    int files = vfs_count();
    if (files < 0) files = 0;
    se_text(t, x + 26, CORE_GRID_Y + 376, "STRATA", h->muted);
    se_num(buf, (uint64_t)files);
    right_text(t, right, CORE_GRID_Y + 376, buf, h->ink);
    uint32_t strata = files > 128 ? 255u : (uint32_t)(files * 255 / 128);
    se_progress(t, x + 26, CORE_GRID_Y + 404, bar_w, (uint8_t)strata, h->border, h->accent_berry);

    gfx_fill_round_rect(t, x + 26, CORE_GRID_Y + 444, bar_w, 46, 14, h->accent_berry);
    gfx_fill_rect_gradient(t, x + 28, CORE_GRID_Y + 446, bar_w - 4, 42,
                           h->accent_orange, h->accent_berry);
    int bw = gfx_string_width_ui("check for prisms");
    se_bold(t, x + 26 + (bar_w - bw) / 2, CORE_GRID_Y + 459, "check for prisms", h->on_accent);

    se_text(t, x + 26, CORE_GRID_Y + 532, "THIS DEVICE", h->muted);
    se_bold(t, x + 26, CORE_GRID_Y + 566, "Prism 14", h->ink);

    struct { const char* k; char v[24]; } rows[3];
    rows[0].k = "uptime";        uptime_text(rows[0].v);
    rows[1].k = "active prisms"; se_num(rows[1].v, (uint64_t)task_count());
    rows[2].k = "apps";          se_num(rows[2].v, (uint64_t)desktop_icon_count());
    for (int i = 0; i < 3; i++) {
        int ry = CORE_GRID_Y + 606 + i * 34;
        if (ry > CORE_GRID_Y + card_h - 26) break;
        se_text(t, x + 26, ry, rows[i].k, h->muted);
        right_text(t, right, ry, rows[i].v, h->ink);
    }
}

static void core_draw(window_t* w, const gfx_target_t* t) {
    core_t* s = w->user_data;
    const ui_runtime_theme_t* h = ui_theme_current();
    gfx_clear(t, h->canvas);
    se_header(t, "CORE - SYSTEM PRISM", "Core", false);

    int total = desktop_icon_count();
    int grid_w = w->width - (CORE_SIDE_W + CORE_SIDE_PAD) - CORE_GRID_X;
    int cw = (grid_w - CORE_GAP * (CORE_COLS - 1)) / CORE_COLS;
    if (cw < 160) cw = 160;

    bool animating = false;
    for (int slot = 0; slot < CORE_PER_PAGE; slot++) {
        int index = s->page * CORE_PER_PAGE + slot;
        if (index >= total) break;
        int col = slot % CORE_COLS, row = slot / CORE_COLS;
        draw_tile(t, s, h, slot, index,
                  CORE_GRID_X + col * (cw + CORE_GAP),
                  CORE_GRID_Y + row * CORE_ROW_STEP, cw);
        if (ui_transition_active(&s->tile[slot])) animating = true;
    }
    if (total == 0) se_text(t, CORE_GRID_X, CORE_GRID_Y + 40, "no prisms registered", h->muted);

    // Пагинация.
    int py = CORE_GRID_Y + CORE_ROWS * CORE_ROW_STEP + 6;
    if (py > w->height - 96) py = w->height - 96;
    int pages = page_count();
    se_card(t, CORE_GRID_X, py, 110, 40, h->surface, h->border);
    se_text(t, CORE_GRID_X + 30, py + 12, "prev", h->muted);
    se_card(t, CORE_GRID_X + 126, py, 110, 40, h->surface, h->border);
    se_text(t, CORE_GRID_X + 156, py + 12, "next", h->muted);
    for (int i = 0; i < pages && i < 8; i++) {
        uint32_t dot = (i == s->page) ? h->accent_coral : h->border;
        gfx_fill_round_rect(t, CORE_GRID_X + 262 + i * 18, py + 17, 10, 6, 3, dot);
    }
    char pbuf[24], nbuf[24];
    se_num(pbuf, (uint64_t)(s->page + 1));
    se_num(nbuf, (uint64_t)pages);
    se_text(t, CORE_GRID_X + 262 + 8 * 18 + 12, py + 12, pbuf, h->ink);
    se_text(t, CORE_GRID_X + 262 + 8 * 18 + 12 + gfx_string_width_ui(pbuf) + 6, py + 12, "/", h->muted);
    se_text(t, CORE_GRID_X + 262 + 8 * 18 + 12 + gfx_string_width_ui(pbuf) + 18, py + 12, nbuf, h->muted);

    draw_side(t, w, h);
    se_text(t, CORE_GRID_X, w->height - 34, s->status, h->accent_berry);

    // Кадры нужны только пока идёт анимация появления.
    w->wants_continuous_redraw = animating;
}

// ------------------------------------------------------------------- input
static void core_key(window_t* w, char c) {
    core_t* s = w->user_data;
    int total = desktop_icon_count();
    if (c == KBD_KEY_ESC) { se_close(w); return; }
    if (total <= 0) return;

    if (c == KBD_KEY_LEFT)       select_index(s, s->selected - 1);
    else if (c == KBD_KEY_RIGHT || c == KBD_KEY_TAB) select_index(s, s->selected + 1);
    else if (c == KBD_KEY_UP)    select_index(s, s->selected - CORE_COLS);
    else if (c == KBD_KEY_DOWN)  select_index(s, s->selected + CORE_COLS);
    else if (c == KBD_KEY_BACKTAB) select_index(s, s->selected - 1);
    else if (c == '[')           { go_page(s, s->page - 1); select_index(s, s->page * CORE_PER_PAGE); }
    else if (c == ']')           { go_page(s, s->page + 1); select_index(s, s->page * CORE_PER_PAGE); }
    else if (c >= '1' && c <= '9') {
        int index = s->page * CORE_PER_PAGE + (c - '1');
        if (index < total) { s->selected = index; launch_index(s, index); }
    }
    else if (c == KBD_KEY_ENTER || c == ' ') launch_index(s, s->selected);
}

static void core_click(window_t* w, int x, int y) {
    core_t* s = w->user_data;
    int total = desktop_icon_count();
    int grid_w = w->width - (CORE_SIDE_W + CORE_SIDE_PAD) - CORE_GRID_X;
    int cw = (grid_w - CORE_GAP * (CORE_COLS - 1)) / CORE_COLS;
    if (cw < 160) cw = 160;

    int py = CORE_GRID_Y + CORE_ROWS * CORE_ROW_STEP + 6;
    if (py > w->height - 96) py = w->height - 96;
    if (y >= py && y < py + 40) {
        if (x >= CORE_GRID_X && x < CORE_GRID_X + 110) { go_page(s, s->page - 1); return; }
        if (x >= CORE_GRID_X + 126 && x < CORE_GRID_X + 236) { go_page(s, s->page + 1); return; }
    }

    // «check for prisms» в правой колонке.
    int sx = w->width - (CORE_SIDE_W + 40), bar_w = CORE_SIDE_W - 52;
    if (x >= sx + 26 && x < sx + 26 + bar_w &&
        y >= CORE_GRID_Y + 444 && y < CORE_GRID_Y + 490) {
        se_copy(s->status, "all prisms are up to date", 64);
        return;
    }

    if (y < CORE_GRID_Y || x < CORE_GRID_X) return;
    int col = (x - CORE_GRID_X) / (cw + CORE_GAP);
    int row = (y - CORE_GRID_Y) / CORE_ROW_STEP;
    if (col < 0 || col >= CORE_COLS || row < 0 || row >= CORE_ROWS) return;
    // Промахи по зазорам между карточками не считаем.
    if ((x - CORE_GRID_X) - col * (cw + CORE_GAP) > cw) return;
    if ((y - CORE_GRID_Y) - row * CORE_ROW_STEP > CORE_CARD_H) return;

    int index = s->page * CORE_PER_PAGE + row * CORE_COLS + col;
    if (index >= total) return;
    s->selected = index;
    launch_index(s, index);
}

window_t* app_core_open(void) {
    core_t* s = kmalloc(sizeof(*s));
    if (!s) return 0;
    s->selected = 0;
    s->page = 0;
    s->confirm = -1;
    se_copy(s->status, "arrows choose - enter opens - 1..9 quick launch - esc returns", 64);
    restart_anim(s);
    window_t* w = se_workspace("Core", core_draw, core_click, core_key, s, false);
    if (!w) { kfree(s); return 0; }
    return w;
}
