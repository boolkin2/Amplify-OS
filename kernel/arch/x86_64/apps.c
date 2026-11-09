// ============================================================================
// apps.c — реализация GUI-приложений AMPLIFY-OS x86_64 (этап 37.6c-3).
// ----------------------------------------------------------------------------
// Все апликации живут в одном файле для компактности. Каждое app:
//   * выделяет state в user_data
//   * предоставляет on_draw / on_click / on_key callbacks
//   * закрытие (WM ставит close_requested) очищает state в free callback.
//
// Внутренний помощник spawn_app_window унифицирует создание окна.
// ============================================================================
#include "apps.h"
#include "wm.h"
#include "heap.h"
#include "graphics.h"
#include "pmm.h"
#include "rtc.h"
#include "timer.h"
#include "cpu.h"
#include "vfs.h"
#include "http.h"
#include "serial.h"
#include "shell.h"
#include "fb.h"
#include "ui_theme.h"
#include "compositor.h"
#include "fat32.h"
#include "keyboard.h"
#include "amply.h"
#include "rune.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- Внутренние helpers --------------------------------------------------

static void copy_str(char* dst, const char* src, int cap) {
    int i;
    for (i = 0; src && i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int  str_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static int  itoa10(int v, char* buf) {
    int n = 0;
    if (v < 0) { buf[n++] = '-'; v = -v; }
    char tmp[12]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    while (ti) buf[n++] = tmp[--ti];
    buf[n] = 0;
    return n;
}

static int  utoa10(uint64_t v, char* buf) {
    int n = 0;
    char tmp[24]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    while (ti) buf[n++] = tmp[--ti];
    buf[n] = 0;
    return n;
}

// Цикл по списку окон wm: ставим spawn-координаты со смещением
static int g_spawn_x = 120;
static int g_spawn_y = 80;

static window_t* spawn_app_window(const char* title, int w, int h,
                                  uint32_t accent,
                                  window_draw_fn  draw_fn,
                                  window_click_fn click_fn,
                                  window_key_fn   key_fn) {
    window_t* win = window_create(title, g_spawn_x, g_spawn_y, w, h);
    if (!win) return 0;
    win->on_draw      = draw_fn;
    win->on_click     = click_fn;
    win->on_key       = key_fn;
    win->accent_color = accent ? accent : 0xFF38598C;
    win->bg_color     = 0xFF1A2438;

    wm_add_window(win);

    g_spawn_x += 30; g_spawn_y += 30;
    if (g_spawn_x > 600) g_spawn_x = 120;
    if (g_spawn_y > 420) g_spawn_y = 80;
    return win;
}

// ============================================================================
// ABOUT
// ============================================================================
static void about_draw(window_t* w, const gfx_target_t* t) {
    gfx_clear(t, w->bg_color);
    gfx_draw_string(t, 14,  12, "AMPLIFY-OS x86_64",          0xFFFFFFFF, w->bg_color);
    gfx_draw_string(t, 14,  32, "этап 37.6c-5 (интерфейс)",   0xFFFFD200, w->bg_color);
    gfx_draw_string(t, 14,  52, "Long Mode + WM + приложения",0xFF8AA8FF, w->bg_color);
    gfx_draw_string(t, 14,  72, "Аутентификация (FNV-1a)",    0xFF8AA8FF, w->bg_color);
    gfx_draw_string(t, 14,  92, "12 графических приложений",  0xFF80FF80, w->bg_color);
    gfx_draw_string(t, 14, 112, "Окна перетаскиваются за заголовок.", 0xFFB0E0F0, w->bg_color);
}
window_t* app_about_open(void) {
    return spawn_app_window("О системе AMPLIFY-OS", 340, 140, 0xFF38598C,
                            about_draw, 0, 0);
}

// ============================================================================
// INFO — system info (CPU vendor/brand, RAM, uptime, наборы инструкций ЦП)
// ----------------------------------------------------------------------------
// Окно расширено: после vendor/brand/RAM/uptime идёт раздел "Инструкции ЦП".
// Каждый поддерживаемый набор инструкций печатается зелёным; вся таблица
// сгруппирована по "семействам" (BASE / SSE-line / AVX / Crypto / BMI / Misc).
// Источник данных — cpu_detect() (см. kernel/arch/x86_64/cpu.c), который
// читает CPUID.1, CPUID.7.0 и CPUID.0x80000001.
// ============================================================================

// Вспомогательная структура: имя инструкции + флаг поддержки.
typedef struct { const char* name; bool present; } info_feat_t;

// Рисует группу плиток-флажков ("MMX", "SSE", "AVX2", ...) с переносом строки
// при выходе за правый край окна. Возвращает Y следующей строки.
static int info_draw_feat_grid(const gfx_target_t* t, int x0, int y, int max_w,
                               const info_feat_t* feats, int count,
                               uint32_t bg) {
    (void)bg;
    int x = x0;
    const int row_h = 18;
    const int pad_x = 8;
    for (int i = 0; i < count; i++) {
        const char* nm = feats[i].name;
        int w_glyphs = 0; for (const char* s = nm; *s; s++) w_glyphs++;
        int box_w = w_glyphs * 8 + 8;       // 4 пикселя padding с каждой стороны
        if (x + box_w > x0 + max_w) {
            x = x0;
            y += row_h;
        }
        // Заливка плитки.
        uint32_t fg, fill, border;
        if (feats[i].present) {
            fg     = 0xFFFFFFFF;
            fill   = 0xFF205838;            // тёмно-зелёный
            border = 0xFF44CC66;
        } else {
            fg     = 0xFF707888;
            fill   = 0xFF1A2030;            // приглушённый
            border = 0xFF303848;
        }
        gfx_fill_rect  (t, x, y - 1, box_w, row_h - 2, fill);
        gfx_stroke_rect(t, x, y - 1, box_w, row_h - 2, border);
        gfx_draw_string(t, x + 4, y + 1, nm, fg, fill);
        x += box_w + pad_x;
    }
    return y + row_h + 4;
}

static void info_draw(window_t* w, const gfx_target_t* t) {
    gfx_clear(t, w->bg_color);
    char buf[96];
    const cpu_info_t* ci = cpu_info();
    int y = 10;
    int x = 14;
    int max_w = w->width - x * 2;

    // ---- Заголовок ----
    gfx_draw_string(t, x, y, "== Сведения о системе ==", 0xFFFFD200, w->bg_color);
    y += 20;

    // ---- ОС ----
    gfx_draw_string(t, x, y, "ОС: AMPLIFY-OS x86_64", 0xFFFFFFFF, w->bg_color);
    y += 18;

    // ---- ЦП vendor + brand ----
    int n = 0; { const char* s = "ЦП: "; while (*s) buf[n++] = *s++; }
    copy_str(buf + n, ci->vendor, sizeof(buf) - n);
    gfx_draw_string(t, x, y, buf, 0xFFFFFFFF, w->bg_color); y += 18;

    if (ci->brand[0]) {
        copy_str(buf, ci->brand, sizeof(buf));
        gfx_draw_string(t, x, y, buf, 0xFFB0E0F0, w->bg_color); y += 18;
    }

    // family/model/stepping
    n = 0; { const char* s = "Семейство: "; while (*s) buf[n++] = *s++; }
    n += utoa10(ci->cpu_family, buf + n);
    { const char* s = ", модель: "; while (*s) buf[n++] = *s++; }
    n += utoa10(ci->cpu_model, buf + n);
    { const char* s = ", степ.: "; while (*s) buf[n++] = *s++; }
    n += utoa10(ci->cpu_stepping, buf + n);
    buf[n] = 0;
    gfx_draw_string(t, x, y, buf, 0xFF8AA8FF, w->bg_color); y += 18;

    // hypervisor (если работаем под QEMU/KVM/Hyper-V и т.п.)
    if (ci->hypervisor && ci->hv_vendor[0]) {
        n = 0; { const char* s = "Гипервизор: "; while (*s) buf[n++] = *s++; }
        copy_str(buf + n, ci->hv_vendor, sizeof(buf) - n);
        gfx_draw_string(t, x, y, buf, 0xFFC890F0, w->bg_color); y += 18;
    }

    // ---- ОЗУ ----
    pmm_stats_t ps = pmm_get_stats();
    n = 0; { const char* s = "Свободно ОЗУ: "; while (*s) buf[n++] = *s++; }
    n += utoa10((uint64_t)ps.free_pages * 4, buf + n);
    { const char* s = " КБ"; while (*s) buf[n++] = *s++; buf[n] = 0; }
    gfx_draw_string(t, x, y, buf, 0xFF80FF80, w->bg_color); y += 18;

    // ---- Uptime ----
    n = 0; { const char* s = "Время работы: "; while (*s) buf[n++] = *s++; }
    n += utoa10(timer_get_uptime_ms() / 1000, buf + n);
    { const char* s = " сек."; while (*s) buf[n++] = *s++; buf[n] = 0; }
    gfx_draw_string(t, x, y, buf, 0xFFFFD200, w->bg_color); y += 22;

    // ---- Разделитель ----
    gfx_fill_rect(t, x, y, max_w, 1, 0xFF334466);
    y += 8;

    // ---- Инструкции ЦП: BASE ----
    gfx_draw_string(t, x, y, "Инструкции ЦП:", 0xFF66AAFF, w->bg_color);
    y += 20;

    gfx_draw_string(t, x, y, "Базовые:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t base[] = {
            {"FPU",       ci->fpu},
            {"TSC",       ci->tsc},
            {"MSR",       ci->msr},
            {"PAE",       ci->pae},
            {"APIC",      ci->apic},
            {"CMOV",      ci->cmov},
            {"PAT",       ci->pat},
            {"CLFLUSH",   ci->clflush},
            {"FXSR",      ci->fxsr},
            {"CX16",      ci->cmpxchg16b},
            {"x2APIC",    ci->x2apic},
            {"HTT",       ci->htt},
        };
        y = info_draw_feat_grid(t, x, y, max_w, base,
                                sizeof(base)/sizeof(base[0]), w->bg_color);
    }

    // ---- SSE-линейка ----
    gfx_draw_string(t, x, y, "SSE / SIMD:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t sse[] = {
            {"MMX",     ci->mmx},
            {"SSE",     ci->sse},
            {"SSE2",    ci->sse2},
            {"SSE3",    ci->sse3},
            {"SSSE3",   ci->ssse3},
            {"SSE4.1",  ci->sse41},
            {"SSE4.2",  ci->sse42},
            {"SSE4A",   ci->sse4a},
            {"FMA",     ci->fma},
            {"F16C",    ci->f16c},
            {"MOVBE",   ci->movbe},
            {"POPCNT",  ci->popcnt},
        };
        y = info_draw_feat_grid(t, x, y, max_w, sse,
                                sizeof(sse)/sizeof(sse[0]), w->bg_color);
    }

    // ---- AVX / AVX-512 ----
    gfx_draw_string(t, x, y, "AVX / AVX-512:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t avx[] = {
            {"AVX",       ci->avx},
            {"AVX2",      ci->avx2},
            {"AVX512F",   ci->avx512f},
            {"AVX512DQ",  ci->avx512dq},
            {"AVX512CD",  ci->avx512cd},
            {"AVX512BW",  ci->avx512bw},
            {"AVX512VL",  ci->avx512vl},
            {"XSAVE",     ci->xsave},
            {"OSXSAVE",   ci->osxsave},
        };
        y = info_draw_feat_grid(t, x, y, max_w, avx,
                                sizeof(avx)/sizeof(avx[0]), w->bg_color);
    }

    // ---- Crypto / RNG ----
    gfx_draw_string(t, x, y, "Криптография и ГСЧ:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t crypt[] = {
            {"AES-NI",     ci->aes},
            {"PCLMULQDQ",  ci->pclmulqdq},
            {"SHA-NI",     ci->sha},
            {"RDRAND",     ci->rdrand},
            {"RDSEED",     ci->rdseed},
            {"ADX",        ci->adx},
        };
        y = info_draw_feat_grid(t, x, y, max_w, crypt,
                                sizeof(crypt)/sizeof(crypt[0]), w->bg_color);
    }

    // ---- BMI / Bit-manipulation / прочие ----
    gfx_draw_string(t, x, y, "BMI и прочее:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t bmi[] = {
            {"BMI1",      ci->bmi1},
            {"BMI2",      ci->bmi2},
            {"LZCNT",     ci->lzcnt},
            {"ERMS",      ci->erms},
            {"FSGSBASE",  ci->fsgsbase},
            {"SMEP",      ci->smep},
            {"SMAP",      ci->smap},
            {"PREFETCHW", ci->prefetchw},
        };
        y = info_draw_feat_grid(t, x, y, max_w, bmi,
                                sizeof(bmi)/sizeof(bmi[0]), w->bg_color);
    }

    // ---- 64-bit / virtualization ----
    gfx_draw_string(t, x, y, "64-bit и виртуализация:", 0xFFAAAACC, w->bg_color);
    y += 18;
    {
        info_feat_t lm[] = {
            {"Long Mode", ci->long_mode},
            {"SYSCALL",   ci->syscall},
            {"NX",        ci->nx},
            {"1G pages",  ci->x64_1gb_pages},
            {"RDTSCP",    ci->rdtscp},
            {"SVM",       ci->svm},
            {"HV",        ci->hypervisor},
        };
        y = info_draw_feat_grid(t, x, y, max_w, lm,
                                sizeof(lm)/sizeof(lm[0]), w->bg_color);
    }
}
window_t* app_info_open(void) {
    // Окно расширено под полную таблицу инструкций (~720x580).
    return spawn_app_window("Сведения о системе", 720, 580, 0xFF4060A8,
                            info_draw, 0, 0);
}

// ============================================================================
// CLOCK
// ============================================================================
static void clock_draw(window_t* w, const gfx_target_t* t) {
    gfx_clear(t, w->bg_color);
    rtc_time_t rt;
    rtc_read(&rt);
    char buf[16];
    int n = 0;
    buf[n++] = '0' + rt.hour   / 10; buf[n++] = '0' + rt.hour   % 10;
    buf[n++] = ':';
    buf[n++] = '0' + rt.minute / 10; buf[n++] = '0' + rt.minute % 10;
    buf[n++] = ':';
    buf[n++] = '0' + rt.second / 10; buf[n++] = '0' + rt.second % 10;
    buf[n] = 0;
    for (int dy = 0; dy < 3; dy++)
        for (int dx = 0; dx < 3; dx++)
            gfx_draw_string(t, 28 + dx, 24 + dy, buf, 0xFFFFD200, w->bg_color);
    char date[16]; n = 0;
    n += utoa10(rt.year, date + n);
    date[n++] = '-';
    date[n++] = '0' + rt.month / 10; date[n++] = '0' + rt.month % 10;
    date[n++] = '-';
    date[n++] = '0' + rt.day   / 10; date[n++] = '0' + rt.day   % 10;
    date[n] = 0;
    gfx_draw_string(t, 50, 64, date, 0xFFE0E8F0, w->bg_color);
}
window_t* app_clock_open(void) {
    return spawn_app_window("Clock", 220, 96, 0xFF50A050,
                            clock_draw, 0, 0);
}

// ============================================================================
// MATRIX — память/uptime
// ============================================================================
static void matrix_draw(window_t* w, const gfx_target_t* t) {
    gfx_clear(t, w->bg_color);
    pmm_stats_t  ps = pmm_get_stats();
    heap_stats_t hs = heap_get_stats();
    char buf[80];
    int y = 12;

    int n = 0; { const char* s = "PMM free pages: "; while (*s) buf[n++] = *s++; }
    n += utoa10(ps.free_pages, buf + n);
    buf[n] = 0;
    gfx_draw_string(t, 14, y, buf, 0xFFFFFFFF, w->bg_color); y += 18;

    n = 0; { const char* s = "Heap used: "; while (*s) buf[n++] = *s++; }
    n += utoa10(hs.used_size, buf + n);
    { const char* s = " / "; while (*s) buf[n++] = *s++; }
    n += utoa10(hs.total_size, buf + n);
    { const char* s = " B"; while (*s) buf[n++] = *s++; buf[n] = 0; }
    gfx_draw_string(t, 14, y, buf, 0xFFFFFFFF, w->bg_color); y += 18;

    n = 0; { const char* s = "Blocks: "; while (*s) buf[n++] = *s++; }
    n += utoa10(hs.used_blocks, buf + n);
    buf[n] = 0;
    gfx_draw_string(t, 14, y, buf, 0xFFB0E0F0, w->bg_color); y += 18;

    n = 0; { const char* s = "Uptime: "; while (*s) buf[n++] = *s++; }
    n += utoa10(timer_get_uptime_ms() / 1000, buf + n);
    { const char* s = " s"; while (*s) buf[n++] = *s++; buf[n] = 0; }
    gfx_draw_string(t, 14, y, buf, 0xFF80FF80, w->bg_color);
}
window_t* app_matrix_open(void) {
    return spawn_app_window("Matrix (system)", 360, 100, 0xFF504890,
                            matrix_draw, 0, 0);
}

// ============================================================================
// CALC — простой калькулятор (4 операции)
// ============================================================================
// Калькулятор: fixed-point ×10000 (4 знака после точки). Kernel-mode без SSE,
// поэтому floating-point исключён.
#define CALC_SCALE 10000LL

typedef struct {
    char    buf[24];      // что напечатано
    int     len;
    int64_t acc;          // fixed-point accumulator
    char    op;           // '+', '-', '*', '/', 0
    bool    fresh;        // следующая цифра очищает экран
} calc_state_t;

static int64_t calc_parse(const calc_state_t* s) {
    int64_t whole = 0, frac = 0, fdiv = 1;
    int i = 0;
    bool neg = false;
    bool in_frac = false;
    if (s->len > 0 && s->buf[0] == '-') { neg = true; i = 1; }
    for (; i < s->len; i++) {
        char c = s->buf[i];
        if (c == '.') { in_frac = true; continue; }
        if (c < '0' || c > '9') continue;
        if (!in_frac) {
            whole = whole * 10 + (c - '0');
        } else if (fdiv < CALC_SCALE) {
            frac = frac * 10 + (c - '0');
            fdiv *= 10;
        }
    }
    // Дополним frac до фиксированной точки CALC_SCALE.
    int64_t v = whole * CALC_SCALE + frac * (CALC_SCALE / fdiv);
    return neg ? -v : v;
}

static void calc_set_result(calc_state_t* s, int64_t v) {
    s->len = 0;
    if (v < 0) { s->buf[s->len++] = '-'; v = -v; }

    int64_t whole = v / CALC_SCALE;
    int64_t frac  = v % CALC_SCALE;

    char tmp[24]; int ti = 0;
    if (whole == 0) tmp[ti++] = '0';
    while (whole) { tmp[ti++] = '0' + (char)(whole % 10); whole /= 10; }
    while (ti) s->buf[s->len++] = tmp[--ti];

    if (frac != 0) {
        s->buf[s->len++] = '.';
        // Печатаем 4 цифры с ведущими нулями, потом обрезаем хвост '0'.
        char fb[5] = "0000";
        for (int k = 3; k >= 0; k--) {
            fb[k] = '0' + (char)(frac % 10);
            frac /= 10;
        }
        int last = 3;
        while (last >= 0 && fb[last] == '0') last--;
        for (int k = 0; k <= last; k++) s->buf[s->len++] = fb[k];
    }
    s->buf[s->len] = 0;
    s->fresh = true;
}

static void calc_apply(calc_state_t* s) {
    if (!s->op) { s->acc = calc_parse(s); return; }
    int64_t cur = calc_parse(s);
    switch (s->op) {
        case '+': s->acc = s->acc + cur; break;
        case '-': s->acc = s->acc - cur; break;
        case '*': s->acc = (s->acc * cur) / CALC_SCALE; break;
        case '/': s->acc = cur != 0 ? (s->acc * CALC_SCALE) / cur : 0; break;
    }
    calc_set_result(s, s->acc);
}

#define CALC_BTN_W 60
#define CALC_BTN_H 44
#define CALC_PAD   8
#define CALC_BTNS_X 14
#define CALC_BTNS_Y 78

static const char calc_layout[4][4] = {
    {'7','8','9','/'},
    {'4','5','6','*'},
    {'1','2','3','-'},
    {'C','0','=','+'}
};

// Возвращает акцентный цвет кнопки по её символу.
static uint32_t calc_btn_color(char ch) {
    if (ch >= '0' && ch <= '9') return UI_BG_LIGHT;
    if (ch == 'C') return UI_FG_ERROR;
    if (ch == '=') return UI_ACCENT_ORANGE;
    return UI_ACCENT_MAGENTA; // операторы
}

static void calc_draw(window_t* w, const gfx_target_t* t) {
    calc_state_t* s = (calc_state_t*)w->user_data;

    // Фон-градиент
    gfx_fill_rect_gradient(t, 0, 0, w->width, w->height,
                           UI_BG_DARK, UI_BG_DEEP);

    // ── Display (большое табло) ──────────────────────────────────────────
    int disp_x = 14, disp_y = 14;
    int disp_w = w->width - 28;
    int disp_h = 54;
    gfx_fill_round_rect(t, disp_x, disp_y, disp_w, disp_h, 8, 0xFF050810);
    gfx_stroke_rect    (t, disp_x, disp_y, disp_w, disp_h,    UI_BORDER_STRONG);
    // подсветка-полоса сверху
    gfx_fill_rect(t, disp_x + 1, disp_y + 1, disp_w - 2, 1,
                  ui_lighten(UI_ACCENT_ORANGE, 20));

    // Текущий операнд — крупно, выровнено вправо. Дублируем глиф
    // для эффекта «жирного» шрифта (благо у нас только 8x16).
    int tw = gfx_string_width(s->buf);
    int tx = disp_x + disp_w - tw - 10;
    int ty = disp_y + (disp_h - 16) / 2 + 1;
    gfx_draw_string(t, tx, ty, s->buf, UI_ACCENT_GOLD, 0xFF050810);
    gfx_draw_string(t, tx + 1, ty, s->buf, UI_ACCENT_GOLD, 0xFF050810);

    // Показываем текущий аккумулятор и активный оператор слева как
    // вспомогательную информацию (если есть незавершённая операция).
    if (s->op) {
        char hint[32]; int n = 0;
        hint[n++] = s->op;
        hint[n++] = ' ';
        // печатаем acc через временный calc_state-формат — но проще
        // показать просто значок оператора.
        hint[n] = 0;
        gfx_draw_string(t, disp_x + 8, ty, hint, UI_ACCENT_MAGENTA, 0xFF050810);
    }

    // ── Кнопки ───────────────────────────────────────────────────────────
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = CALC_BTNS_X + c * (CALC_BTN_W + CALC_PAD);
            int by = CALC_BTNS_Y + r * (CALC_BTN_H + CALC_PAD);
            char ch = calc_layout[r][c];
            uint32_t base = calc_btn_color(ch);

            // Тень-«вдавливание»
            gfx_fill_round_rect(t, bx + 1, by + 2, CALC_BTN_W, CALC_BTN_H, 7,
                                ui_darken(UI_BG_DEEP, 50));

            // Сам прямоугольник с градиентом сверху-вниз.
            gfx_fill_rect_gradient(t, bx, by, CALC_BTN_W, CALC_BTN_H,
                                   ui_lighten(base, 35), base);
            gfx_stroke_rect(t, bx, by, CALC_BTN_W, CALC_BTN_H,
                            ui_darken(base, 60));

            // Подпись
            char lbl[2] = { ch, 0 };
            uint32_t fg = UI_FG_PRIMARY;
            int lblx = bx + (CALC_BTN_W - 8) / 2;
            int lbly = by + (CALC_BTN_H - 16) / 2;
            gfx_draw_string(t, lblx, lbly, lbl, fg, base);
            // «жирный» эффект
            gfx_draw_string(t, lblx + 1, lbly, lbl, fg, base);
        }
    }
    (void)s;
}

static void calc_press(calc_state_t* s, char ch) {
    if (ch >= '0' && ch <= '9') {
        if (s->fresh) { s->len = 0; s->buf[0] = 0; s->fresh = false; }
        if (s->len < (int)sizeof(s->buf) - 1) {
            s->buf[s->len++] = ch;
            s->buf[s->len] = 0;
        }
    } else if (ch == 'C') {
        s->len = 0; s->buf[0] = '0'; s->buf[1] = 0; s->len = 1;
        s->acc = 0; s->op = 0; s->fresh = true;
    } else if (ch == '=') {
        calc_apply(s);
        s->op = 0;
    } else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
        calc_apply(s);
        s->op = ch;
        s->fresh = true;
    }
}

static void calc_click(window_t* w, int lx, int ly) {
    calc_state_t* s = (calc_state_t*)w->user_data;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = CALC_BTNS_X + c * (CALC_BTN_W + CALC_PAD);
            int by = CALC_BTNS_Y + r * (CALC_BTN_H + CALC_PAD);
            if (lx >= bx && lx < bx + CALC_BTN_W &&
                ly >= by && ly < by + CALC_BTN_H) {
                calc_press(s, calc_layout[r][c]);
                return;
            }
        }
    }
}

static void calc_key(window_t* w, char c) {
    calc_state_t* s = (calc_state_t*)w->user_data;
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' || c == '/' ||
        c == '=' || c == 'C' || c == 'c') {
        calc_press(s, c == 'c' ? 'C' : c);
    } else if (c == '\n') {
        calc_press(s, '=');
    } else if (c == '\b') {
        if (s->len > 0) { s->buf[--s->len] = 0; }
        if (s->len == 0) { s->buf[0] = '0'; s->buf[1] = 0; s->len = 1; s->fresh = true; }
    }
}

window_t* app_calc_open(void) {
    calc_state_t* st = (calc_state_t*)kmalloc(sizeof(calc_state_t));
    if (!st) return 0;
    st->buf[0] = '0'; st->buf[1] = 0; st->len = 1;
    st->acc = 0; st->op = 0; st->fresh = true;

    window_t* w = spawn_app_window("Calculator",
        CALC_BTNS_X * 2 + 4 * CALC_BTN_W + 3 * CALC_PAD,
        CALC_BTNS_Y + 4 * CALC_BTN_H + 3 * CALC_PAD + 12,
        UI_ACCENT_ORANGE, calc_draw, calc_click, calc_key);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

// ============================================================================
// NOTES — текстовый редактор с FAT32-persist (этап 37.6c-5).
// ----------------------------------------------------------------------------
// Файл по умолчанию: /disk/NOTES.TXT (8.3, FAT32 root).
//   * При открытии — пробуем загрузить файл; иначе показываем greeter.
//   * Ctrl+S (код 19) — сохранить через fat32_save_file (create or overwrite).
//   * Статус операции отображается в нижней панели ~2 секунды.
// ============================================================================
#define NOTES_BUF      4096
#define NOTES_COLS     58
#define NOTES_PATH     "/disk/NOTES.TXT"

typedef enum {
    NOTES_STATUS_IDLE = 0,
    NOTES_STATUS_LOADED,
    NOTES_STATUS_SAVED,
    NOTES_STATUS_FAIL,
} notes_status_t;

typedef struct {
    char     buf[NOTES_BUF];
    int      len;
    notes_status_t status;
    uint64_t status_until_ms;
} notes_state_t;

static void notes_set_status(notes_state_t* s, notes_status_t st) {
    s->status = st;
    s->status_until_ms = timer_get_uptime_ms() + 2500;
}

static void notes_save(notes_state_t* s) {
    if (!fat32_is_mounted()) { notes_set_status(s, NOTES_STATUS_FAIL); return; }
    int64_t r = fat32_save_file(NOTES_PATH, s->buf, (uint64_t)s->len);
    notes_set_status(s, (r >= 0) ? NOTES_STATUS_SAVED : NOTES_STATUS_FAIL);
    serial_puts("[notes] save -> ");
    serial_puts(NOTES_PATH);
    serial_puts(r >= 0 ? " OK\n" : " FAIL\n");
}

static void notes_load(notes_state_t* s) {
    if (!fat32_is_mounted()) return;
    int64_t r = fat32_read_file(NOTES_PATH, s->buf, NOTES_BUF - 1);
    if (r > 0) {
        s->len = (int)r;
        s->buf[s->len] = 0;
        notes_set_status(s, NOTES_STATUS_LOADED);
        serial_puts("[notes] loaded "); serial_puts(NOTES_PATH); serial_puts("\n");
    }
}

static void notes_draw(window_t* w, const gfx_target_t* t) {
    notes_state_t* s = (notes_state_t*)w->user_data;
    gfx_fill_rect_gradient(t, 0, 0, w->width, w->height,
                           UI_BG_DARK, UI_BG_DEEP);

    // Поле редактирования (papyrus-like)
    gfx_fill_rect(t, 6, 6, w->width - 12, w->height - 36, 0xFF0E1428);
    gfx_stroke_rect(t, 6, 6, w->width - 12, w->height - 36, UI_BORDER_SOFT);

    // Рендерим текст по строкам
    int cx = 14, cy = 12;
    int col = 0;
    for (int i = 0; i < s->len; i++) {
        char c = s->buf[i];
        if (c == '\n') {
            cx = 14; cy += 16; col = 0;
            if (cy > w->height - 44) break;
            continue;
        }
        if (col >= NOTES_COLS) {
            cx = 14; cy += 16; col = 0;
            if (cy > w->height - 44) break;
        }
        char str[2] = { c, 0 };
        gfx_draw_string(t, cx, cy, str, UI_FG_PRIMARY, 0xFF0E1428);
        cx += 8;
        col++;
    }

    // Курсор (мигающий по тикам)
    bool blink = ((timer_get_uptime_ms() / 500) & 1) == 0;
    if (blink && cy <= w->height - 44) {
        gfx_fill_rect(t, cx, cy, 7, 14, UI_ACCENT_GOLD);
    }

    // Bottom bar
    int byb = w->height - 26;
    gfx_fill_rect_gradient(t, 0, byb, w->width, 26, UI_BG_MID, UI_BG_DARK);
    gfx_fill_rect(t, 0, byb, w->width, 1, UI_ACCENT_ORANGE);

    char ll[80]; int n = 0;
    { const char* p = "NOTES.TXT  ·  "; while (*p) ll[n++] = *p++; }
    n += utoa10(s->len, ll + n);
    { const char* p = " bytes  ·  Ctrl+S to save"; while (*p) ll[n++] = *p++; ll[n] = 0; }
    gfx_draw_string(t, 10, byb + 6, ll, UI_FG_SECONDARY, 0xFF152040);

    // Статус (если ещё валиден).
    if (s->status != NOTES_STATUS_IDLE && timer_get_uptime_ms() < s->status_until_ms) {
        const char* msg = "";
        uint32_t fg = UI_FG_OK;
        switch (s->status) {
            case NOTES_STATUS_LOADED: msg = "loaded from /disk/NOTES.TXT"; fg = UI_ACCENT_CYAN; break;
            case NOTES_STATUS_SAVED:  msg = "saved!"; fg = UI_FG_OK; break;
            case NOTES_STATUS_FAIL:   msg = "save failed"; fg = UI_FG_ERROR; break;
            default: break;
        }
        int mw = 0; for (const char* p = msg; *p; p++) mw++;
        gfx_draw_string(t, w->width - mw * 8 - 10, byb + 6, msg, fg, 0xFF152040);
    }
}

static void notes_key(window_t* w, char c) {
    notes_state_t* s = (notes_state_t*)w->user_data;
    if (c == 19) {              // Ctrl+S
        notes_save(s);
        return;
    }
    if (c == '\b') {
        if (s->len > 0) s->len--;
        return;
    }
    // Принимаем visible chars и \n.
    if (c == '\n' || (c >= 32 && c < 127)) {
        if (s->len < NOTES_BUF - 1) s->buf[s->len++] = c;
    }
    (void)w;
}

window_t* app_notes_open(void) {
    notes_state_t* st = (notes_state_t*)kmalloc(sizeof(notes_state_t));
    if (!st) return 0;
    st->len = 0;
    st->status = NOTES_STATUS_IDLE;
    st->status_until_ms = 0;

    // Пробуем загрузить с диска.
    notes_load(st);

    // Если файла нет — стартовое сообщение.
    if (st->len == 0) {
        const char* greet =
            "Welcome to AMPLIFY Notes.\n"
            "Type anything. Backspace deletes.\n"
            "Ctrl+S to save into /disk/NOTES.TXT.\n\n";
        while (*greet && st->len < NOTES_BUF - 1) st->buf[st->len++] = *greet++;
    }

    window_t* w = spawn_app_window("Notes", 560, 380, UI_ACCENT_GOLD,
                                   notes_draw, 0, notes_key);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

// ============================================================================
// TERM — графический терминал, исполняющий полный набор 18 команд shell.c.
// ----------------------------------------------------------------------------
// Дизайн (этап 37.6c-5):
//   * Тёмный фон в стиле UI_BG_DEEP + градиентная шапка.
//   * Заголовок «amplify > shell» + glyph-логотип в углу.
//   * Большой scrollback (~16 KB) с авто-скроллом к низу.
//   * Перехват вывода через fb_console_set_hooks: всё, что shell-команды
//     выводят в fb_console_*, попадает в наш буфер. Так весь набор из 18
//     команд (guide/shine/voice/scan/read/engrave/matrix/embers/pulse/
//     disk/shade/clock/cycle/freeze + algiases ping/cat/ls/echo/help/reboot/halt)
//     работае�� без копипасты кода.
//   * Prompt всегда внизу окна, мигающий курсор.
//   * История последних 16 команд: ↑/↓ переключает (через спец-коды,
//     поскольку наш keyboard.c доставляет '\xE0'-prefixed, но WM их не
//     прокидывает — оставлено на будущее, сейчас просто стрелки игнорятся).
// ============================================================================
#define TERM_BUF        16384
#define TERM_INPUT      256
#define TERM_PROMPT     "amplify > "
#define TERM_PROMPT_LEN 10

typedef struct {
    char buf[TERM_BUF];          // линейный scrollback (rolling)
    int  len;
    char input[TERM_INPUT];
    int  ilen;
    int  cursor;
    char history[16][TERM_INPUT];
    int  history_count;
    int  history_pos;
    int  scroll_lines;
    uint32_t failures;
    uint32_t last_blink;
    bool cursor_on;
    uint64_t opened_ms;
    uint32_t commands_run;
} term_state_t;

// Указатель на «активный» терминал, в который перенаправляется fb_console.
// Только один терм может быть активным на время одного shell_exec_line().
static term_state_t* g_term_active = 0;

static void term_putc_raw(term_state_t* s, char c) {
    // Сдвигаем половину буфера если переполнен
    if (s->len >= TERM_BUF - 2) {
        int half = TERM_BUF / 2;
        for (int i = 0; i < TERM_BUF - half; i++) s->buf[i] = s->buf[i + half];
        s->len -= half;
    }
    s->buf[s->len++] = c;
    s->buf[s->len] = 0;
}

static void term_append(term_state_t* s, const char* str) {
    while (*str) term_putc_raw(s, *str++);
}

// Хуки для fb_console — вызываются shell-командами через fb_console_putc/reset.
static void term_sink_putc(char c) {
    if (!g_term_active) return;
    if (c == '\r') return;          // игнорируем CR — все newlines уже '\n'
    if (c == '\b') {                // backspace из shell — почти не используется
        if (g_term_active->len > 0) {
            g_term_active->len--;
            g_term_active->buf[g_term_active->len] = 0;
        }
        return;
    }
    term_putc_raw(g_term_active, c);
}

static void term_sink_reset(void) {
    if (!g_term_active) return;
    g_term_active->len = 0;
    g_term_active->buf[0] = 0;
}

// [51.3] Программа WebAssembly, запущенная из терминала, работает на другом
// ядре и печатает в него, когда команда уже вернулась; ввод с клавиатуры, пока
// она жива, идёт ей. Вывод fb_console для этого не годится: он привязан к
// терминалу только на время shell_exec_line. Чей терминал — помнит сама
// программа (rune_owner): так «владение» кончается вместе с ней.
static void term_job_out(const char* str) {
    for (const char* c = str; *c; c++) serial_putc(*c);        // как у fb_console: копия в журнал
    // Пока исполняется команда — в её терминал (сообщения самого запуска),
    // после — в терминал-владелец программы.
    term_state_t* t = g_term_active ? g_term_active : (term_state_t*)rune_owner();
    if (!t) return;
    if (t != g_term_active) {
        // Окно могли закрыть — его состояние уже освобождено.
        bool alive = false;
        for (window_t* w = wm_iterate(0); w; w = wm_iterate(w))
            if (w->user_data == t) { alive = true; break; }
        if (!alive) { rune_disown(); return; }
    }
    term_append(t, str);
    wm_invalidate();
}

// Для команды `rune run`: куда программе писать и чей это терминал (0 — команда
// исполняется не из окна терминала).
rune_sink_t app_term_job_sink(void) { return term_job_out; }
void* app_term_job_owner(void) { return g_term_active; }

static void term_exec(term_state_t* s) {
    s->input[s->ilen] = 0;

    // Эхо команды в буфер.
    term_append(s, TERM_PROMPT);
    term_append(s, s->input);
    term_putc_raw(s, '\n');

    // Пустая команда — сразу новый prompt.
    if (s->ilen == 0) {
        s->ilen = 0;
        return;
    }

    // History is committed before execution; duplicates remain meaningful runs.
    int slot = s->history_count < 16 ? s->history_count++ : 15;
    if (slot == 15 && s->history_count >= 16) {
        for (int i=0;i<15;i++) copy_str(s->history[i],s->history[i+1],TERM_INPUT);
    }
    copy_str(s->history[slot],s->input,TERM_INPUT);
    s->history_pos = s->history_count;

    // Перенаправляем весь fb_console-вывод в наш sink на время команды.
    g_term_active = s;
    fb_console_set_hooks(term_sink_putc, term_sink_reset);
    shell_exec_line(s->input);
    s->commands_run++;
    fb_console_set_hooks(0, 0);
    g_term_active = 0;

    s->ilen = 0;
    s->cursor = 0;
    s->input[0] = 0;
}

// Возвращает кол-во строк во фрагменте от start до end (учитывая \n).
static int count_lines(const char* buf, int len) {
    int n = 1;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') n++;
    return n;
}

static void term_draw(window_t* w, const gfx_target_t* t) {
    term_state_t* s = (term_state_t*)w->user_data;
    const uint32_t canvas = 0xFF151019;
    const uint32_t surface = 0xFF211923;
    const uint32_t raised = 0xFF2A202C;
    const uint32_t border = 0xFF352A36;
    const uint32_t text = 0xFFF8F3F7;
    const uint32_t muted = 0xFFA99FAA;
    const uint32_t accent = 0xFFE8785D;
    const uint32_t berry = 0xFFB54478;
    uint64_t age = timer_get_uptime_ms() - s->opened_ms;
    int slide = age < 260 ? (int)((260 - age) / 10) : 0;
    if (age > 320) w->wants_continuous_redraw = false;

    gfx_clear(t, canvas);
    int side_w = w->width / 5;
    if (side_w < 190) side_w = 190;
    if (side_w > 260) side_w = 260;
    gfx_fill_rect(t, 0, 0, side_w, w->height, 0xFF1A141D);
    gfx_fill_rect(t, side_w - 1, 0, 1, w->height, border);

    gfx_draw_string_scaled(t, 28, 30, "PULSE", text, 2);
    gfx_draw_string_transparent(t, 28, 70, "OUTCOME RUNNER", muted);
    const char* nav[] = { "Current", "Recent runs", "Saved", "Shell guide" };
    for (int i = 0; i < 4; i++) {
        int y = 122 + i * 48;
        if (i == 0) gfx_fill_round_rect(t, 18, y - 10, side_w - 36, 38, 10, raised);
        gfx_draw_string_transparent(t, 34, y, nav[i], i == 0 ? text : muted);
    }
    gfx_draw_string_transparent(t, 28, w->height - 58, "ESC  Back to Apex", muted);

    int content_x = side_w + 48;
    int content_w = w->width - content_x - 48;
    gfx_draw_string_scaled(t, content_x, 32, "Say the outcome.", text, 2);
    gfx_draw_string_transparent(t, content_x, 72, "Run a command, inspect the result, keep the signal.", muted);
    gfx_fill_round_rect(t, w->width - 94, 26, 66, 34, 10, raised);
    gfx_draw_string_transparent(t, w->width - 80, 35, "Back", text);

    int card_y = 108 + slide;
    int metrics_h = 94;
    int card_h = w->height - card_y - metrics_h - 44;
    if (card_h < 220) card_h = 220;
    gfx_fill_round_rect(t, content_x, card_y, content_w, card_h, 16, border);
    gfx_fill_round_rect(t, content_x + 1, card_y + 1, content_w - 2, card_h - 2, 15, surface);
    gfx_draw_string_transparent(t, content_x + 22, card_y + 18, "LIVE SHELL", muted);
    gfx_draw_string_transparent(t, content_x + content_w - 142, card_y + 18, "AMPLIFY x86_64", muted);
    gfx_fill_rect(t, content_x + 20, card_y + 44, content_w - 40, 1, border);

    int prompt_h = 48;
    int prompt_y = card_y + card_h - prompt_h - 16;
    gfx_fill_round_rect(t, content_x + 18, prompt_y, content_w - 36, prompt_h, 10, raised);
    gfx_draw_string_transparent(t, content_x + 34, prompt_y + 16, TERM_PROMPT, accent);
    int input_x = content_x + 34 + gfx_string_width_ui(TERM_PROMPT);
    int max_input_cols = (content_x + content_w - input_x - 42) / 8;
    int input_skip = s->ilen > max_input_cols ? s->ilen - max_input_cols : 0;
    for (int i = input_skip; i < s->ilen; i++) {
        char one[2] = { s->input[i], 0 };
        gfx_draw_string_transparent(t, input_x + (i - input_skip) * 8, prompt_y + 16, one, text);
    }
    if (s->cursor_on) {
        int visible_cursor=s->cursor-input_skip;if(visible_cursor<0)visible_cursor=0;
        gfx_fill_rect(t,input_x+visible_cursor*8,prompt_y+15,2,18,accent);
    }

    int area_top = card_y + 58;
    int area_bottom = prompt_y - 12;
    int max_rows = (area_bottom - area_top) / 18;
    if (max_rows < 1) max_rows = 1;
    int total_lines = count_lines(s->buf, s->len);
    int skip_lines = total_lines > max_rows ? total_lines - max_rows : 0;
    if (s->scroll_lines < skip_lines) skip_lines -= s->scroll_lines;
    int line_start = 0, seen = 0;
    while (seen < skip_lines && line_start < s->len) if (s->buf[line_start++] == '\n') seen++;
    int row = 0, col = 0, x = content_x + 22, y = area_top;
    int cols = (content_w - 44) / 8;
    for (int i = line_start; i < s->len && row < max_rows; i++) {
        char c = s->buf[i];
        if (c == '\n') { row++; col = 0; x = content_x + 22; y += 18; continue; }
        if (col >= cols) { row++; col = 0; x = content_x + 22; y += 18; if (row >= max_rows) break; }
        char one[2] = { c, 0 };
        gfx_draw_string_transparent(t, x, y, one, (col == 0 && c == '>') ? accent : text);
        x += 8; col++;
    }

    int gap = 14;
    int metric_y = card_y + card_h + 16;
    int metric_w = (content_w - gap * 2) / 3;
    char count[16]; itoa10((int)s->commands_run, count);
    const char* values[3] = { count, "live", "0" };
    const char* labels[3] = { "commands", "session", "failures" };
    for (int i = 0; i < 3; i++) {
        int mx = content_x + i * (metric_w + gap);
        gfx_fill_round_rect(t, mx, metric_y, metric_w, metrics_h, 14, surface);
        gfx_stroke_rect(t, mx, metric_y, metric_w, metrics_h, border);
        gfx_draw_string_scaled(t, mx + 18, metric_y + 14, values[i], i == 2 ? berry : text, 2);
        gfx_draw_string_transparent(t, mx + 18, metric_y + 56, labels[i], muted);
    }
}

static void term_close(window_t* w) {
    if (compositor_active() && !compositor_is_animating(w)) compositor_anim_close(w);
    else { w->close_requested = true; w->visible = false; }
}

static void term_click(window_t* w, int mx, int my) {
    if (mx >= w->width - 104 && my >= 18 && my < 70) term_close(w);
}

static void term_history_load(term_state_t* s,int pos){
    if(pos<0||pos>=s->history_count)return;
    s->history_pos=pos;
    copy_str(s->input,s->history[pos],TERM_INPUT);s->ilen=str_len(s->input);s->cursor=s->ilen;
}
static void term_key(window_t* w, char c) {
    term_state_t* s = (term_state_t*)w->user_data;
    if (c == 27) {
        // Окно закрывается, программа остаётся: ввода у неё больше не будет.
        if (rune_owner() == s) { rune_input_eof(); rune_disown(); }
        term_close(w);
        return;
    }
    if (rune_owner() == s) {
        // [51.3] Пока программа работает, терминал — её консоль: строка уходит
        // ей на ввод, Ctrl+C останавливает её, Ctrl+D закрывает ввод.
        if (c == 3) { term_append(s, "^C\n"); rune_stop(); return; }
        if (c == 4) { term_append(s, "^D\n"); rune_input_eof(); return; }
        if (c == '\n') {
            s->input[s->ilen] = 0;
            term_append(s, s->input);
            term_putc_raw(s, '\n');
            rune_input(s->input, s->ilen);
            rune_input("\n", 1);
            s->ilen = 0; s->cursor = 0; s->input[0] = 0; s->scroll_lines = 0;
            return;
        }
        if (c == KBD_KEY_UP || c == KBD_KEY_DOWN || c == '\t' || c == 12) return;
        // Правка строки (буквы, Backspace, стрелки влево-вправо) — как обычно, ниже.
    }
    if (c == 12) { term_sink_reset(); s->scroll_lines=0; return; } /* Ctrl+L */
    if (c == 3) { s->ilen=0;s->cursor=0;s->input[0]=0;term_append(s,"^C\n");return; }
    if (c == KBD_KEY_UP) { if(s->history_count)term_history_load(s,s->history_pos>0?s->history_pos-1:0);return; }
    if (c == KBD_KEY_DOWN) { if(s->history_pos+1<s->history_count)term_history_load(s,s->history_pos+1);else{s->history_pos=s->history_count;s->ilen=s->cursor=0;s->input[0]=0;}return; }
    if (c == KBD_KEY_LEFT) { if(s->cursor>0)s->cursor--;return; }
    if (c == KBD_KEY_RIGHT) { if(s->cursor<s->ilen)s->cursor++;return; }
    if (c == '\t') { const char*cmd="guide";if(s->ilen==0){copy_str(s->input,cmd,TERM_INPUT);s->ilen=s->cursor=5;}return; }
    if (c == '\n') { term_exec(s);s->cursor_on=true;s->scroll_lines=0;return; }
    if (c == '\b') { if(s->cursor>0){for(int i=s->cursor-1;i<s->ilen;i++)s->input[i]=s->input[i+1];s->cursor--;s->ilen--;}return; }
    if (c >= 32 && c < 127 && s->ilen < TERM_INPUT-1) { for(int i=s->ilen;i>s->cursor;i--)s->input[i]=s->input[i-1];s->input[s->cursor++]=c;s->input[++s->ilen]=0; }
}

window_t* app_term_open(void) {
    term_state_t* st = (term_state_t*)kmalloc(sizeof(term_state_t));
    if (!st) return 0;
    st->len = 0;
    st->ilen = 0;
    st->cursor = 0;
    st->history_count = 0;
    st->history_pos = 0;
    st->scroll_lines = 0;
    st->failures = 0;
    st->input[0] = 0;
    st->cursor_on = true;
    st->last_blink = 0;
    st->opened_ms = timer_get_uptime_ms();
    st->commands_run = 0;

    // Стартовый баннер.
    term_append(st,
        "         /\\\n"
        "        /  \\\n"
        "       /    \\     AMPLIFY-OS x86_64 terminal\n"
        "      /      \\    stage 37.6c-5 · 18 commands\n"
        "     /________\\\n"
        "\n"
        "Type `guide` for the full command list.\n"
        "Try: matrix, scan, read /disk/HELLO.TXT, clock, embers.\n"
        "\n");

    const gfx_target_t* screen = gfx_screen();
    int sw = screen ? screen->width : 1600;
    int sh = screen ? screen->height : 1000;
    int left = (sw * 64) / 1600;
    int top = (sh * 48) / 1000;
    window_t* w = window_create("Pulse", left, top, sw - left, sh - top);
    if (!w) { kfree(st); return 0; }
    w->on_draw = term_draw;
    w->on_click = term_click;
    w->on_key = term_key;
    w->accent_color = UI_ACCENT_CORAL;
    w->bg_color = 0xFF151019;
    window_set_chrome(w, WINDOW_CHROME_WORKSPACE);
    w->wants_continuous_redraw = true;
    w->user_data = st;
    w->owns_user_data = true;
    wm_add_window(w);
    return w;
}

// ============================================================================
// FILES — файловый менеджер с предпросмотром, удалением и обновлением.
// ----------------------------------------------------------------------------
// Этап 37.6c-5:
//   * Список файлов в левой панели (с выделением).
//   * Превью содержимого справа (читаем до 2 KB через vfs_read).
//   * Тулбар внизу: [Open] [Delete] [Refresh].
//   * "Open" = загрузить превью; "Delete" = fat32_delete_file (только /disk/*).
//   * Статус операции под тулбаром, исчезает через 2.5 секунды.
// ============================================================================
#define FILES_MAX        48
#define FILES_NAME_LEN   40
#define FILES_PREVIEW    2048

typedef enum {
    FILES_STATUS_IDLE = 0,
    FILES_STATUS_LOADED,
    FILES_STATUS_DELETED,
    FILES_STATUS_REFRESH,
    FILES_STATUS_FAIL,
} files_status_t;

typedef struct {
    int       count;
    char      names[FILES_MAX][FILES_NAME_LEN];
    uint32_t  sizes[FILES_MAX];
    int       selected;
    char      preview[FILES_PREVIEW];
    int       preview_len;
    bool      preview_loaded;
    files_status_t status;
    uint64_t  status_until_ms;
    char      status_text[64];
} files_state_t;

static void files_set_status(files_state_t* s, files_status_t st, const char* text) {
    s->status = st;
    s->status_until_ms = timer_get_uptime_ms() + 2500;
    copy_str(s->status_text, text, sizeof(s->status_text));
}

static void files_refresh(files_state_t* s) {
    s->count = 0;
    s->selected = -1;
    s->preview_loaded = false;
    s->preview_len = 0;

    int total = vfs_count();
    for (int i = 0; i < total && s->count < FILES_MAX; i++) {
        vfs_node_t* n = vfs_at(i);
        if (!n) continue;
        copy_str(s->names[s->count], n->name, FILES_NAME_LEN);
        s->sizes[s->count] = (uint32_t)n->size;
        s->count++;
    }
    files_set_status(s, FILES_STATUS_REFRESH, "refreshed");
}

static void files_open_selected(files_state_t* s) {
    if (s->selected < 0 || s->selected >= s->count) {
        files_set_status(s, FILES_STATUS_FAIL, "no selection");
        return;
    }
    const char* name = s->names[s->selected];
    int fd = vfs_open(name);
    if (fd < 0) {
        files_set_status(s, FILES_STATUS_FAIL, "open failed");
        return;
    }
    int64_t got = vfs_read(fd, s->preview, FILES_PREVIEW - 1);
    vfs_close(fd);
    if (got < 0) got = 0;
    s->preview_len = (int)got;
    s->preview[s->preview_len] = 0;
    s->preview_loaded = true;
    files_set_status(s, FILES_STATUS_LOADED, "opened");
}

static bool path_is_disk(const char* p) {
    return p[0] == '/' && p[1] == 'd' && p[2] == 'i' && p[3] == 's' &&
           p[4] == 'k' && p[5] == '/';
}

static void files_delete_selected(files_state_t* s) {
    if (s->selected < 0 || s->selected >= s->count) {
        files_set_status(s, FILES_STATUS_FAIL, "no selection");
        return;
    }
    const char* name = s->names[s->selected];
    if (!path_is_disk(name)) {
        files_set_status(s, FILES_STATUS_FAIL, "only /disk/* deletable");
        return;
    }
    int rc = fat32_delete_file(name);
    if (rc != 0) {
        files_set_status(s, FILES_STATUS_FAIL, "delete failed");
        return;
    }
    // успех — рефрешим список (это перезатрёт status).
    files_refresh(s);
    s->status = FILES_STATUS_DELETED;
    copy_str(s->status_text, "file deleted", sizeof(s->status_text));
    s->status_until_ms = timer_get_uptime_ms() + 2500;
    s->preview_loaded = false;
    s->preview_len = 0;
}

// Геометрия (в px). Делим окно на левы�� список и правый preview.
#define FILES_TOOL_H   30
#define FILES_HEADER_H 26
#define FILES_LIST_W   240
#define FILES_ROW_H    18

// Тулбар-кнопки (Open, Delete, Refresh).
typedef struct { int x, y, w, h; const char* label; uint32_t accent; char action; } files_btn_t;

static void files_layout_buttons(window_t* w, files_btn_t out[3]) {
    int by = w->height - FILES_TOOL_H + 4;
    out[0] = (files_btn_t){ 8,    by, 70, 22, "Open",    UI_ACCENT_CYAN,    'o' };
    out[1] = (files_btn_t){ 86,   by, 70, 22, "Delete",  UI_FG_ERROR,       'd' };
    out[2] = (files_btn_t){ 164,  by, 90, 22, "Refresh", UI_ACCENT_GOLD,    'r' };
}

static void files_draw_button(const gfx_target_t* t, const files_btn_t* b) {
    gfx_fill_rect_gradient(t, b->x, b->y, b->w, b->h,
                           ui_lighten(b->accent, 30), b->accent);
    gfx_stroke_rect(t, b->x, b->y, b->w, b->h, ui_darken(b->accent, 60));
    int lw = 0; for (const char* p = b->label; *p; p++) lw++;
    int tx = b->x + (b->w - lw * 8) / 2;
    int ty = b->y + (b->h - 16) / 2 + 1;
    gfx_draw_string(t, tx, ty, b->label, UI_FG_PRIMARY, b->accent);
}

static void files_draw(window_t* w, const gfx_target_t* t) {
    files_state_t* s = (files_state_t*)w->user_data;
    gfx_fill_rect_gradient(t, 0, 0, w->width, w->height, UI_BG_DARK, UI_BG_DEEP);

    // ── Header ──────────────────────────────────────────────────────────
    gfx_fill_rect_gradient(t, 0, 0, w->width, FILES_HEADER_H, UI_BG_MID, UI_BG_DARK);
    gfx_fill_rect(t, 0, FILES_HEADER_H - 1, w->width, 1, UI_ACCENT_ORANGE);
    char hdr[64]; int n = 0;
    { const char* p = "AMPLIFY Files  ·  "; while (*p) hdr[n++] = *p++; }
    n += utoa10(s->count, hdr + n);
    { const char* p = " entries"; while (*p) hdr[n++] = *p++; hdr[n] = 0; }
    gfx_draw_string(t, 12, 6, hdr, UI_FG_PRIMARY, UI_BG_MID);
    gfx_draw_string(t, w->width - 100, 6, fat32_is_mounted() ? "FAT32 mounted" : "no FAT32",
                    fat32_is_mounted() ? UI_FG_OK : UI_FG_DIM, UI_BG_MID);

    // ── Список ─────────────────────────────────────────────────────────
    int list_x = 0;
    int list_y = FILES_HEADER_H;
    int list_w = FILES_LIST_W;
    int list_h = w->height - FILES_HEADER_H - FILES_TOOL_H;
    gfx_fill_rect(t, list_x, list_y, list_w, list_h, 0xFF0C1226);
    gfx_fill_rect(t, list_x + list_w - 1, list_y, 1, list_h, UI_BORDER_SOFT);

    if (s->count == 0) {
        gfx_draw_string(t, list_x + 10, list_y + 8, "(empty)", UI_FG_DIM, 0xFF0C1226);
    } else {
        int rows_visible = list_h / FILES_ROW_H;
        for (int i = 0; i < s->count && i < rows_visible; i++) {
            int y = list_y + i * FILES_ROW_H;
            uint32_t bg = (i == s->selected) ? UI_BG_LIGHT : 0xFF0C1226;
            uint32_t fg = (i == s->selected) ? UI_FG_PRIMARY : UI_FG_SECONDARY;
            if (i == s->selected) {
                gfx_fill_rect(t, list_x, y, list_w - 1, FILES_ROW_H, bg);
                gfx_fill_rect(t, list_x, y, 3, FILES_ROW_H, UI_ACCENT_ORANGE);
            }
            // Имя обрезаем до 22 символов.
            char nm[24]; int k = 0;
            for (; s->names[i][k] && k < 23; k++) nm[k] = s->names[i][k];
            nm[k] = 0;
            gfx_draw_string(t, list_x + 8, y + 1, nm, fg, bg);
            // Размер в правой части.
            char sb[12]; int sn = 0;
            sn += utoa10(s->sizes[i], sb + sn);
            sb[sn++] = 'B'; sb[sn] = 0;
            int sw = sn * 8;
            gfx_draw_string(t, list_x + list_w - sw - 8, y + 1, sb,
                            UI_FG_DIM, bg);
        }
    }

    // ── Preview ────────────────────────────────────────────────────────
    int pv_x = list_w;
    int pv_y = FILES_HEADER_H;
    int pv_w = w->width - list_w;
    int pv_h = list_h;
    gfx_fill_rect(t, pv_x, pv_y, pv_w, pv_h, 0xFF080F22);

    // Box-drawing-символы U+2500 не входят в CP1251, поэтому используем
    // обычные дефисы — выглядит достаточно опрятно с 8×16 шрифтом.
    gfx_draw_string(t, pv_x + 10, pv_y + 4,
                    s->preview_loaded ? "-- предпросмотр --"
                                      : "(нажмите «Открыть» для предпросмотра)",
                    UI_FG_DIM, 0xFF080F22);

    if (s->preview_loaded) {
        int x = pv_x + 10, y = pv_y + 24;
        int cols = (pv_w - 20) / 8;
        int col = 0;
        for (int i = 0; i < s->preview_len; i++) {
            char c = s->preview[i];
            if (c == '\n') { x = pv_x + 10; y += 14; col = 0; if (y > pv_y + pv_h - 20) break; continue; }
            if (c == '\r') continue;
            if (c < 32 || c > 126) c = '.';
            if (col >= cols) { x = pv_x + 10; y += 14; col = 0; if (y > pv_y + pv_h - 20) break; }
            char str[2] = { c, 0 };
            gfx_draw_string(t, x, y, str, UI_FG_PRIMARY, 0xFF080F22);
            x += 8; col++;
        }
    }

    // ── Toolbar ────────────────────────────────────────────────────────
    int tool_y = w->height - FILES_TOOL_H;
    gfx_fill_rect_gradient(t, 0, tool_y, w->width, FILES_TOOL_H,
                           UI_BG_MID, UI_BG_DARK);
    gfx_fill_rect(t, 0, tool_y, w->width, 1, UI_ACCENT_ORANGE);
    files_btn_t bs[3]; files_layout_buttons(w, bs);
    for (int i = 0; i < 3; i++) files_draw_button(t, &bs[i]);

    // Status справа.
    if (s->status != FILES_STATUS_IDLE && timer_get_uptime_ms() < s->status_until_ms) {
        uint32_t fg = UI_FG_OK;
        if (s->status == FILES_STATUS_FAIL) fg = UI_FG_ERROR;
        if (s->status == FILES_STATUS_REFRESH) fg = UI_ACCENT_CYAN;
        int sl = 0; for (const char* p = s->status_text; *p; p++) sl++;
        int sx = w->width - sl * 8 - 10;
        int sy = tool_y + (FILES_TOOL_H - 16) / 2 + 1;
        gfx_draw_string(t, sx, sy, s->status_text, fg, 0xFF152040);
    }
}

static void files_click(window_t* w, int lx, int ly) {
    files_state_t* s = (files_state_t*)w->user_data;

    // Toolbar buttons.
    files_btn_t bs[3]; files_layout_buttons(w, bs);
    for (int i = 0; i < 3; i++) {
        const files_btn_t* b = &bs[i];
        if (lx >= b->x && lx < b->x + b->w &&
            ly >= b->y && ly < b->y + b->h) {
            switch (b->action) {
                case 'o': files_open_selected(s);  return;
                case 'd': files_delete_selected(s); return;
                case 'r': files_refresh(s);        return;
            }
        }
    }

    // Клик по списку файлов.
    if (lx >= 0 && lx < FILES_LIST_W && ly >= FILES_HEADER_H && ly < w->height - FILES_TOOL_H) {
        int row = (ly - FILES_HEADER_H) / FILES_ROW_H;
        if (row >= 0 && row < s->count) {
            s->selected = row;
            // двойной клик не отслеживаем; одинарный клик только выбирает,
            // явно жмём Open для превью.
        }
    }
}

static void files_key(window_t* w, char c) {
    files_state_t* s = (files_state_t*)w->user_data;
    if (c == '\n') files_open_selected(s);
    else if (c == 4 /*Ctrl+D*/) files_delete_selected(s);
    else if (c == 18 /*Ctrl+R*/) files_refresh(s);
    else if (c == '\t') { if (s->count > 0) s->selected = (s->selected + 1) % s->count; }
    (void)w;
}

window_t* app_files_open(void) {
    files_state_t* st = (files_state_t*)kmalloc(sizeof(files_state_t));
    if (!st) return 0;
    st->status = FILES_STATUS_IDLE;
    st->status_until_ms = 0;
    files_refresh(st);
    st->status = FILES_STATUS_IDLE; // первый рефреш — тихо
    window_t* w = spawn_app_window("Files", 620, 380, UI_ACCENT_CYAN,
                                   files_draw, files_click, files_key);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

// ============================================================================
// PAINT — рисование с палитрой, кистями, очисткой и сохранением (37.6c-5).
// ----------------------------------------------------------------------------
// Layout:
//   Top toolbar (h=28): [•] [●] [⬤] (brush size) | [Clear] [Save]
//   Canvas       :       persistent ARGB-буфер окна
//   Bottom palette (h=22): 12 цветов
// Save → PPM P6 → /disk/PAINT.PPM (полный canvas region).
// ============================================================================
#define PAINT_TOP_H        28
#define PAINT_BOT_H        22
#define PAINT_PATH         "/disk/PAINT.PPM"
#define PAINT_PALETTE_N    12

typedef enum {
    PAINT_STATUS_IDLE = 0,
    PAINT_STATUS_SAVED,
    PAINT_STATUS_FAIL,
    PAINT_STATUS_CLEARED,
} paint_status_t;

typedef struct {
    uint32_t color;
    int      brush;          // 0 = thin (2x2), 1 = medium (4x4), 2 = thick (8x8)
    paint_status_t status;
    uint64_t status_until_ms;
    char     status_text[40];
} paint_state_t;

static const uint32_t paint_palette[PAINT_PALETTE_N] = {
    0xFFFFFFFF, 0xFF000000, 0xFFFF4848, 0xFFF5A037,
    0xFFFFD200, 0xFF80E060, 0xFF40C0A0, 0xFF60D0F0,
    0xFF5878E0, 0xFFC84190, 0xFF8050A0, 0xFFC0A080,
};

static int paint_brush_size(int b) {
    if (b == 0) return 2;
    if (b == 1) return 4;
    return 8;
}

static void paint_set_status(paint_state_t* s, paint_status_t st, const char* text) {
    s->status = st;
    s->status_until_ms = timer_get_uptime_ms() + 2500;
    copy_str(s->status_text, text, sizeof(s->status_text));
}

// Сохраняем canvas (область между top/bottom barами) как PPM P6.
// Простой формат: текстовый header "P6\nW H\n255\n", далее W*H*3 байт BGR…RGB.
static void paint_save(window_t* w, paint_state_t* s) {
    if (!fat32_is_mounted()) { paint_set_status(s, PAINT_STATUS_FAIL, "no FAT32"); return; }
    int cw = w->width;
    int top = PAINT_TOP_H;
    int bot = w->height - PAINT_BOT_H;
    int ch  = bot - top;
    if (cw <= 0 || ch <= 0) { paint_set_status(s, PAINT_STATUS_FAIL, "empty canvas"); return; }

    // Header — короткий.
    char hdr[40]; int hn = 0;
    hdr[hn++] = 'P'; hdr[hn++] = '6'; hdr[hn++] = '\n';
    int v = cw;  char d[8]; int di = 0; if (v==0) d[di++]='0';
    while (v) { d[di++] = '0' + (v % 10); v /= 10; } while (di) hdr[hn++] = d[--di];
    hdr[hn++] = ' ';
    v = ch; di = 0; if (v==0) d[di++]='0';
    while (v) { d[di++] = '0' + (v % 10); v /= 10; } while (di) hdr[hn++] = d[--di];
    hdr[hn++] = '\n';
    const char* tail = "255\n"; while (*tail) hdr[hn++] = *tail++;

    int total = hn + cw * ch * 3;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) { paint_set_status(s, PAINT_STATUS_FAIL, "OOM"); return; }
    for (int i = 0; i < hn; i++) buf[i] = (uint8_t)hdr[i];

    int off = hn;
    for (int y = top; y < bot; y++) {
        for (int x = 0; x < cw; x++) {
            uint32_t c = w->buffer[y * w->width + x];
            buf[off++] = (uint8_t)((c >> 16) & 0xFF); // R
            buf[off++] = (uint8_t)((c >>  8) & 0xFF); // G
            buf[off++] = (uint8_t)( c        & 0xFF); // B
        }
    }

    int64_t r = fat32_save_file(PAINT_PATH, buf, (uint64_t)total);
    kfree(buf);
    paint_set_status(s, (r >= 0) ? PAINT_STATUS_SAVED : PAINT_STATUS_FAIL,
                     (r >= 0) ? "saved /disk/PAINT.PPM" : "save failed");
    serial_puts("[paint] save -> "); serial_puts(PAINT_PATH);
    serial_puts(r >= 0 ? " OK\n" : " FAIL\n");
}

static void paint_clear(window_t* w, paint_state_t* s) {
    int top = PAINT_TOP_H;
    int bot = w->height - PAINT_BOT_H;
    for (int y = top; y < bot; y++)
        for (int x = 0; x < w->width; x++)
            w->buffer[y * w->width + x] = UI_BG_DEEP;
    paint_set_status(s, PAINT_STATUS_CLEARED, "canvas cleared");
}

// Геометрия тулбара
static void paint_btn_rect_brush(int idx, int* bx, int* by, int* bw, int* bh) {
    *bx = 6 + idx * 30; *by = 4; *bw = 26; *bh = PAINT_TOP_H - 8;
}
static void paint_btn_rect_clear(window_t* w, int* bx, int* by, int* bw, int* bh) {
    *bx = w->width - 150; *by = 4; *bw = 64; *bh = PAINT_TOP_H - 8;
}
static void paint_btn_rect_save(window_t* w, int* bx, int* by, int* bw, int* bh) {
    *bx = w->width - 80;  *by = 4; *bw = 70; *bh = PAINT_TOP_H - 8;
}

static void paint_draw(window_t* w, const gfx_target_t* t) {
    paint_state_t* s = (paint_state_t*)w->user_data;

    // ── Top toolbar ─────────────────────────────────────────────────────
    gfx_fill_rect_gradient(t, 0, 0, w->width, PAINT_TOP_H, UI_BG_MID, UI_BG_DARK);
    gfx_fill_rect(t, 0, PAINT_TOP_H - 1, w->width, 1, UI_ACCENT_ORANGE);

    // Brush size кнопки
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; paint_btn_rect_brush(i, &bx, &by, &bw, &bh);
        bool active = (s->brush == i);
        uint32_t bg = active ? UI_ACCENT_ORANGE : UI_BG_LIGHT;
        gfx_fill_round_rect(t, bx, by, bw, bh, 4, bg);
        gfx_stroke_rect(t, bx, by, bw, bh, ui_darken(bg, 50));
        // Точка-индикатор размера
        int dot = paint_brush_size(i);
        int cx = bx + bw / 2 - dot / 2;
        int cy = by + bh / 2 - dot / 2;
        gfx_fill_rect(t, cx, cy, dot, dot, active ? UI_BG_DEEP : UI_FG_PRIMARY);
    }

    // Текущий цвет — preview-квадратик
    int prev_x = 6 + 3 * 30 + 8;
    int prev_y = 6;
    gfx_fill_rect(t, prev_x, prev_y, 16, 16, s->color);
    gfx_stroke_rect(t, prev_x, prev_y, 16, 16, UI_BORDER_STRONG);
    gfx_draw_string(t, prev_x + 22, prev_y, "текущий", UI_FG_SECONDARY, UI_BG_MID);

    // Clear / Save
    {
        int bx, by, bw, bh; paint_btn_rect_clear(w, &bx, &by, &bw, &bh);
        gfx_fill_round_rect(t, bx, by, bw, bh, 4, UI_FG_ERROR);
        gfx_stroke_rect(t, bx, by, bw, bh, ui_darken(UI_FG_ERROR, 60));
        gfx_draw_string(t, bx + (bw - 40) / 2, by + (bh - 16) / 2, "Clear",
                        UI_FG_PRIMARY, UI_FG_ERROR);
    }
    {
        int bx, by, bw, bh; paint_btn_rect_save(w, &bx, &by, &bw, &bh);
        gfx_fill_round_rect(t, bx, by, bw, bh, 4, UI_FG_OK);
        gfx_stroke_rect(t, bx, by, bw, bh, ui_darken(UI_FG_OK, 60));
        gfx_draw_string(t, bx + (bw - 32) / 2, by + (bh - 16) / 2, "Save",
                        UI_BG_DEEP, UI_FG_OK);
    }

    // ── Bottom palette ──────────────────────────────────────────────────
    int pal_y = w->height - PAINT_BOT_H;
    gfx_fill_rect_gradient(t, 0, pal_y, w->width, PAINT_BOT_H, UI_BG_DARK, UI_BG_DEEP);
    int sw = w->width / PAINT_PALETTE_N;
    for (int i = 0; i < PAINT_PALETTE_N; i++) {
        gfx_fill_rect(t, i * sw, pal_y + 2, sw - 1, PAINT_BOT_H - 4,
                      paint_palette[i]);
        if (paint_palette[i] == s->color) {
            gfx_stroke_rect(t, i * sw, pal_y + 2, sw - 1, PAINT_BOT_H - 4,
                            UI_FG_PRIMARY);
            gfx_stroke_rect(t, i * sw + 1, pal_y + 3, sw - 3, PAINT_BOT_H - 6,
                            UI_BG_DEEP);
        }
    }

    // Status overlay поверх тулбара (если активен).
    if (s->status != PAINT_STATUS_IDLE &&
        timer_get_uptime_ms() < s->status_until_ms) {
        uint32_t fg = UI_FG_OK;
        if (s->status == PAINT_STATUS_FAIL) fg = UI_FG_ERROR;
        if (s->status == PAINT_STATUS_CLEARED) fg = UI_ACCENT_CYAN;
        int sl = 0; for (const char* p = s->status_text; *p; p++) sl++;
        int sx = w->width - sl * 8 - 10;
        // рисуем ниже тулбара как баннер на канвасе
        gfx_fill_rect(t, sx - 8, PAINT_TOP_H + 4, sl * 8 + 16, 20, UI_BG_DEEP);
        gfx_draw_string(t, sx, PAINT_TOP_H + 6, s->status_text, fg, UI_BG_DEEP);
    }
}

static void paint_click(window_t* w, int lx, int ly) {
    paint_state_t* s = (paint_state_t*)w->user_data;

    // Top toolbar
    if (ly < PAINT_TOP_H) {
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh; paint_btn_rect_brush(i, &bx, &by, &bw, &bh);
            if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
                s->brush = i;
                return;
            }
        }
        {
            int bx, by, bw, bh; paint_btn_rect_clear(w, &bx, &by, &bw, &bh);
            if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
                paint_clear(w, s);
                return;
            }
        }
        {
            int bx, by, bw, bh; paint_btn_rect_save(w, &bx, &by, &bw, &bh);
            if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
                paint_save(w, s);
                return;
            }
        }
        return;
    }

    // Bottom palette
    if (ly >= w->height - PAINT_BOT_H) {
        int sw = w->width / PAINT_PALETTE_N;
        int idx = lx / sw;
        if (idx >= 0 && idx < PAINT_PALETTE_N) s->color = paint_palette[idx];
        return;
    }

    // Canvas — рисуем кистью.
    int sz = paint_brush_size(s->brush);
    int x0 = lx - sz / 2;
    int y0 = ly - sz / 2;
    for (int dy = 0; dy < sz; dy++) {
        for (int dx = 0; dx < sz; dx++) {
            window_put_pixel(w, x0 + dx, y0 + dy, s->color);
        }
    }
}

static void paint_key(window_t* w, char c) {
    paint_state_t* s = (paint_state_t*)w->user_data;
    if (c == 19) paint_save(w, s);          // Ctrl+S
    else if (c == 12) paint_clear(w, s);    // Ctrl+L (clear)
    else if (c >= '1' && c <= '3') s->brush = c - '1';
    (void)w;
}

window_t* app_paint_open(void) {
    paint_state_t* st = (paint_state_t*)kmalloc(sizeof(paint_state_t));
    if (!st) return 0;
    st->color = paint_palette[4];      // золотой
    st->brush = 1;                     // medium
    st->status = PAINT_STATUS_IDLE;
    st->status_until_ms = 0;
    window_t* w = spawn_app_window("Paint", 480, 320, UI_ACCENT_MAGENTA,
                                   paint_draw, paint_click, paint_key);
    if (!w) { kfree(st); return 0; }
    // Заливаем canvas начальным цветом фона.
    window_fill(w, UI_BG_DEEP);
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

// ============================================================================
// EMBERS — анимация (мерцающие угли)
// ============================================================================
typedef struct {
    uint32_t seed;
} embers_state_t;

static uint32_t embers_rand(embers_state_t* s) {
    s->seed = s->seed * 1664525 + 1013904223;
    return s->seed;
}

static void embers_draw(window_t* w, const gfx_target_t* t) {
    embers_state_t* s = (embers_state_t*)w->user_data;

    // Затухание: умножаем существующий пиксель на 0.92.
    for (int y = 0; y < w->height; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)t->pixels + (size_t)y * t->pitch);
        for (int x = 0; x < w->width; x++) {
            uint32_t c = row[x];
            uint32_t r = (c >> 16) & 0xFF;
            uint32_t g = (c >>  8) & 0xFF;
            uint32_t b =  c        & 0xFF;
            r = (r * 230) / 255;
            g = (g * 230) / 255;
            b = (b * 230) / 255;
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    // Новые искры внизу
    for (int i = 0; i < 30; i++) {
        int x = embers_rand(s) % w->width;
        int y = w->height - 1 - (embers_rand(s) % 4);
        // Огненный цвет
        uint32_t r = 200 + (embers_rand(s) % 56);
        uint32_t g =  80 + (embers_rand(s) %  80);
        uint32_t b =       (embers_rand(s) %  20);
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        window_put_pixel(w, x, y,     c);
        window_put_pixel(w, x, y - 1, c);
    }

    // Подъём искр: для каждого яркого пикселя двигаем вверх.
    for (int y = 1; y < w->height; y++) {
        uint32_t* row     = (uint32_t*)((uint8_t*)t->pixels + (size_t)y * t->pitch);
        uint32_t* row_up  = (uint32_t*)((uint8_t*)t->pixels + (size_t)(y-1) * t->pitch);
        for (int x = 0; x < w->width; x++) {
            uint32_t c = row[x];
            uint32_t r = (c >> 16) & 0xFF;
            if (r > 100) {
                int nx = x + (int)(embers_rand(s) % 3) - 1;
                if (nx < 0) nx = 0;
                if (nx >= w->width) nx = w->width - 1;
                row_up[nx] = c;
            }
        }
    }
}

window_t* app_embers_open(void) {
    embers_state_t* st = (embers_state_t*)kmalloc(sizeof(embers_state_t));
    if (!st) return 0;
    st->seed = (uint32_t)timer_get_uptime_ms() ^ 0xDEADBEEF;
    window_t* w = spawn_app_window("Embers", 360, 240, 0xFFB04020,
                                   embers_draw, 0, 0);
    if (!w) { kfree(st); return 0; }
    window_fill(w, 0xFF000000);
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

// ============================================================================
// BROWSER — с подэтапа 48.2 живёт в app_browser.c (workspace в стиле Pearl).
// ============================================================================

// ============================================================================
// TASKMGR — реальный диспетчер задач (этап 37.6c-5).
// ----------------------------------------------------------------------------
// Три секции:
//   1) Tasks: реальные user-mode задачи (task_iterate из планировщика).
//      Колонки: TID · STATE · entry · user_rsp.
//   2) Windows: список открытых окон GUI (wm_iterate).
//   3) System: PMM/Heap progress bars + uptime.
// Перерисовывается каждый кадр — всегда live данные.
// ============================================================================
#include "task.h"

static void taskmgr_draw_bar(const gfx_target_t* t, int x, int y, int w, int h,
                             uint32_t used, uint32_t total,
                             uint32_t fg, uint32_t bg) {
    gfx_fill_rect(t, x, y, w, h, bg);
    gfx_stroke_rect(t, x, y, w, h, UI_BORDER_STRONG);
    if (total == 0) return;
    int fill = (int)((uint64_t)used * (w - 2) / total);
    if (fill > w - 2) fill = w - 2;
    if (fill < 0) fill = 0;
    gfx_fill_rect_gradient(t, x + 1, y + 1, fill, h - 2,
                           ui_lighten(fg, 40), fg);
}

static void taskmgr_section_title(const gfx_target_t* t, int x, int y,
                                  const char* text, int width) {
    gfx_fill_rect_gradient(t, x, y, width, 20, UI_BG_MID, UI_BG_DARK);
    gfx_fill_rect(t, x, y, 3, 20, UI_ACCENT_ORANGE);
    gfx_draw_string(t, x + 10, y + 2, text, UI_ACCENT_GOLD, UI_BG_MID);
}

static void taskmgr_draw(window_t* w, const gfx_target_t* t) {
    gfx_fill_rect_gradient(t, 0, 0, w->width, w->height, UI_BG_DARK, UI_BG_DEEP);

    int x = 10, y = 8;
    int sec_w = w->width - 20;

    // ── 1. Tasks ────────────────────────────────────────────────────────
    char hdr[64]; int hn = 0;
    { const char* p = "Tasks  ·  "; while (*p) hdr[hn++] = *p++; }
    hn += utoa10(task_count(), hdr + hn);
    { const char* p = " scheduled"; while (*p) hdr[hn++] = *p++; hdr[hn] = 0; }
    taskmgr_section_title(t, x, y, hdr, sec_w);
    y += 22;

    // Header row
    gfx_draw_string(t, x + 4,   y, "TID",   UI_FG_DIM, UI_BG_DARK);
    gfx_draw_string(t, x + 50,  y, "STATE", UI_FG_DIM, UI_BG_DARK);
    gfx_draw_string(t, x + 120, y, "ENTRY", UI_FG_DIM, UI_BG_DARK);
    gfx_draw_string(t, x + 260, y, "USER-RSP", UI_FG_DIM, UI_BG_DARK);
    y += 16;

    int row_y = y;
    int n_tasks = 0;
    task_t* cur_task = task_current();
    for (task_t* tk = task_iterate(0); tk; tk = task_iterate(tk)) {
        if (row_y > w->height - 130) break;
        uint32_t bg_row = (tk == cur_task) ? UI_BG_LIGHT : 0xFF0C1226;
        gfx_fill_rect(t, x, row_y - 1, sec_w, 16, bg_row);

        char tb[32]; int n = 0;
        n += itoa10(tk->tid, tb + n); tb[n] = 0;
        gfx_draw_string(t, x + 4, row_y, tb, UI_FG_PRIMARY, bg_row);

        const char* st = task_state_name(tk->state);
        uint32_t st_fg = UI_FG_SECONDARY;
        if (tk->state == TASK_RUNNING) st_fg = UI_FG_OK;
        else if (tk->state == TASK_DEAD) st_fg = UI_FG_ERROR;
        else if (tk->state == TASK_NEW)  st_fg = UI_ACCENT_CYAN;
        gfx_draw_string(t, x + 50, row_y, st, st_fg, bg_row);

        // entry hex
        char hb[20]; int hi = 0;
        hb[hi++] = '0'; hb[hi++] = 'x';
        for (int k = 11; k >= 0; k--) {
            uint32_t nib = (uint32_t)((tk->user_entry >> (k * 4)) & 0xF);
            hb[hi++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        hb[hi] = 0;
        gfx_draw_string(t, x + 120, row_y, hb, UI_ACCENT_CYAN, bg_row);

        // rsp hex (short)
        char rb[20]; int ri = 0;
        rb[ri++] = '0'; rb[ri++] = 'x';
        for (int k = 11; k >= 0; k--) {
            uint32_t nib = (uint32_t)((tk->user_rsp >> (k * 4)) & 0xF);
            rb[ri++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        }
        rb[ri] = 0;
        gfx_draw_string(t, x + 260, row_y, rb, UI_FG_DIM, bg_row);

        row_y += 16;
        n_tasks++;
    }
    if (n_tasks == 0) {
        gfx_draw_string(t, x + 4, row_y, "(no user-mode tasks)", UI_FG_DIM, UI_BG_DARK);
        row_y += 16;
    }
    y = row_y + 6;

    // ── 2. Windows ─────────────────────��────────────────────────────────
    int n_wins = 0;
    int wins_section_y = y;
    for (window_t* xw = wm_iterate(0); xw; xw = wm_iterate(xw)) {
        if (xw == w) continue;
        n_wins++;
    }
    char wh[40]; int whn = 0;
    { const char* p = "Windows  ·  "; while (*p) wh[whn++] = *p++; }
    whn += utoa10(n_wins, wh + whn);
    { const char* p = " open"; while (*p) wh[whn++] = *p++; wh[whn] = 0; }
    taskmgr_section_title(t, x, wins_section_y, wh, sec_w);
    y = wins_section_y + 22;

    for (window_t* xw = wm_iterate(0); xw; xw = wm_iterate(xw)) {
        if (xw == w) continue;
        if (y > w->height - 76) break;
        uint32_t fg = xw->focused ? UI_ACCENT_GOLD : UI_FG_SECONDARY;
        char buf[64]; int n = 0;
        buf[n++] = '#';
        n += itoa10(xw->id, buf + n);
        buf[n++] = ' '; buf[n++] = ' ';
        copy_str(buf + n, xw->title, sizeof(buf) - n);
        gfx_draw_string(t, x + 6, y, buf, fg, UI_BG_DARK);
        y += 16;
    }
    if (n_wins == 0) {
        gfx_draw_string(t, x + 6, y, "(only TaskMgr)", UI_FG_DIM, UI_BG_DARK);
    }

    // ── 3. System (нижняя полоса с прогресс-барами) ──────────────────────
    int sys_y = w->height - 70;
    taskmgr_section_title(t, x, sys_y, "System", sec_w);
    sys_y += 22;

    pmm_stats_t  ps = pmm_get_stats();
    heap_stats_t hs = heap_get_stats();

    // PMM bar
    char lbl[40]; int ln = 0;
    { const char* p = "RAM: "; while (*p) lbl[ln++] = *p++; }
    ln += utoa10((uint64_t)ps.used_pages * 4 / 1024, lbl + ln);
    { const char* p = "/"; while (*p) lbl[ln++] = *p++; }
    ln += utoa10((uint64_t)(ps.used_pages + ps.free_pages) * 4 / 1024, lbl + ln);
    { const char* p = " MB"; while (*p) lbl[ln++] = *p++; lbl[ln] = 0; }
    gfx_draw_string(t, x + 6, sys_y, lbl, UI_FG_PRIMARY, UI_BG_DARK);
    taskmgr_draw_bar(t, x + 130, sys_y + 2, sec_w - 140, 12,
                     ps.used_pages, ps.used_pages + ps.free_pages,
                     UI_ACCENT_ORANGE, UI_BG_DEEP);
    sys_y += 18;

    // Heap bar
    ln = 0; { const char* p = "Heap: "; while (*p) lbl[ln++] = *p++; }
    ln += utoa10(hs.used_size / 1024, lbl + ln);
    { const char* p = "/"; while (*p) lbl[ln++] = *p++; }
    ln += utoa10(hs.total_size / 1024, lbl + ln);
    { const char* p = " KB"; while (*p) lbl[ln++] = *p++; lbl[ln] = 0; }
    gfx_draw_string(t, x + 6, sys_y, lbl, UI_FG_PRIMARY, UI_BG_DARK);
    taskmgr_draw_bar(t, x + 130, sys_y + 2, sec_w - 140, 12,
                     (uint32_t)hs.used_size, (uint32_t)hs.total_size,
                     UI_ACCENT_MAGENTA, UI_BG_DEEP);
    sys_y += 18;

    // Uptime
    ln = 0; { const char* p = "Up: "; while (*p) lbl[ln++] = *p++; }
    uint64_t up = timer_get_uptime_ms() / 1000;
    ln += utoa10(up, lbl + ln);
    { const char* p = " s"; while (*p) lbl[ln++] = *p++; lbl[ln] = 0; }
    gfx_draw_string(t, x + 6, sys_y, lbl, UI_FG_DIM, UI_BG_DARK);
}

window_t* app_taskmgr_open(void) {
    return spawn_app_window("Task Manager", 460, 360, UI_FG_ERROR,
                            taskmgr_draw, 0, 0);
}

// ============================================================================
// LAUNCH ELF — stub (37.6c-3): рамка с сообщением «not implemented» т.к.
// spawn_user_task сейчас не публичен в WM, тестируется в shell-режиме.
// Будет дополнено в 37.6c-4.
// ============================================================================
typedef struct {
    char  path[64];
    char  status[128];
} launch_state_t;

static void launch_draw(window_t* w, const gfx_target_t* t) {
    launch_state_t* s = (launch_state_t*)w->user_data;
    gfx_clear(t, w->bg_color);
    gfx_draw_string(t, 14, 12, "Запуск ELF:", 0xFFFFFFFF, w->bg_color);
    gfx_draw_string(t, 14, 32, s->path, 0xFFFFD200, w->bg_color);
    gfx_draw_string(t, 14, 60, s->status, 0xFF80FF80, w->bg_color);
}

window_t* app_launch_elf_open(const char* fat32_path) {
    launch_state_t* st = (launch_state_t*)kmalloc(sizeof(launch_state_t));
    if (!st) return 0;
    copy_str(st->path, fat32_path ? fat32_path : "/COUNTER.ELF", sizeof(st->path));
    copy_str(st->status, "launcher reserved for 37.6c-4 (dynlink+spawn)", sizeof(st->status));
    window_t* w = spawn_app_window("Launcher", 460, 110, 0xFFA85090,
                                   launch_draw, 0, 0);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}


// ============================================================================
// AMPLY CENTER — графический клиент пакетного менеджера (этап 45).
// ----------------------------------------------------------------------------
// Окно с двумя колонками:
//   * Слева — список пакетов в репозитории (из /amply_index.txt);
//   * Справа — multi-line лог последних операций (install/remove/verify).
// Внизу — toolbar:
//   [ Установить ]  [ Удалить ]  [ Проверить ]  [ Обновить всё ]  [ Список ]
// Клик по строке слева подсвечивает выбранный пакет; кнопки toolbar'а
// действуют над выбранным.
// ============================================================================
typedef struct {
    int     selected;          // индекс пакета в g_repo (0..N-1) или -1
    int     scroll;            // первая отображаемая строка лога
    char    log[16 * 1024];    // лог операций (rolling)
    int     log_len;
    bool    show_installed;    // true: показываем БД, false: репо
} amply_state_t;

#define AMPLY_LIST_X      14
#define AMPLY_LIST_Y      40
#define AMPLY_LIST_W      260
#define AMPLY_LIST_ITEM_H 22
#define AMPLY_LOG_X       286
#define AMPLY_LOG_Y       40
#define AMPLY_LOG_W       310
#define AMPLY_TOOLBAR_H   30

static int amply_strlen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void amply_log_clear(amply_state_t* s) { s->log[0] = 0; s->log_len = 0; s->scroll = 0; }
static void amply_log_append(amply_state_t* s, const char* m) {
    int n = amply_strlen_(m);
    int cap = (int)sizeof(s->log) - 1;
    if (s->log_len + n > cap) {
        // Сжимаем — убираем первые 4KB.
        int drop = 4096;
        if (drop >= s->log_len) { s->log_len = 0; s->log[0] = 0; }
        else {
            for (int i = 0; i < s->log_len - drop; i++) s->log[i] = s->log[i + drop];
            s->log_len -= drop;
            s->log[s->log_len] = 0;
        }
    }
    if (s->log_len + n > cap) n = cap - s->log_len;
    for (int i = 0; i < n; i++) s->log[s->log_len + i] = m[i];
    s->log_len += n;
    s->log[s->log_len] = 0;
}

static void amply_draw_button(const gfx_target_t* t, int x, int y, int w, int h,
                              const char* label, uint32_t accent) {
    gfx_fill_round_rect(t, x, y, w, h, 6, ui_darken(accent, 60));
    gfx_fill_rect_gradient(t, x + 1, y + 1, w - 2, h - 2,
                           ui_lighten(accent, 30), accent);
    gfx_stroke_rect(t, x, y, w, h, ui_lighten(accent, 50));
    int tw = gfx_string_width(label);
    gfx_draw_string(t, x + (w - tw) / 2, y + (h - 16) / 2 + 1, label,
                    UI_FG_PRIMARY, 0);
}

static void amply_button_rects(window_t* w, int rects[5][4]) {
    int W = w->width;
    int y = w->height - AMPLY_TOOLBAR_H - 6;
    int gap = 6;
    int total = W - 24;
    int bw = (total - 4 * gap) / 5;
    int x = 12;
    for (int i = 0; i < 5; i++) {
        rects[i][0] = x;
        rects[i][1] = y;
        rects[i][2] = bw;
        rects[i][3] = AMPLY_TOOLBAR_H;
        x += bw + gap;
    }
}

static void amply_draw(window_t* w, const gfx_target_t* t) {
    amply_state_t* s = (amply_state_t*)w->user_data;
    if (!s) return;
    gfx_clear(t, w->bg_color);

    // ----- Заголовок -----
    gfx_fill_rect_gradient(t, 0, 0, w->width, 30,
                           UI_ACCENT_ORANGE, UI_ACCENT_MAGENTA);
    const char* title = s->show_installed ? "Amply  ·  Установленные пакеты"
                                          : "Amply  ·  Каталог репозитория";
    gfx_draw_string(t, 12, 8, title, UI_FG_PRIMARY, 0);

    // Tab toggle.
    const char* tab1 = "[Каталог]";
    const char* tab2 = "[Установлено]";
    int tx2 = w->width - gfx_string_width(tab2) - 12;
    int tx1 = tx2 - gfx_string_width(tab1) - 12;
    gfx_draw_string(t, tx1, 8, tab1,
                    s->show_installed ? UI_FG_DIM : UI_FG_PRIMARY, 0);
    gfx_draw_string(t, tx2, 8, tab2,
                    s->show_installed ? UI_FG_PRIMARY : UI_FG_DIM, 0);

    // ----- Левая колонка -----
    gfx_fill_round_rect(t, AMPLY_LIST_X, AMPLY_LIST_Y, AMPLY_LIST_W,
                        w->height - AMPLY_LIST_Y - AMPLY_TOOLBAR_H - 12,
                        8, UI_BG_DARK);

    int n = s->show_installed ? amply_db_count() : amply_repo_count();
    int row_y = AMPLY_LIST_Y + 8;
    for (int i = 0; i < n; i++) {
        const char* name; const char* ver; const char* sum = "";
        if (s->show_installed) {
            const amply_installed_t* r = amply_db_at(i);
            name = r->name; ver = r->version;
        } else {
            const amply_index_entry_t* r = amply_repo_at(i);
            name = r->name; ver = r->version; sum = r->summary;
        }
        bool sel = (s->selected == i);
        if (sel) {
            gfx_fill_round_rect(t, AMPLY_LIST_X + 4, row_y - 2,
                                AMPLY_LIST_W - 8, AMPLY_LIST_ITEM_H, 6,
                                ui_darken(UI_ACCENT_ORANGE, 30));
        }
        gfx_draw_string(t, AMPLY_LIST_X + 12, row_y, name,
                        UI_FG_PRIMARY, sel ? ui_darken(UI_ACCENT_ORANGE, 30) : UI_BG_DARK);
        int nw = gfx_string_width(name);
        gfx_draw_string(t, AMPLY_LIST_X + 12 + nw + 8, row_y, ver,
                        UI_ACCENT_GOLD, sel ? ui_darken(UI_ACCENT_ORANGE, 30) : UI_BG_DARK);
        if (!s->show_installed) {
            // Покажем кратко summary под текстом, обрезаем.
            char trim[40]; int j = 0; for (; sum[j] && j < 39; j++) trim[j] = sum[j]; trim[j] = 0;
            gfx_draw_string(t, AMPLY_LIST_X + 12, row_y + 12, trim,
                            UI_FG_DIM, sel ? ui_darken(UI_ACCENT_ORANGE, 30) : UI_BG_DARK);
        }
        row_y += AMPLY_LIST_ITEM_H + (s->show_installed ? 0 : 4);
    }
    if (n == 0) {
        gfx_draw_string(t, AMPLY_LIST_X + 12, AMPLY_LIST_Y + 12,
                        s->show_installed ? "(пусто — установите пакет)"
                                           : "(каталог репозитория пуст)",
                        UI_FG_DIM, UI_BG_DARK);
    }

    // ----- Правая колонка: лог -----
    gfx_fill_round_rect(t, AMPLY_LOG_X, AMPLY_LOG_Y, AMPLY_LOG_W,
                        w->height - AMPLY_LOG_Y - AMPLY_TOOLBAR_H - 12, 8,
                        UI_BG_DEEP);
    gfx_draw_string(t, AMPLY_LOG_X + 8, AMPLY_LOG_Y + 6, "Журнал операций:",
                    UI_FG_DIM, UI_BG_DEEP);

    int log_y = AMPLY_LOG_Y + 24;
    int max_h = w->height - AMPLY_LOG_Y - AMPLY_TOOLBAR_H - 36;
    int line_h = 14;
    int max_lines = max_h / line_h;
    // Идём по логу со scroll'ом.
    int line = 0;
    int i = 0;
    int skip_lines = s->scroll;
    char tmp[80];
    int  tlen = 0;
    while (i < s->log_len && line < max_lines) {
        char c = s->log[i++];
        if (c == '\n' || tlen >= 78) {
            tmp[tlen] = 0;
            if (skip_lines > 0) skip_lines--;
            else {
                gfx_draw_string(t, AMPLY_LOG_X + 8, log_y + line * line_h, tmp,
                                UI_FG_PRIMARY, UI_BG_DEEP);
                line++;
            }
            tlen = 0;
            if (c != '\n' && tlen < 78) tmp[tlen++] = c;
        } else {
            tmp[tlen++] = c;
        }
    }
    if (tlen > 0 && line < max_lines) {
        tmp[tlen] = 0;
        if (skip_lines == 0)
            gfx_draw_string(t, AMPLY_LOG_X + 8, log_y + line * line_h, tmp,
                            UI_FG_PRIMARY, UI_BG_DEEP);
    }

    // ----- Toolbar -----
    int rects[5][4];
    amply_button_rects(w, rects);
    const char* labels[5] = { "Установить", "Удалить", "Проверить", "Обновить", "Очистить лог" };
    uint32_t cols[5] = {
        UI_ACCENT_ORANGE, UI_FG_ERROR, UI_ACCENT_GOLD,
        UI_ACCENT_MAGENTA, UI_BG_LIGHT
    };
    for (int k = 0; k < 5; k++)
        amply_draw_button(t, rects[k][0], rects[k][1], rects[k][2], rects[k][3],
                          labels[k], cols[k]);
}

static void amply_perform(amply_state_t* s, int op) {
    char tmp[3072]; tmp[0] = 0;
    const char* name = 0;
    if (s->selected >= 0 && !s->show_installed) {
        const amply_index_entry_t* e = amply_repo_at(s->selected);
        if (e) name = e->name;
    } else if (s->selected >= 0 && s->show_installed) {
        const amply_installed_t* r = amply_db_at(s->selected);
        if (r) name = r->name;
    }

    switch (op) {
        case 0:
            if (!name) { amply_log_append(s, "[hint] выбери пакет в каталоге\n"); return; }
            amply_install(name, tmp, sizeof(tmp));
            amply_log_append(s, tmp);
            break;
        case 1:
            if (!name) { amply_log_append(s, "[hint] выбери пакет\n"); return; }
            amply_remove(name, tmp, sizeof(tmp));
            amply_log_append(s, tmp);
            break;
        case 2:
            if (!name) { amply_log_append(s, "[hint] выбери пакет\n"); return; }
            amply_verify_installed(name, tmp, sizeof(tmp));
            amply_log_append(s, tmp);
            break;
        case 3:
            amply_upgrade_all(tmp, sizeof(tmp));
            amply_log_append(s, tmp);
            break;
        case 4:
            amply_log_clear(s);
            amply_log_append(s, "[log] cleared\n");
            break;
    }
}

static void amply_click(window_t* w, int mx, int my) {
    amply_state_t* s = (amply_state_t*)w->user_data;
    if (!s) return;
    int lx = mx - w->x, ly = my - w->y - WINDOW_TITLE_HEIGHT;

    // Заголовочные табы.
    if (ly < 30) {
        const char* tab1 = "[Каталог]";
        const char* tab2 = "[Установлено]";
        int tx2 = w->width - gfx_string_width(tab2) - 12;
        int tx1 = tx2 - gfx_string_width(tab1) - 12;
        if (lx >= tx1 && lx < tx2 - 8) { s->show_installed = false; s->selected = -1; return; }
        if (lx >= tx2 && lx < tx2 + gfx_string_width(tab2)) {
            s->show_installed = true; s->selected = -1; return;
        }
        return;
    }

    // Список слева.
    if (lx >= AMPLY_LIST_X && lx < AMPLY_LIST_X + AMPLY_LIST_W
        && ly >= AMPLY_LIST_Y && ly < w->height - AMPLY_TOOLBAR_H - 12) {
        int n = s->show_installed ? amply_db_count() : amply_repo_count();
        int rel = ly - AMPLY_LIST_Y - 8;
        int item = rel / (AMPLY_LIST_ITEM_H + (s->show_installed ? 0 : 4));
        if (item >= 0 && item < n) s->selected = item;
        return;
    }

    // Toolbar.
    int rects[5][4];
    amply_button_rects(w, rects);
    for (int k = 0; k < 5; k++) {
        if (lx >= rects[k][0] && lx < rects[k][0] + rects[k][2]
            && ly >= rects[k][1] && ly < rects[k][1] + rects[k][3]) {
            amply_perform(s, k);
            return;
        }
    }
}

static void amply_key(window_t* w, char c) {
    amply_state_t* s = (amply_state_t*)w->user_data;
    if (!s) return;
    if (c == '\n' || c == '\r') {
        if (!s->show_installed && s->selected >= 0) amply_perform(s, 0);
        return;
    }
    if (c == 'd' || c == 'D') { if (s->selected >= 0) amply_perform(s, 1); return; }
    if (c == 'v' || c == 'V') { if (s->selected >= 0) amply_perform(s, 2); return; }
    if (c == 'u' || c == 'U') amply_perform(s, 3);
    if (c == 'l' || c == 'L') amply_perform(s, 4);
}

window_t* app_amply_open(void) {
    amply_state_t* st = (amply_state_t*)kmalloc(sizeof(amply_state_t));
    if (!st) return 0;
    for (size_t i = 0; i < sizeof(*st); i++) ((uint8_t*)st)[i] = 0;
    st->selected = -1;
    st->show_installed = false;

    // Вступительная "шапка" в логе.
    char banner[256];
    int o = 0;
    const char* h = "Amply package manager  ·  Welcome\n"
                    "------------------------------------\n"
                    "Repo: ";
    while (h[o]) { banner[o] = h[o]; o++; }
    const char* url = amply_repo_url();
    if (url[0]) {
        for (int i = 0; url[i] && o < (int)sizeof(banner) - 1; i++) banner[o++] = url[i];
    } else {
        const char* off = "(offline / initrd)";
        for (int i = 0; off[i] && o < (int)sizeof(banner) - 1; i++) banner[o++] = off[i];
    }
    banner[o++] = '\n';
    banner[o] = 0;
    amply_log_append(st, banner);

    char counts[64];
    int n_repo = amply_repo_count();
    int n_db   = amply_db_count();
    int p = 0;
    const char* l1 = "Repo entries: "; while (l1[p]) { counts[p] = l1[p]; p++; }
    char nbuf[16]; int nl = 0;
    int v = n_repo;
    if (v == 0) nbuf[nl++] = '0';
    else { char tmp[16]; int tn = 0; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) nbuf[nl++] = tmp[--tn]; }
    for (int i = 0; i < nl; i++) counts[p++] = nbuf[i];
    const char* l2 = "  ·  Installed: ";
    int li = 0; while (l2[li]) counts[p++] = l2[li++];
    nl = 0; v = n_db;
    if (v == 0) nbuf[nl++] = '0';
    else { char tmp[16]; int tn = 0; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) nbuf[nl++] = tmp[--tn]; }
    for (int i = 0; i < nl; i++) counts[p++] = nbuf[i];
    counts[p++] = '\n'; counts[p] = 0;
    amply_log_append(st, counts);
    amply_log_append(st, "Подсказки: Enter — установить, D — удалить, V — проверить, U — обновить, L — очистить лог.\n");

    window_t* w = spawn_app_window("Amply Center", 620, 420, UI_ACCENT_ORANGE,
                                   amply_draw, amply_click, amply_key);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}
