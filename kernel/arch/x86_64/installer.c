// ============================================================================
// installer.c — графический установщик AMPLIFY-OS x86_64 (этап 57).
// ----------------------------------------------------------------------------
// Шесть экранов по макету: WELCOME / LICENSE / DESTINATION / OPTIONS /
// INSTALL / COMPLETE.  Слева — неподвижная навигация с нумерованными шагами,
// справа — контент на перламутровом холсте.  Управление мышью и клавиатурой.
//
// Геометрия снята с макета (2000x1250 px) и переведена в логическую сетку
// 1600x1000 (коэффициент 0.8), поверх которой работает ui_layout.  Два
// отличия от обычного ui_ux():
//   * сайдбар прижат к ЛЕВОМУ КРАЮ ЭКРАНА (макет 16:10, а экран может быть
//     16:9 — иначе слева осталась бы полоса холста);
//   * контент центрирован в оставшейся области — это ровно то, что даёт
//     ui_ux() с его горизонтальным смещением (см. комментарий к CENTER_X).
//
// Вся работа с железом — в installer_engine.c; здесь только кадры и ввод.
// Тексты интерфейса — английские, как в макете.
// ============================================================================
#include "installer.h"
#include "installer_engine.h"

#include "graphics.h"
#include "doublebuf.h"
#include "ui_theme.h"
#include "ui_layout.h"
#include "ui_icons.h"
#include "ui_font.h"
#include "keyboard.h"
#include "mouse.h"
#include "cursor.h"
#include "timer.h"
#include "serial.h"
#include "heap.h"
#include "power.h"

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Геометрия (логическая сетка 1600x1000).
// ----------------------------------------------------------------------------
#define SIDEBAR_W    244
// Центр контентной области.  (244 + 1600) / 2 = 922; на экране любой
// пропорции ui_ux(922) совпадает с серединой между правым краем сайдбара
// и правым краем экрана, потому что ui_ux добавляет симметричный отступ.
#define CENTER_X     922
#define CONTENT_X    322
#define CONTENT_W    1200
#define FOOTER_Y     872
#define BTN_W        184
#define BTN_H        52

// Цвета градиента кнопок и индикатора — как в макете (тёплый оранжевый →
// приглушённая ягода), без магентового хвоста ui_sunset().
#define GRAD_FROM    0xFFE69D40u
#define GRAD_TO      0xFFC45A79u

typedef enum {
    SCR_WELCOME = 0,
    SCR_LICENSE,
    SCR_DESTINATION,
    SCR_OPTIONS,
    SCR_INSTALL,
    SCR_COMPLETE,
    SCR_COUNT
} screen_t;

static const char* k_nav_num[SCR_COUNT]  = { "01", "02", "03", "04", "05", "06" };
static const char* k_nav_name[SCR_COUNT] = { "WELCOME", "LICENSE", "DESTINATION",
                                             "OPTIONS", "INSTALL", "COMPLETE" };

// Фокусируемые элементы экрана OPTIONS (Tab / Shift+Tab / стрелки).
enum {
    FOCUS_HOSTNAME = 0,
    FOCUS_USERNAME,
    FOCUS_PASSWORD,
    FOCUS_CONFIRM,
    FOCUS_VAULT,
    FOCUS_DARK,
    FOCUS_CRYPT,        // [этап 48] шифрование системного раздела
    FOCUS_COUNT
};

#define LOG_LINES 8

typedef struct {
    screen_t screen;
    int      frame;

    bool     license_ok;
    int      sel_target;
    int      focus;
    bool     vault;
    bool     dark;
    bool     encrypt;          // [этап 48] LUKS1 на системном разделе
    bool     show_errors;      // подсвечивать ошибки только после попытки продолжить
    bool     confirm;          // открыт диалог «стереть диск?»

    char     hostname[INSTALL_HOSTNAME_MAX];
    char     username[INSTALL_USER_MAX];
    char     passphrase[INSTALL_PASS_MAX];
    char     confirm_pass[INSTALL_PASS_MAX];

    // Прогресс установки.
    install_step_t step;
    int            step_pct;
    int            overall_pct;
    char           message[96];
    char           log[LOG_LINES][72];
    int            log_count;
    bool           running;
    bool           finished;
    bool           failed;
    uint64_t       started_ms;
    uint64_t       elapsed_ms;
} ui_state_t;

static ui_state_t g_ui;
static bool       g_live = false;

// ----------------------------------------------------------------------------
// Строковые мелочи.
// ----------------------------------------------------------------------------
static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void s_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool s_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int s_append(char* buf, int pos, int cap, const char* s) {
    for (int i = 0; s && s[i] && pos < cap - 1; i++) buf[pos++] = s[i];
    buf[pos] = 0;
    return pos;
}

static int s_append_u64(char* buf, int pos, int cap, uint64_t v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && pos < cap - 1) buf[pos++] = tmp[--n];
    buf[pos] = 0;
    return pos;
}

// Размер из мегабайт: "256 MB", "3.7 GB", ровные гигабайты — без ".0".
static int s_append_size(char* buf, int pos, int cap, uint64_t mb) {
    if (mb < 1024) {
        pos = s_append_u64(buf, pos, cap, mb);
        return s_append(buf, pos, cap, " MB");
    }
    uint64_t gb    = mb / 1024;
    uint64_t tenth = ((mb % 1024) * 10) / 1024;
    pos = s_append_u64(buf, pos, cap, gb);
    if (tenth) {
        pos = s_append(buf, pos, cap, ".");
        pos = s_append_u64(buf, pos, cap, tenth);
    }
    return s_append(buf, pos, cap, " GB");
}

static bool token_present(const char* cmdline, const char* token) {
    if (!cmdline) return false;
    int tl = s_len(token);
    for (int i = 0; cmdline[i]; i++) {
        if (i != 0 && cmdline[i - 1] != ' ') continue;
        int k = 0;
        while (k < tl && cmdline[i + k] == token[k]) k++;
        if (k == tl && (cmdline[i + k] == 0 || cmdline[i + k] == ' ')) return true;
    }
    return false;
}

// Номер сборки YYYYMMDD из __DATE__ ("Sep 26 2026") — в сайдбаре макета
// стоит именно дата сборки, поэтому не хардкодим её.
static const char* build_stamp(void) {
    static char out[9] = { 0 };
    if (out[0]) return out;
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* d = __DATE__;
    int m = 1;
    for (int i = 0; i < 12; i++)
        if (months[i*3] == d[0] && months[i*3+1] == d[1] && months[i*3+2] == d[2]) m = i + 1;
    out[0] = d[7]; out[1] = d[8]; out[2] = d[9]; out[3] = d[10];
    out[4] = (char)('0' + m / 10); out[5] = (char)('0' + m % 10);
    out[6] = (d[4] == ' ') ? '0' : d[4]; out[7] = d[5];
    out[8] = 0;
    return out;
}

// ----------------------------------------------------------------------------
// Палитра поверх темы.
// ----------------------------------------------------------------------------
static uint32_t c_soft(void)  { const ui_runtime_theme_t* th = ui_theme_current(); return ui_lerp(th->muted, th->canvas, 50); }
static uint32_t c_faint(void) { const ui_runtime_theme_t* th = ui_theme_current(); return ui_lerp(th->muted, th->canvas, 150); }
static uint32_t c_side(void)  { const ui_runtime_theme_t* th = ui_theme_current(); return ui_lerp(th->surface, th->canvas, 110); }
static uint32_t c_grad(int i, int n) { return ui_lerp(GRAD_FROM, GRAD_TO, (uint8_t)(n > 1 ? (i * 255) / (n - 1) : 0)); }

// ----------------------------------------------------------------------------
// Примитивы: контент (ui_ux — центрированная сетка) и сайдбар (прижат влево).
// ----------------------------------------------------------------------------
static int sb_x(int x) { return ui_uw(x); }          // без горизонтального отступа

static void fill(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}

static void frame_rect(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_stroke_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}

// Горизонтальный градиент макета, острые углы.
static void grad_rect(const gfx_target_t* t, int x, int y, int w, int h) {
    int sx = ui_ux(x), sy = ui_uy(y), sw = ui_uw(w), sh = ui_uh(h);
    for (int i = 0; i < sw; i++) gfx_fill_rect(t, sx + i, sy, 1, sh, c_grad(i, sw));
}

// Текст по базовой линии (логические координаты).
static void txt(const gfx_target_t* t, int x, int base, const char* s, uint32_t c,
                ui_font_id_t f, int tracking, bool bold) {
    ui_font_draw_baseline_ex(t, ui_ux(x), ui_uy(base), s, c, f, tracking, bold);
}

static void stxt(const gfx_target_t* t, int x, int base, const char* s, uint32_t c,
                 ui_font_id_t f, int tracking, bool bold) {
    ui_font_draw_baseline_ex(t, sb_x(x), ui_uy(base), s, c, f, tracking, bold);
}

static int text_px(const char* s, ui_font_id_t f, int tracking, bool bold) {
    return ui_font_text_width_tracked(s, f, tracking) + (bold ? 1 : 0);
}

// Центрирование в экранных пикселях — шрифт не масштабируется сеткой.
static void ctxt(const gfx_target_t* t, int cx, int base, const char* s, uint32_t c,
                 ui_font_id_t f, int tracking, bool bold) {
    int w = text_px(s, f, tracking, bold);
    ui_font_draw_baseline_ex(t, ui_ux(cx) - w / 2, ui_uy(base), s, c, f, tracking, bold);
}

// Ширина строки в логических единицах.
static int logical_w(int px) {
    const ui_layout_t* l = ui_layout_get();
    return l->scale_fp > 0 ? (int)(((int64_t)px << 16) / l->scale_fp) : px;
}

// Вертикальная середина строки для кнопок: базовая линия так, чтобы
// заглавные буквы стояли по центру прямоугольника.
static int button_base(int y, int h, ui_font_id_t f) {
    const ui_layout_t* l = ui_layout_get();
    int cap_logical = l->scale_fp > 0
                    ? (int)(((int64_t)ui_font_cap_height(f) << 16) / l->scale_fp) : 10;
    return y + (h + cap_logical) / 2;
}

// ----------------------------------------------------------------------------
// Крупный заголовок.  Самый большой атлас — 44 px, а в макете «AMPLIFY-OS»
// примерно в 1.3 раза крупнее и заметно жирнее.  Рисуем строку белым по
// чёрному во временный буфер (канал = покрытие глифа), затем масштабируем
// маску билинейно и накладываем нужным цветом.  Никакой аллокации: буфер
// статический, ~300 КиБ в .bss.
// ----------------------------------------------------------------------------
#define TXT_BUF_W 768
#define TXT_BUF_H 96
// [38.1.0] Из кучи при первом заголовке (установщик — только на live-носителе),
// а не 288 КБ в .bss каждого запуска ядра (CLAUDE.md §5.50).
static uint32_t* g_txt_px;

static void draw_display_text(const gfx_target_t* t, int cx_screen, int base_screen,
                              const char* s, uint32_t color, int cap_px_target) {
    const ui_font_id_t f = UI_FONT_44;
    const int tracking = -1;
    int w = text_px(s, f, tracking, true) + 4;
    int h = ui_font_line_height(f) + 4;
    int asc = ui_font_ascent(f);
    if (w > TXT_BUF_W) w = TXT_BUF_W;
    if (h > TXT_BUF_H) h = TXT_BUF_H;

    if (!g_txt_px) g_txt_px = (uint32_t*)kmalloc((size_t)TXT_BUF_W * TXT_BUF_H * 4);
    if (!g_txt_px) return;                  // без памяти — экран без заголовка

    for (int i = 0; i < TXT_BUF_W * h; i++) g_txt_px[i] = 0xFF000000u;
    gfx_target_t tmp = { g_txt_px, w, h, TXT_BUF_W * 4 };
    // Два прохода со сдвигом на пиксель + штатный bold — «Bold» гарнитуры
    // макета заметно тяжелее одиночного bold-смаза DejaVu.
    ui_font_draw_baseline_ex(&tmp, 1, asc + 2, s, 0xFFFFFFFFu, f, tracking, true);
    ui_font_draw_baseline_ex(&tmp, 2, asc + 2, s, 0xFFFFFFFFu, f, tracking, true);

    int cap = ui_font_cap_height(f);
    if (cap <= 0) cap = 32;
    int scale_q8 = (cap_px_target * 256) / cap;          // 256 = 1.0
    if (scale_q8 < 256) scale_q8 = 256;

    int dw = (w * scale_q8) >> 8;
    int dh = (h * scale_q8) >> 8;
    int dx0 = cx_screen - dw / 2;
    int dy0 = base_screen - (((asc + 2) * scale_q8) >> 8);
    int inv = (1 << 24) / scale_q8;                       // шаг источника, 16.16
    uint32_t rgb = color & 0x00FFFFFFu;

    for (int dy = 0; dy < dh; dy++) {
        int py = dy0 + dy;
        if (py < 0 || py >= t->height) continue;
        int fy = ((dy * 2 + 1) * inv) / 2 - 32768;
        if (fy < 0) fy = 0;
        int sy = fy >> 16, wy = (fy >> 8) & 0xFF;
        if (sy >= h - 1) { sy = h - 2; wy = 255; }
        const uint32_t* r0 = &g_txt_px[sy * TXT_BUF_W];
        const uint32_t* r1 = r0 + TXT_BUF_W;
        uint32_t* dst = (uint32_t*)((uint8_t*)t->pixels + (size_t)py * (size_t)t->pitch);
        for (int dx = 0; dx < dw; dx++) {
            int px = dx0 + dx;
            if (px < 0 || px >= t->width) continue;
            int fx = ((dx * 2 + 1) * inv) / 2 - 32768;
            if (fx < 0) fx = 0;
            int sx = fx >> 16, wx = (fx >> 8) & 0xFF;
            if (sx >= w - 1) { sx = w - 2; wx = 255; }
            int a00 = r0[sx] & 0xFF, a01 = r0[sx + 1] & 0xFF;
            int a10 = r1[sx] & 0xFF, a11 = r1[sx + 1] & 0xFF;
            int top = a00 * (256 - wx) + a01 * wx;
            int bot = a10 * (256 - wx) + a11 * wx;
            int a = (top * (256 - wy) + bot * wy) >> 16;
            if (a <= 0) continue;
            if (a > 255) a = 255;
            dst[px] = gfx_blend(dst[px], ((uint32_t)a << 24) | rgb);
        }
    }
}

// Высота заглавной буквы в экранных пикселях для логической величины.
static int cap_px(int logical) { return ui_uh(logical); }

// ----------------------------------------------------------------------------
// «CHRT-OS»: «CHRT-» чернилами, «OS» — ягодным акцентом, как в макете.
// ----------------------------------------------------------------------------
static void draw_chrt_os(const gfx_target_t* t, int sx, int sbase, ui_font_id_t f,
                         int tracking, bool bold) {
    const ui_runtime_theme_t* th = ui_theme_current();
    int full = text_px("CHRT-OS", f, tracking, bold);
    int os   = text_px("OS", f, tracking, bold);
    ui_font_draw_baseline_ex(t, sx, sbase, "CHRT-", th->ink, f, tracking, bold);
    ui_font_draw_baseline_ex(t, sx + full - os, sbase, "OS", th->accent_berry, f, tracking, bold);
}

// ----------------------------------------------------------------------------
// Кнопки.
// ----------------------------------------------------------------------------
typedef struct { bool click; int x, y; } pointer_t;
static pointer_t g_ptr;

static bool hovered(int x, int y, int w, int h) {
    return ui_hit_logical(g_ptr.x, g_ptr.y, x, y, w, h);
}

static void draw_primary_button(const gfx_target_t* t, int x, int y, int w, int h,
                                const char* label, bool enabled) {
    const ui_runtime_theme_t* th = ui_theme_current();
    if (enabled) {
        grad_rect(t, x, y, w, h);
        if (hovered(x, y, w, h)) fill(t, x, y + h - 3, w, 3, ui_darken(GRAD_TO, 30));
    } else {
        fill(t, x, y, w, h, th->surface_soft);
        frame_rect(t, x, y, w, h, th->border);
    }
    uint32_t fg = enabled ? th->on_accent : c_soft();
    int wpx = text_px(label, UI_FONT_14, 3, true);
    ui_font_draw_baseline_ex(t, ui_ux(x) + (ui_uw(w) - wpx) / 2,
                             ui_uy(button_base(y, h, UI_FONT_14)),
                             label, fg, UI_FONT_14, 3, true);
}

static void draw_ghost_button(const gfx_target_t* t, int x, int y, int w, int h,
                              const char* label) {
    const ui_runtime_theme_t* th = ui_theme_current();
    bool hv = hovered(x, y, w, h);
    fill(t, x, y, w, h, th->surface);
    frame_rect(t, x, y, w, h, hv ? th->accent_berry : th->border);
    int wpx = text_px(label, UI_FONT_14, 3, true);
    ui_font_draw_baseline_ex(t, ui_ux(x) + (ui_uw(w) - wpx) / 2,
                             ui_uy(button_base(y, h, UI_FONT_14)),
                             label, th->ink, UI_FONT_14, 3, true);
}

// ----------------------------------------------------------------------------
// Боковая навигация (прижата к левому краю экрана).
// ----------------------------------------------------------------------------
static void draw_sidebar(const gfx_target_t* t, screen_t active) {
    const ui_runtime_theme_t* th = ui_theme_current();
    int right = sb_x(SIDEBAR_W);

    gfx_fill_rect(t, 0, 0, right, t->height, c_side());
    gfx_fill_rect(t, right - 1, 0, 1, t->height, th->border);

    // Логотип и название.
    ui_draw_bitmap_scaled(t, sb_x(31), ui_uy(36), ui_uw(47), ui_uh(46), ui_logo_big());
    stxt(t, 27, 127, "AMPLIFY-OS", th->ink, UI_FONT_20, -1, true);
    draw_chrt_os(t, sb_x(27), ui_uy(147), UI_FONT_12, 1, true);

    gfx_fill_rect(t, sb_x(22), ui_uy(176), ui_uw(199), 1, th->border);

    // Шаги.
    for (int i = 0; i < SCR_COUNT; i++) {
        int base = 227 + i * 43;
        bool is_active = (i == (int)active);
        bool is_done   = (i < (int)active);

        if (is_active) {
            // Вертикальный градиентный маркер у самого края экрана.
            int y0 = ui_uy(base - 23), hh = ui_uh(40);
            for (int r = 0; r < hh; r++)
                gfx_fill_rect(t, 0, y0 + r, ui_uw(3), 1, c_grad(r, hh));
        }
        uint32_t num_c = is_active ? th->accent_berry
                       : (is_done  ? th->accent_coral : c_soft());
        uint32_t lbl_c = is_active ? th->ink : c_soft();
        stxt(t, 30, base, k_nav_num[i], num_c, UI_FONT_MONO_14, 0, false);
        stxt(t, 58, base, k_nav_name[i], lbl_c, UI_FONT_12, 2, is_active);
    }

    // Версия внизу.
    gfx_fill_rect(t, sb_x(26), ui_uy(918), ui_uw(190), 1, th->border);
    static char ver[48];
    int p = 0;
    p = s_append(ver, p, (int)sizeof(ver), "v1.0.0 \xB7 build ");
    p = s_append(ver, p, (int)sizeof(ver), build_stamp());
    stxt(t, 27, 950, ver, c_soft(), UI_FONT_MONO_14, 0, false);
    stxt(t, 27, 970, "x86_64 \xB7 UEFI", c_faint(), UI_FONT_MONO_14, 0, false);
}

// ----------------------------------------------------------------------------
// Общий заголовок контентных экранов.
// ----------------------------------------------------------------------------
static void draw_heading(const gfx_target_t* t, const char* kicker,
                         const char* title, const char* subtitle) {
    const ui_runtime_theme_t* th = ui_theme_current();
    txt(t, CONTENT_X, 132, kicker, th->accent_berry, UI_FONT_MONO_14, 2, false);
    txt(t, CONTENT_X, 180, title, th->ink, UI_FONT_28, 0, true);
    if (subtitle) txt(t, CONTENT_X, 214, subtitle, c_soft(), UI_FONT_16, 0, false);
    fill(t, CONTENT_X, 240, CONTENT_W, 1, th->border);
}

// ----------------------------------------------------------------------------
// Экран 01 — WELCOME (по макету).
// ----------------------------------------------------------------------------
static void draw_welcome(const gfx_target_t* t) {
    const ui_runtime_theme_t* th = ui_theme_current();

    ui_draw_bitmap_scaled(t, ui_ux(CENTER_X - 53), ui_uy(236),
                          ui_uw(106), ui_uh(104), ui_logo_big());

    draw_display_text(t, ui_ux(CENTER_X), ui_uy(443), "AMPLIFY-OS", th->ink, cap_px(37));

    int w = text_px("CHRT-OS", UI_FONT_20, 1, true);
    draw_chrt_os(t, ui_ux(CENTER_X) - w / 2, ui_uy(479), UI_FONT_20, 1, true);
    ctxt(t, CENTER_X, 507, "COMING OUT 2025", th->accent_berry, UI_FONT_12, 4, false);

    ctxt(t, CENTER_X, 573, "Welcome to the AMPLIFY-OS installer. This guided process will",
         c_soft(), UI_FONT_16, 0, false);
    ctxt(t, CENTER_X, 599, "prepare your system in a few minutes. Make sure your device is",
         c_soft(), UI_FONT_16, 0, false);
    ctxt(t, CENTER_X, 624, "connected to power.", c_soft(), UI_FONT_16, 0, false);

    draw_primary_button(t, CENTER_X - 138, 677, 276, BTN_H, "BEGIN INSTALLATION", true);

    static char hint[80];
    int p = 0;
    p = s_append(hint, p, (int)sizeof(hint), "Requires ");
    p = s_append_size(hint, p, (int)sizeof(hint), install_required_mb());
    p = s_append(hint, p, (int)sizeof(hint), " available disk space");
    ctxt(t, CENTER_X, 766, hint, c_soft(), UI_FONT_MONO_14, 0, false);
}

// ----------------------------------------------------------------------------
// Экран 02 — LICENSE.
// ----------------------------------------------------------------------------
static const char* k_license[] = {
    "AMPLIFY-OS is an experimental operating system published for research,",
    "education and personal experimentation. It is provided as is, without",
    "warranty of any kind, express or implied.",
    "",
    "The installer writes a new GUID partition table to the drive you select.",
    "Every partition and every file already present on that drive will be",
    "permanently destroyed. Back up anything you care about before you continue.",
    "",
    "You keep ownership of everything you create with this system. The kernel,",
    "the shell and the bundled applications may be studied, modified and",
    "redistributed as long as this notice travels with them.",
    "",
    "By accepting you confirm that you understand the destructive nature of",
    "the installation and that you install AMPLIFY-OS at your own risk."
};

static void draw_checkbox(const gfx_target_t* t, int x, int y, bool on) {
    const ui_runtime_theme_t* th = ui_theme_current();
    if (on) {
        grad_rect(t, x, y, 22, 22);
        // Галочка из двух штрихов.
        for (int i = 0; i < 5; i++) fill(t, x + 5 + i, y + 10 + i, 2, 2, th->on_accent);
        for (int i = 0; i < 9; i++) fill(t, x + 9 + i, y + 14 - i, 2, 2, th->on_accent);
    } else {
        fill(t, x, y, 22, 22, th->surface);
        frame_rect(t, x, y, 22, 22, hovered(x, y, 22, 22) ? th->accent_berry : th->border);
    }
}

static void draw_license(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    draw_heading(t, "STEP 02", "License agreement",
                 "Read the terms before the installer touches your drive.");

    fill(t, CONTENT_X, 272, CONTENT_W, 452, th->surface);
    frame_rect(t, CONTENT_X, 272, CONTENT_W, 452, th->border);

    int lines = (int)(sizeof(k_license) / sizeof(k_license[0]));
    int base = 316;
    for (int i = 0; i < lines; i++) {
        if (k_license[i][0]) txt(t, CONTENT_X + 40, base, k_license[i], th->muted, UI_FONT_16, 0, false);
        base += 28;
    }

    draw_checkbox(t, CONTENT_X, 758, st->license_ok);
    txt(t, CONTENT_X + 40, 775, "I accept the terms and understand that the target drive will be erased",
        th->ink, UI_FONT_16, 0, false);
    txt(t, CONTENT_X + 40, 800, "Space toggles the checkbox", c_faint(), UI_FONT_12, 0, false);
}

// ----------------------------------------------------------------------------
// Экран 03 — DESTINATION.
// ----------------------------------------------------------------------------
static void draw_layout_preview(const gfx_target_t* t, int x, int y, int w,
                                const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    const install_target_t* tgt = install_target_at(st->sel_target);
    if (!tgt) return;

    uint64_t esp_mb = 0, sys_mb = 0, vault_mb = 0;
    install_preview_layout(tgt->dev, st->vault, &esp_mb, &sys_mb, &vault_mb);
    uint64_t total = esp_mb + sys_mb + vault_mb;
    if (!total) return;

    int esp_w   = (int)((esp_mb * (uint64_t)w) / total);
    int sys_w   = (int)((sys_mb * (uint64_t)w) / total);
    if (esp_w < 6) esp_w = 6;
    int vault_w = w - esp_w - sys_w;
    if (vault_w < 0) vault_w = 0;
    if (!vault_mb) sys_w = w - esp_w;

    fill(t, x, y, esp_w, 14, GRAD_FROM);
    fill(t, x + esp_w, y, sys_w, 14, ui_lerp(GRAD_FROM, GRAD_TO, 140));
    if (vault_mb) fill(t, x + esp_w + sys_w, y, vault_w, 14, GRAD_TO);
    fill(t, x + esp_w, y, 2, 14, th->canvas);
    if (vault_mb) fill(t, x + esp_w + sys_w, y, 2, 14, th->canvas);

    static char line[128];
    int p = 0;
    p = s_append(line, p, (int)sizeof(line), "BOOT (EFI) ");
    p = s_append_size(line, p, (int)sizeof(line), esp_mb);
    p = s_append(line, p, (int)sizeof(line), "   \xB7   SYSTEM ");
    p = s_append_size(line, p, (int)sizeof(line), sys_mb);
    if (vault_mb) {
        p = s_append(line, p, (int)sizeof(line), "   \xB7   VAULT ");
        p = s_append_size(line, p, (int)sizeof(line), vault_mb);
    }
    txt(t, x, y + 42, line, c_soft(), UI_FONT_MONO_14, 0, false);
}

static void draw_destination(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    draw_heading(t, "STEP 03", "Choose a destination",
                 "AMPLIFY-OS takes the whole drive: a UEFI boot partition and a system partition.");

    int count = install_target_count();
    if (count == 0) {
        fill(t, CONTENT_X, 280, CONTENT_W, 132, th->surface);
        frame_rect(t, CONTENT_X, 280, CONTENT_W, 132, th->border);
        txt(t, CONTENT_X + 40, 330, "No suitable drive was found.", th->ink, UI_FONT_20, 0, true);
        static char need[96];
        int p = 0;
        p = s_append(need, p, (int)sizeof(need), "Attach an IDE/ATA drive of at least ");
        p = s_append_size(need, p, (int)sizeof(need), install_required_mb());
        p = s_append(need, p, (int)sizeof(need), " and press R to rescan.");
        txt(t, CONTENT_X + 40, 366, need, c_soft(), UI_FONT_16, 0, false);
        draw_ghost_button(t, CONTENT_X + CONTENT_W - 40 - BTN_W, 320, BTN_W, BTN_H, "RESCAN");
        return;
    }

    int y = 272;
    for (int i = 0; i < count; i++) {
        const install_target_t* tgt = install_target_at(i);
        bool sel = (i == st->sel_target);
        bool hv  = hovered(CONTENT_X, y, CONTENT_W, 104);

        fill(t, CONTENT_X, y, CONTENT_W, 104, sel ? th->surface : c_side());
        frame_rect(t, CONTENT_X, y, CONTENT_W, 104,
                   sel ? th->accent_berry : (hv ? ui_lerp(th->border, th->accent_berry, 90) : th->border));
        if (sel) {
            int y0 = ui_uy(y), hh = ui_uh(104);
            for (int r = 0; r < hh; r++)
                gfx_fill_rect(t, ui_ux(CONTENT_X), y0 + r, ui_uw(4), 1, c_grad(r, hh));
        }

        // Радио-маркер (квадратный — острые углы, как в макете).
        frame_rect(t, CONTENT_X + 36, y + 43, 18, 18, sel ? th->accent_berry : th->border);
        if (sel) fill(t, CONTENT_X + 40, y + 47, 10, 10, th->accent_berry);

        txt(t, CONTENT_X + 80, y + 44, tgt->model, th->ink, UI_FONT_20, 0, true);

        static char sub[128];
        int p = 0;
        p = s_append(sub, p, (int)sizeof(sub), tgt->bus ? tgt->bus : "ATA");
        p = s_append(sub, p, (int)sizeof(sub), "  \xB7  ");
        p = s_append_size(sub, p, (int)sizeof(sub), tgt->size_mb);
        if (tgt->amplify)             p = s_append(sub, p, (int)sizeof(sub), "  \xB7  AMPLIFY-OS already installed");
        else if (tgt->has_partitions) p = s_append(sub, p, (int)sizeof(sub), "  \xB7  contains partitions");
        else                          p = s_append(sub, p, (int)sizeof(sub), "  \xB7  empty");
        txt(t, CONTENT_X + 80, y + 74, sub, c_soft(), UI_FONT_MONO_14, 0, false);

        if (tgt->has_partitions) {
            const char* warn = "ALL DATA WILL BE ERASED";
            int wl = logical_w(text_px(warn, UI_FONT_12, 2, true));
            txt(t, CONTENT_X + CONTENT_W - 40 - wl, y + 58, warn, th->accent_coral, UI_FONT_12, 2, true);
        }
        y += 118;
    }

    txt(t, CONTENT_X, 700, "LAYOUT PREVIEW", c_soft(), UI_FONT_12, 3, true);
    draw_layout_preview(t, CONTENT_X, 718, CONTENT_W, st);
}

// ----------------------------------------------------------------------------
// Экран 04 — OPTIONS.
// ----------------------------------------------------------------------------
// Правила полей повторяют users.c / UTS: иначе установщик создал бы
// учётную запись, которую экран входа не сможет разобрать.
static bool host_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}
static bool user_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

// Первая ошибка ввода или 0, если всё в порядке.
static const char* options_error(const ui_state_t* st) {
    int hl = s_len(st->hostname);
    if (hl == 0) return "Enter a hostname for this machine.";
    if (st->hostname[0] == '-' || st->hostname[hl - 1] == '-')
        return "The hostname cannot start or end with a hyphen.";
    if (st->username[0] == 0) return "Enter a name for the administrator account.";
    if (s_len(st->passphrase) < 4) return "The passphrase needs at least 4 characters.";
    // Пароль шифрованного тома перебирают офлайн по образу диска — короткий
    // пароль там не спасают никакие итерации PBKDF2.
    if (st->encrypt && s_len(st->passphrase) < 8)
        return "An encrypted drive needs a passphrase of at least 8 characters.";
    if (!s_eq(st->passphrase, st->confirm_pass)) return "The passphrases do not match.";
    return 0;
}

static void draw_field(const gfx_target_t* t, int x, int y, int w, const char* label,
                       const char* value, bool focused, bool mask, bool invalid, int frame) {
    const ui_runtime_theme_t* th = ui_theme_current();

    txt(t, x, y + 12, label, c_soft(), UI_FONT_12, 3, true);
    fill(t, x, y + 22, w, 46, th->surface);
    uint32_t border = focused ? th->accent_berry
                    : (invalid ? th->accent_coral
                    : (hovered(x, y + 22, w, 46) ? ui_lerp(th->border, th->accent_berry, 90) : th->border));
    frame_rect(t, x, y + 22, w, 46, border);
    if (focused) fill(t, x, y + 22 + 44, w, 2, th->accent_berry);

    static char shown[64];
    if (mask) {
        int n = s_len(value);
        if (n > 63) n = 63;
        for (int i = 0; i < n; i++) shown[i] = (char)0x95;      // «•» в CP1251
        shown[n] = 0;
    } else {
        s_copy(shown, value, (int)sizeof(shown));
    }
    txt(t, x + 16, y + 51, shown, th->ink, UI_FONT_16, 0, false);

    if (focused && ((frame / 12) & 1) == 0) {
        int wl = logical_w(ui_font_text_width(shown, UI_FONT_16));
        fill(t, x + 17 + wl, y + 34, 2, 22, th->accent_berry);
    }
}

static void draw_toggle(const gfx_target_t* t, int x, int y, const char* title,
                        const char* note, bool on, bool focused) {
    const ui_runtime_theme_t* th = ui_theme_current();

    if (on) grad_rect(t, x, y, 52, 26);
    else    { fill(t, x, y, 52, 26, th->surface_soft); frame_rect(t, x, y, 52, 26, th->border); }
    fill(t, on ? x + 29 : x + 4, y + 4, 19, 18, on ? th->on_accent : c_soft());
    if (focused) frame_rect(t, x - 4, y - 4, 60, 34, th->accent_berry);

    txt(t, x + 72, y + 12, title, th->ink, UI_FONT_16, 0, true);
    txt(t, x + 72, y + 34, note, c_soft(), UI_FONT_14, 0, false);
}

#define OPT_FIELD_W   520
#define OPT_FIELD_Y0  266
#define OPT_FIELD_DY  88
#define OPT_RIGHT_X   (CONTENT_X + 660)
#define OPT_VAULT_Y   300
#define OPT_DARK_Y    372
#define OPT_CRYPT_Y   444

static void draw_options(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    draw_heading(t, "STEP 04", "System options",
                 "Name this machine and create the first identity.");

    static const char* labels[4] = { "HOSTNAME", "ADMINISTRATOR", "PASSPHRASE", "CONFIRM PASSPHRASE" };
    const char* values[4] = { st->hostname, st->username, st->passphrase, st->confirm_pass };
    const char* err = options_error(st);
    for (int i = 0; i < 4; i++) {
        bool invalid = st->show_errors && err &&
                       ((i == 0 && (!st->hostname[0] || st->hostname[0] == '-' ||
                                    st->hostname[s_len(st->hostname) - 1] == '-')) ||
                        (i == 1 && !st->username[0]) ||
                        (i == 2 && s_len(st->passphrase) < (st->encrypt ? 8 : 4)) ||
                        (i == 3 && !s_eq(st->passphrase, st->confirm_pass)));
        draw_field(t, CONTENT_X, OPT_FIELD_Y0 + i * OPT_FIELD_DY, OPT_FIELD_W, labels[i], values[i],
                   st->focus == i, i >= 2, invalid, st->frame);
    }
    if (st->show_errors && err)
        txt(t, CONTENT_X, OPT_FIELD_Y0 + 4 * OPT_FIELD_DY + 14, err, th->accent_coral, UI_FONT_14, 0, true);

    int rx = OPT_RIGHT_X;
    txt(t, rx, 278, "PREFERENCES", c_soft(), UI_FONT_12, 3, true);
    draw_toggle(t, rx, OPT_VAULT_Y, "Separate vault partition",
                "Keeps personal files on their own volume", st->vault, st->focus == FOCUS_VAULT);
    draw_toggle(t, rx, OPT_DARK_Y, "Dark appearance",
                "Start the desktop with the dark Pearl theme", st->dark, st->focus == FOCUS_DARK);
    draw_toggle(t, rx, OPT_CRYPT_Y, "Encrypt the drive",
                "LUKS1 \xB7 AES-256-XTS \xB7 unlocked with the passphrase at boot",
                st->encrypt, st->focus == FOCUS_CRYPT);

    fill(t, rx, 528, 540, 1, th->border);
    txt(t, rx, 562, "SUMMARY", c_soft(), UI_FONT_12, 3, true);

    const install_target_t* tgt = install_target_at(st->sel_target);
    static char line[128];
    int p = 0;
    p = s_append(line, p, (int)sizeof(line), tgt ? tgt->model : "No drive");
    p = s_append(line, p, (int)sizeof(line), "  \xB7  ");
    p = s_append_size(line, p, (int)sizeof(line), tgt ? tgt->size_mb : 0);
    txt(t, rx, 594, line, th->ink, UI_FONT_16, 0, true);

    uint64_t esp_mb = 0, sys_mb = 0, vault_mb = 0;
    if (tgt) install_preview_layout(tgt->dev, st->vault, &esp_mb, &sys_mb, &vault_mb);
    p = 0;
    p = s_append(line, p, (int)sizeof(line), "Boot ");
    p = s_append_size(line, p, (int)sizeof(line), esp_mb);
    p = s_append(line, p, (int)sizeof(line), "  \xB7  System ");
    p = s_append_size(line, p, (int)sizeof(line), sys_mb);
    if (vault_mb) {
        p = s_append(line, p, (int)sizeof(line), "  \xB7  Vault ");
        p = s_append_size(line, p, (int)sizeof(line), vault_mb);
    }
    txt(t, rx, 622, line, c_soft(), UI_FONT_14, 0, false);
    if (st->vault && !vault_mb)
        txt(t, rx, 646, "The drive is too small for a separate vault partition.",
            th->accent_coral, UI_FONT_14, 0, false);
    txt(t, rx, 674, st->encrypt ? "UEFI firmware \xB7 GPT \xB7 LUKS1 \xB7 FAT32 inside"
                                : "UEFI firmware \xB7 GPT \xB7 FAT32 volumes",
        c_soft(), UI_FONT_14, 0, false);
    txt(t, rx, 698, "Every partition on the drive will be replaced.", c_soft(), UI_FONT_14, 0, false);
}

// ----------------------------------------------------------------------------
// Диалог подтверждения перед стиранием диска.
// ----------------------------------------------------------------------------
#define DLG_W 600
#define DLG_H 300
#define DLG_X (CENTER_X - DLG_W / 2)
#define DLG_Y 330

// Светлая «вуаль» 50/50 с холстом одним проходом по кадру: сдвиг/маска
// вместо попиксельного альфа-блендинга — дёшево даже на TCG.
static void veil(const gfx_target_t* t, uint32_t color) {
    uint32_t half = (color >> 1) & 0x007F7F7Fu;
    for (int y = 0; y < t->height; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)t->pixels + (size_t)y * (size_t)t->pitch);
        for (int x = 0; x < t->width; x++)
            row[x] = 0xFF000000u | (((row[x] >> 1) & 0x007F7F7Fu) + half);
    }
}

// Мягкая тень, смещённая вниз: альфа убывает квадратично с расстоянием до
// прямоугольника.  gfx_drop_shadow рисует непрозрачные плашки — на светлой
// вуали это выглядело бы серым блоком.  Пиксели под самим диалогом не
// трогаем: он всё равно закрасит их поверх.
static void soft_shadow(const gfx_target_t* t, int sx, int sy, int sw, int sh) {
    const int R = 18, OFF = 8, AMAX = 40;
    int top = sy + OFF, bottom = sy + sh + OFF;
    for (int y = top - R; y < bottom + R; y++) {
        if (y < 0 || y >= t->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)t->pixels + (size_t)y * (size_t)t->pitch);
        int dy = (y < top) ? (top - y) : (y >= bottom ? y - bottom + 1 : 0);
        for (int x = sx - R; x < sx + sw + R; x++) {
            if (x < 0 || x >= t->width) continue;
            if (y >= sy && y < sy + sh && x >= sx && x < sx + sw) { x = sx + sw - 1; continue; }
            int dx = (x < sx) ? (sx - x) : (x >= sx + sw ? x - (sx + sw) + 1 : 0);
            int d = dx > dy ? dx : dy;
            if (d >= R) continue;
            int a = (AMAX * (R - d) * (R - d)) / (R * R);
            if (a > 0) row[x] = gfx_blend(row[x], ((uint32_t)a << 24) | 0x00401830u);
        }
    }
}

static void draw_confirm(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    const install_target_t* tgt = install_target_at(st->sel_target);

    veil(t, th->canvas);
    soft_shadow(t, ui_ux(DLG_X), ui_uy(DLG_Y), ui_uw(DLG_W), ui_uh(DLG_H));
    fill(t, DLG_X, DLG_Y, DLG_W, DLG_H, th->surface);
    frame_rect(t, DLG_X, DLG_Y, DLG_W, DLG_H, th->border);
    {
        int x0 = ui_ux(DLG_X), ww = ui_uw(DLG_W);
        for (int i = 0; i < ww; i++) gfx_fill_rect(t, x0 + i, ui_uy(DLG_Y), 1, ui_uh(4), c_grad(i, ww));
    }

    txt(t, DLG_X + 40, DLG_Y + 52, "ERASE AND INSTALL", th->accent_berry, UI_FONT_MONO_14, 2, false);
    static char title[96];
    int p = 0;
    p = s_append(title, p, (int)sizeof(title), "Erase ");
    p = s_append(title, p, (int)sizeof(title), tgt ? tgt->model : "the drive");
    p = s_append(title, p, (int)sizeof(title), "?");
    txt(t, DLG_X + 40, DLG_Y + 92, title, th->ink, UI_FONT_20, 0, true);

    p = 0;
    p = s_append(title, p, (int)sizeof(title), "All data on this ");
    p = s_append_size(title, p, (int)sizeof(title), tgt ? tgt->size_mb : 0);
    p = s_append(title, p, (int)sizeof(title), " drive will be permanently destroyed.");
    txt(t, DLG_X + 40, DLG_Y + 128, title, c_soft(), UI_FONT_16, 0, false);
    txt(t, DLG_X + 40, DLG_Y + 152, "This cannot be undone.", c_soft(), UI_FONT_16, 0, false);
    if (st->encrypt)
        txt(t, DLG_X + 40, DLG_Y + 182,
            "The drive will be encrypted: a lost passphrase cannot be recovered.",
            th->accent_berry, UI_FONT_14, 0, true);

    draw_ghost_button(t, DLG_X + 40, DLG_Y + DLG_H - 40 - BTN_H, 200, BTN_H, "CANCEL");
    draw_primary_button(t, DLG_X + DLG_W - 40 - 260, DLG_Y + DLG_H - 40 - BTN_H, 260, BTN_H,
                        "ERASE AND INSTALL", true);
}

// ----------------------------------------------------------------------------
// Экран 05 — INSTALL.
// ----------------------------------------------------------------------------
static void draw_install(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    draw_heading(t, "STEP 05", "Installing AMPLIFY-OS",
                 "Do not power off the machine while the drive is being written.");

    static char pct[8];
    int p = 0;
    p = s_append_u64(pct, p, (int)sizeof(pct), (uint64_t)st->overall_pct);
    p = s_append(pct, p, (int)sizeof(pct), "%");
    txt(t, CONTENT_X, 330, pct, th->ink, UI_FONT_44, 0, true);

    txt(t, CONTENT_X + 200, 306, install_step_name(st->step), th->ink, UI_FONT_20, 0, true);
    if (st->message[0]) txt(t, CONTENT_X + 200, 332, st->message, c_soft(), UI_FONT_MONO_14, 0, false);

    // Полоса прогресса.
    fill(t, CONTENT_X, 364, CONTENT_W, 8, th->surface_soft);
    int done_w = (CONTENT_W * st->overall_pct) / 100;
    if (done_w > 0) grad_rect(t, CONTENT_X, 364, done_w, 8);

    // Список шагов.
    int base = 424;
    for (int i = 0; i < INSTALL_STEP_COUNT; i++) {
        bool done    = i < (int)st->step || (st->finished && !st->failed);
        bool current = i == (int)st->step && !done;

        uint32_t dot = done ? th->accent_coral
                     : (current ? (st->failed ? th->accent_coral : th->accent_berry) : th->border);
        fill(t, CONTENT_X + 2, base - 9, 8, 8, dot);

        uint32_t fg = current ? th->ink : (done ? c_soft() : c_faint());
        txt(t, CONTENT_X + 28, base, install_step_name((install_step_t)i), fg,
            UI_FONT_16, 0, current);
        base += 29;             // 12 шагов должны уместиться над рамкой ошибки
    }

    // Журнал.
    int lx = CONTENT_X + 560;
    txt(t, lx, 418, "LOG", c_soft(), UI_FONT_12, 3, true);
    fill(t, lx, 432, 640, 290, th->surface);
    frame_rect(t, lx, 432, 640, 290, th->border);
    for (int i = 0; i < st->log_count; i++)
        txt(t, lx + 22, 466 + i * 32, st->log[i], th->muted, UI_FONT_MONO_14, 0, false);

    if (st->failed) {
        fill(t, CONTENT_X, 760, CONTENT_W, 58, th->surface);
        frame_rect(t, CONTENT_X, 760, CONTENT_W, 58, th->accent_coral);
        txt(t, CONTENT_X + 24, 795, install_last_error(), th->accent_coral, UI_FONT_16, 0, true);
    }
}

// ----------------------------------------------------------------------------
// Экран 06 — COMPLETE.
// ----------------------------------------------------------------------------
#define DONE_BTN_Y 668

static void draw_complete(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();

    ui_draw_bitmap_scaled(t, ui_ux(CENTER_X - 53), ui_uy(236),
                          ui_uw(106), ui_uh(104), ui_logo_big());

    if (st->failed) {
        ctxt(t, CENTER_X, 432, "INSTALLATION FAILED", th->accent_coral, UI_FONT_28, 3, true);
        ctxt(t, CENTER_X, 486, install_last_error(), th->ink, UI_FONT_16, 0, false);
        ctxt(t, CENTER_X, 514, "The drive may be left in an inconsistent state.",
             c_soft(), UI_FONT_16, 0, false);
        ctxt(t, CENTER_X, 540, "Nothing was written to any other drive.",
             c_soft(), UI_FONT_16, 0, false);
        draw_ghost_button(t, CENTER_X - 212, DONE_BTN_Y, 200, BTN_H, "TRY AGAIN");
        draw_primary_button(t, CENTER_X + 12, DONE_BTN_Y, 200, BTN_H, "RESTART", true);
        return;
    }

    ctxt(t, CENTER_X, 432, "INSTALLATION COMPLETE", th->ink, UI_FONT_28, 3, true);
    ctxt(t, CENTER_X, 486, "AMPLIFY-OS is now on your drive.", c_soft(), UI_FONT_16, 0, false);
    ctxt(t, CENTER_X, 512, "Remove the installation media and restart the machine.",
         c_soft(), UI_FONT_16, 0, false);

    const install_target_t* tgt = install_target_at(st->sel_target);
    static char line[128];
    int p = 0;
    p = s_append(line, p, (int)sizeof(line), st->username);
    p = s_append(line, p, (int)sizeof(line), "@");
    p = s_append(line, p, (int)sizeof(line), st->hostname);
    p = s_append(line, p, (int)sizeof(line), "  \xB7  ");
    p = s_append(line, p, (int)sizeof(line), tgt ? tgt->model : "drive");
    if (st->encrypt) p = s_append(line, p, (int)sizeof(line), "  \xB7  LUKS1 encrypted");
    p = s_append(line, p, (int)sizeof(line), "  \xB7  ");
    p = s_append_u64(line, p, (int)sizeof(line), st->elapsed_ms / 1000);
    p = s_append(line, p, (int)sizeof(line), " s");
    ctxt(t, CENTER_X, 566, line, th->accent_berry, UI_FONT_MONO_14, 0, false);

    draw_primary_button(t, CENTER_X - 212, DONE_BTN_Y, 200, BTN_H, "RESTART NOW", true);
    draw_ghost_button(t, CENTER_X + 12, DONE_BTN_Y, 200, BTN_H, "LIVE SESSION");
}

// ----------------------------------------------------------------------------
// Нижняя панель навигации (для контентных экранов).
// ----------------------------------------------------------------------------
static bool screen_can_continue(const ui_state_t* st) {
    switch (st->screen) {
        case SCR_LICENSE:     return st->license_ok;
        case SCR_DESTINATION: return install_target_count() > 0;
        case SCR_OPTIONS:     return options_error(st) == 0;
        default:              return true;
    }
}

static void draw_footer(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    fill(t, CONTENT_X, FOOTER_Y - 32, CONTENT_W, 1, th->border);

    draw_ghost_button(t, CONTENT_X, FOOTER_Y, BTN_W, BTN_H, "BACK");

    const char* next = (st->screen == SCR_OPTIONS) ? "INSTALL NOW" : "CONTINUE";
    draw_primary_button(t, CONTENT_X + CONTENT_W - BTN_W, FOOTER_Y, BTN_W, BTN_H,
                        next, screen_can_continue(st));

    txt(t, CONTENT_X + BTN_W + 32, button_base(FOOTER_Y, BTN_H, UI_FONT_12),
        "Tab moves focus  \xB7  Enter continues  \xB7  Esc goes back",
        c_faint(), UI_FONT_12, 0, false);
}

// ----------------------------------------------------------------------------
// Кадр целиком.
// ----------------------------------------------------------------------------
static void draw_frame(const gfx_target_t* t, const ui_state_t* st) {
    const ui_runtime_theme_t* th = ui_theme_current();
    gfx_clear(t, th->canvas);
    draw_sidebar(t, st->screen);

    switch (st->screen) {
        case SCR_WELCOME:     draw_welcome(t);          break;
        case SCR_LICENSE:     draw_license(t, st);      draw_footer(t, st); break;
        case SCR_DESTINATION: draw_destination(t, st);  draw_footer(t, st); break;
        case SCR_OPTIONS:     draw_options(t, st);      draw_footer(t, st); break;
        case SCR_INSTALL:     draw_install(t, st);      break;
        case SCR_COMPLETE:    draw_complete(t, st);     break;
        default: break;
    }
    if (st->confirm) draw_confirm(t, st);

    const mouse_state_t* m = mouse_get_state();
    if (m) cursor_draw(t, m->x, m->y);
}

static void present(const ui_state_t* st) {
    const gfx_target_t* t = doublebuf_target();
    if (!t) return;
    ui_layout_update(t->width, t->height);
    draw_frame(t, st);
    doublebuf_swap();
}

// ----------------------------------------------------------------------------
// Прогресс установки: колбэк рисует живой кадр прямо во время записи.
// ----------------------------------------------------------------------------
static void log_push(ui_state_t* st, const char* line) {
    if (st->log_count < LOG_LINES) {
        s_copy(st->log[st->log_count++], line, (int)sizeof(st->log[0]));
        return;
    }
    for (int i = 1; i < LOG_LINES; i++) s_copy(st->log[i - 1], st->log[i], (int)sizeof(st->log[0]));
    s_copy(st->log[LOG_LINES - 1], line, (int)sizeof(st->log[0]));
}

static void log_step(ui_state_t* st, install_step_t step) {
    static char line[72];
    int p = 0;
    p = s_append(line, p, (int)sizeof(line), "> ");
    p = s_append(line, p, (int)sizeof(line), install_step_name(step));
    log_push(st, line);
}

static void progress_cb(install_step_t step, int percent, const char* message, void* user) {
    ui_state_t* st = (ui_state_t*)user;

    if (step != st->step) {
        log_step(st, step);
        st->step       = step;
        st->message[0] = 0;
    }
    st->step_pct = percent;
    st->overall_pct = ((int)step * 100 + percent) / INSTALL_STEP_COUNT;
    if (message && !s_eq(message, st->message)) {
        s_copy(st->message, message, (int)sizeof(st->message));
        static char line[72];
        int p = 0;
        p = s_append(line, p, (int)sizeof(line), "  ");
        p = s_append(line, p, (int)sizeof(line), message);
        log_push(st, line);
    }

    // Кадр не чаще ~30 раз в секунду: колбэк из mkfs приходит на каждый
    // мегабайт, и перерисовка 1920x1080 на каждом тике тормозила бы запись.
    static uint64_t last_ms = 0;
    uint64_t now = timer_get_uptime_ms();
    if (now - last_ms < 33 && percent != 0 && percent != 100) return;
    last_ms = now;

    mouse_poll();
    st->frame++;
    present(st);
}

// ----------------------------------------------------------------------------
// Ввод.
// ----------------------------------------------------------------------------
static void poll_pointer(void) {
    static bool was_down = false;
    mouse_poll();
    const mouse_state_t* m = mouse_get_state();
    g_ptr.click = false;
    if (!m) return;
    g_ptr.x = m->x;
    g_ptr.y = m->y;
    bool down = (m->buttons & MOUSE_BTN_LEFT) != 0;
    g_ptr.click = down && !was_down;
    was_down    = down;
}

static bool hit(int x, int y, int w, int h) {
    return g_ptr.click && ui_hit_logical(g_ptr.x, g_ptr.y, x, y, w, h);
}

static void field_type(char* buf, int cap, char c) {
    int n = s_len(buf);
    if (n < cap - 1) { buf[n] = c; buf[n + 1] = 0; }
}

static void field_backspace(char* buf) {
    int n = s_len(buf);
    if (n) buf[n - 1] = 0;
}

// Поле ввода под фокусом (0 — если фокус на переключателе).
static char* focused_field(ui_state_t* st, int* cap) {
    switch (st->focus) {
        case FOCUS_HOSTNAME: *cap = INSTALL_HOSTNAME_MAX; return st->hostname;
        case FOCUS_USERNAME: *cap = INSTALL_USER_MAX;     return st->username;
        case FOCUS_PASSWORD: *cap = INSTALL_PASS_MAX;     return st->passphrase;
        case FOCUS_CONFIRM:  *cap = INSTALL_PASS_MAX;     return st->confirm_pass;
        default:             *cap = 0;                    return 0;
    }
}

static void drain_keyboard(void) {
    while (keyboard_has_char()) keyboard_get_char();
}

// ----------------------------------------------------------------------------
// Переходы между экранами.
// ----------------------------------------------------------------------------
static void go_back(ui_state_t* st) {
    if (st->screen > SCR_WELCOME && st->screen < SCR_INSTALL)
        st->screen = (screen_t)(st->screen - 1);
}

static void start_install(ui_state_t* st);

static void go_next(ui_state_t* st) {
    if (st->screen == SCR_OPTIONS && options_error(st)) {
        st->show_errors = true;
        return;
    }
    if (!screen_can_continue(st)) return;
    switch (st->screen) {
        case SCR_WELCOME:     st->screen = SCR_LICENSE;     break;
        case SCR_LICENSE:     st->screen = SCR_DESTINATION; break;
        case SCR_DESTINATION: st->screen = SCR_OPTIONS;     break;
        case SCR_OPTIONS:     st->confirm = true;           break;
        default: break;
    }
}

static void start_install(ui_state_t* st) {
    const install_target_t* tgt = install_target_at(st->sel_target);
    st->confirm = false;
    if (!tgt) return;

    st->screen      = SCR_INSTALL;
    st->running     = true;
    st->finished    = false;
    st->failed      = false;
    st->step        = INSTALL_STEP_PLAN;
    st->step_pct    = 0;
    st->overall_pct = 0;
    st->log_count   = 0;
    st->message[0]  = 0;
    st->started_ms  = timer_get_uptime_ms();
    log_step(st, INSTALL_STEP_PLAN);
    present(st);

    install_config_t cfg;
    cfg.target_dev = tgt->dev;
    cfg.vault      = st->vault;
    cfg.dark       = st->dark;
    cfg.encrypt    = st->encrypt;
    s_copy(cfg.hostname,   st->hostname,   INSTALL_HOSTNAME_MAX);
    s_copy(cfg.username,   st->username,   INSTALL_USER_MAX);
    s_copy(cfg.passphrase, st->passphrase, INSTALL_PASS_MAX);

    bool ok = install_run(&cfg, progress_cb, st);

    // Пароль в памяти установщика больше не нужен.
    for (int i = 0; i < INSTALL_PASS_MAX; i++) { cfg.passphrase[i] = 0; st->confirm_pass[i] = 0; }

    st->elapsed_ms  = timer_get_uptime_ms() - st->started_ms;
    st->running     = false;
    st->finished    = true;
    st->failed      = !ok;
    if (ok) st->overall_pct = 100;
    log_push(st, ok ? "> Done" : "> Failed");
    present(st);
    timer_sleep(ok ? 900 : 1600);

    // Всё, что пользователь нажал во время записи, выбрасываем — иначе
    // случайный Enter мгновенно перезагрузил бы машину на экране COMPLETE.
    drain_keyboard();
    st->screen = SCR_COMPLETE;
}

static void retry(ui_state_t* st) {
    install_scan_targets();
    if (st->sel_target >= install_target_count()) st->sel_target = 0;
    st->failed = false;
    st->finished = false;
    st->screen = SCR_DESTINATION;
}

// ----------------------------------------------------------------------------
// Обработка одного кадра ввода.  Возвращает false, если пора выйти в live.
// ----------------------------------------------------------------------------
static bool handle_input(ui_state_t* st) {
    poll_pointer();

    // --- Мышь -------------------------------------------------------------
    if (st->confirm) {
        if (hit(DLG_X + 40, DLG_Y + DLG_H - 40 - BTN_H, 200, BTN_H)) st->confirm = false;
        else if (hit(DLG_X + DLG_W - 40 - 260, DLG_Y + DLG_H - 40 - BTN_H, 260, BTN_H)) start_install(st);
    } else switch (st->screen) {
        case SCR_WELCOME:
            if (hit(CENTER_X - 138, 677, 276, BTN_H)) go_next(st);
            break;

        case SCR_LICENSE:
            if (hit(CONTENT_X, 752, 900, 34)) st->license_ok = !st->license_ok;
            break;

        case SCR_DESTINATION: {
            int count = install_target_count();
            for (int i = 0; i < count; i++)
                if (hit(CONTENT_X, 272 + i * 118, CONTENT_W, 104)) st->sel_target = i;
            if (count == 0 && hit(CONTENT_X + CONTENT_W - 40 - BTN_W, 320, BTN_W, BTN_H))
                install_scan_targets();
            break;
        }

        case SCR_OPTIONS:
            for (int i = 0; i < 4; i++)
                if (hit(CONTENT_X, OPT_FIELD_Y0 + i * OPT_FIELD_DY + 22, OPT_FIELD_W, 46)) st->focus = i;
            if (hit(OPT_RIGHT_X - 4, OPT_VAULT_Y - 4, 520, 50)) { st->vault = !st->vault; st->focus = FOCUS_VAULT; }
            if (hit(OPT_RIGHT_X - 4, OPT_DARK_Y - 4,  520, 50)) { st->dark  = !st->dark;  st->focus = FOCUS_DARK;  }
            if (hit(OPT_RIGHT_X - 4, OPT_CRYPT_Y - 4, 520, 50)) { st->encrypt = !st->encrypt; st->focus = FOCUS_CRYPT; }
            break;

        case SCR_COMPLETE:
            if (st->failed) {
                if (hit(CENTER_X - 212, DONE_BTN_Y, 200, BTN_H)) { retry(st); return true; }
                if (hit(CENTER_X + 12,  DONE_BTN_Y, 200, BTN_H)) power_reboot_with_curtain();
            } else {
                if (hit(CENTER_X - 212, DONE_BTN_Y, 200, BTN_H)) power_reboot_with_curtain();
                if (hit(CENTER_X + 12,  DONE_BTN_Y, 200, BTN_H)) return false;
            }
            break;

        default: break;
    }

    // Общая нижняя панель.
    if (!st->confirm && st->screen >= SCR_LICENSE && st->screen <= SCR_OPTIONS) {
        if (hit(CONTENT_X, FOOTER_Y, BTN_W, BTN_H)) go_back(st);
        if (hit(CONTENT_X + CONTENT_W - BTN_W, FOOTER_Y, BTN_W, BTN_H)) go_next(st);
    }

    // --- Клавиатура -------------------------------------------------------
    if (!keyboard_has_char()) return true;
    char c = keyboard_get_char();

    if (st->confirm) {
        if (c == KBD_KEY_ESC) st->confirm = false;
        else if (c == '\n' || c == '\r') start_install(st);
        return true;
    }

    if (c == KBD_KEY_ESC) {
        // Esc на первом экране — выход в live-сессию без установки.
        if (st->screen == SCR_WELCOME) return false;
        if (st->screen == SCR_COMPLETE && !st->failed) return false;
        go_back(st);
        return true;
    }
    if (c == '\n' || c == '\r') {
        if (st->screen == SCR_COMPLETE) {
            if (!st->failed) power_reboot_with_curtain();
            retry(st);
            return true;
        }
        go_next(st);
        return true;
    }

    switch (st->screen) {
        case SCR_LICENSE:
            if (c == ' ') st->license_ok = !st->license_ok;
            break;

        case SCR_DESTINATION: {
            int count = install_target_count();
            if (count > 0 && c == KBD_KEY_UP)
                st->sel_target = (st->sel_target + count - 1) % count;
            if (count > 0 && c == KBD_KEY_DOWN)
                st->sel_target = (st->sel_target + 1) % count;
            if (count == 0 && (c == 'r' || c == 'R')) install_scan_targets();
            break;
        }

        case SCR_OPTIONS: {
            if (c == '\t' || c == KBD_KEY_DOWN) { st->focus = (st->focus + 1) % FOCUS_COUNT; break; }
            if (c == KBD_KEY_BACKTAB || c == KBD_KEY_UP) {
                st->focus = (st->focus + FOCUS_COUNT - 1) % FOCUS_COUNT;
                break;
            }
            if (st->focus == FOCUS_VAULT || st->focus == FOCUS_DARK || st->focus == FOCUS_CRYPT) {
                if (c == ' ' || c == KBD_KEY_LEFT || c == KBD_KEY_RIGHT) {
                    if      (st->focus == FOCUS_VAULT) st->vault   = !st->vault;
                    else if (st->focus == FOCUS_DARK)  st->dark    = !st->dark;
                    else                               st->encrypt = !st->encrypt;
                }
                break;
            }
            int   cap = 0;
            char* buf = focused_field(st, &cap);
            if (!buf) break;
            if (c == '\b') { field_backspace(buf); break; }
            if (c < 32 || c >= 127) break;
            if (st->focus == FOCUS_HOSTNAME && !host_char_ok(c)) break;
            if (st->focus == FOCUS_USERNAME && !user_char_ok(c)) break;
            if (st->focus == FOCUS_HOSTNAME && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            field_type(buf, cap, c);
            break;
        }

        default: break;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Публичный API.
// ----------------------------------------------------------------------------
void installer_init_from_mb2(const mb2_info_t* info) {
    g_live = false;
    if (!info) return;

    const mb2_tag_t* tag = mb2_find_tag(info, MB2_TAG_CMDLINE);
    if (!tag) return;

    const char* cmdline = (const char*)((const uint8_t*)tag + 8);
    if (token_present(cmdline, "installer") || token_present(cmdline, "live")) {
        g_live = true;
        serial_puts("[installer] live mode requested by cmdline\n");
    }
}

bool installer_should_run(void) { return g_live; }

void installer_run(void) {
    serial_puts("[installer] starting\n");

    // Установщик всегда в светлой Pearl-теме, как на макете.
    ui_theme_set_dark(false);

    ui_state_t* st  = &g_ui;
    st->screen      = SCR_WELCOME;
    st->frame       = 0;
    st->license_ok  = false;
    st->sel_target  = 0;
    st->focus       = FOCUS_HOSTNAME;
    st->vault       = false;
    st->dark        = false;
    st->encrypt     = false;
    st->show_errors = false;
    st->confirm     = false;
    st->log_count   = 0;
    st->running     = false;
    st->finished    = false;
    st->failed      = false;
    st->message[0]  = 0;
    s_copy(st->hostname,     "amplify", INSTALL_HOSTNAME_MAX);
    s_copy(st->username,     "",        INSTALL_USER_MAX);
    s_copy(st->passphrase,   "",        INSTALL_PASS_MAX);
    s_copy(st->confirm_pass, "",        INSTALL_PASS_MAX);

    install_scan_targets();
    drain_keyboard();

    for (;;) {
        st->frame++;
        present(st);
        if (!handle_input(st)) break;
        timer_sleep(30);
    }

    serial_puts("[installer] leaving to live session\n");
}
