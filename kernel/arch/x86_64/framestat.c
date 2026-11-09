// ============================================================================
// framestat.c — счётчики времени кадра (см. framestat.h).
// ----------------------------------------------------------------------------
// Всё копится в тактах TSC и переводится в микросекунды только при закрытии
// секундного окна: tsc_per_ms = такты окна / миллисекунды окна. Так не нужна
// калибровка при старте, и ошибка ограничена разрешением таймера ядра
// (10 мс на окно в 1000 мс = 1 %; после перехода на 1000 Гц — 0.1 %).
// ============================================================================
#include "framestat.h"
#include "timer.h"
#include "serial.h"
#include "fat32.h"
#include "virtio.h"
#include "virtio_gpu.h"

#define FS_WINDOW_MS 1000u

static uint64_t    g_sum[FS_PHASES];     // такты TSC за окно
static uint64_t    g_max[FS_PHASES];     // самый долгий вызов за окно
static uint64_t    g_pixels;
static uint32_t    g_frames;
static uint32_t    g_wakeups;
static uint64_t    g_win_tsc0;
static uint64_t    g_win_ms0;
static uint64_t    g_win_spins0;
static uint64_t    g_win_hwmoves0;
static bool        g_started;
static bool        g_have;
static bool        g_serial;
static fs_report_t g_last;

static const char* const g_names[FS_PHASES] = {
    "input", "scene", "cursor", "copy", "xfer", "flush", "frame", "sleep"
};

const char* fs_phase_name(int p) {
    return (p >= 0 && p < FS_PHASES) ? g_names[p] : "?";
}

void fs_set_serial(bool on) { g_serial = on; }
bool fs_serial(void)        { return g_serial; }

// perf.conf — строки key=value, как mind.conf. Нужен один ключ: fstat=1.
// Файла нет (live-сессия, чистая установка) — счётчики работают, serial молчит.
static char    g_conf[256];
static int64_t g_conf_len = 0;

bool fs_conf_has(const char* key) {
    for (int64_t i = 0; i < g_conf_len; i++) {
        if (i != 0 && g_conf[i - 1] != '\n') continue;
        int k = 0;
        while (key[k] && g_conf[i + k] == key[k]) k++;
        if (!key[k]) return true;
    }
    return false;
}

bool fs_conf_value(const char* key, char* out, int cap) {
    for (int64_t i = 0; i < g_conf_len; i++) {
        if (i != 0 && g_conf[i - 1] != '\n') continue;
        int k = 0;
        while (key[k] && g_conf[i + k] == key[k]) k++;
        if (key[k]) continue;
        int n = 0;
        for (int64_t j = i + k; j < g_conf_len && g_conf[j] != '\n' && g_conf[j] != '\r' && n < cap - 1; j++)
            out[n++] = g_conf[j];
        out[n] = 0;
        return true;
    }
    return false;
}

void fs_init(void) {
    int64_t n = fat32_read_file(FS_CONF_PATH, g_conf, sizeof(g_conf) - 1);
    if (n <= 0) { g_conf_len = 0; return; }
    g_conf[n] = 0;
    g_conf_len = n;
    if (fs_conf_has("fstat=1")) {
        g_serial = true;
        serial_puts("[fstat] serial report enabled by perf.conf\n");
    }
}

void fs_add(fs_phase_t p, uint64_t t0) {
    uint64_t d = fs_now() - t0;
    g_sum[p] += d;
    if (d > g_max[p]) g_max[p] = d;
}

void fs_pixels(uint64_t n) { g_pixels += n; }
void fs_frame_done(void)   { g_frames++; }

static void window_reset(uint64_t tsc, uint64_t ms) {
    for (int i = 0; i < FS_PHASES; i++) { g_sum[i] = 0; g_max[i] = 0; }
    g_pixels     = 0;
    g_frames     = 0;
    g_wakeups    = 0;
    g_win_tsc0   = tsc;
    g_win_ms0    = ms;
    g_win_spins0 = virtq_wait_spins();
    g_win_hwmoves0 = virtio_gpu_cursor_moves();
}

static void print_report(const fs_report_t* r) {
    serial_puts("[fstat] fps=");
    serial_print_dec(r->fps_x10 / 10); serial_puts(".");
    serial_print_dec(r->fps_x10 % 10);
    for (int i = 0; i < FS_PHASES; i++) {
        serial_puts(" "); serial_puts(g_names[i]); serial_puts("=");
        serial_print_dec(r->avg_us[i]); serial_puts("/");
        serial_print_dec(r->max_us[i]);
    }
    serial_puts(" us(avg/max) busy=");
    serial_print_dec(r->busy_permille / 10); serial_puts(".");
    serial_print_dec(r->busy_permille % 10);
    serial_puts("% wake=");  serial_print_dec(r->wakeups);
    serial_puts(" px=");     serial_print_dec(r->px_per_frame);
    serial_puts(" spins=");  serial_print_dec(r->spins_per_frame);
    serial_puts(" hwmoves="); serial_print_dec(r->hw_cursor_moves);
    serial_puts("\n");
}

void fs_loop_tick(void) {
    uint64_t tsc = fs_now();
    uint64_t ms  = timer_get_uptime_ms();
    if (!g_started) { g_started = true; window_reset(tsc, ms); return; }
    g_wakeups++;

    uint64_t dms = ms - g_win_ms0;
    if (dms < FS_WINDOW_MS) return;

    uint64_t dtsc       = tsc - g_win_tsc0;
    uint64_t tsc_per_ms = dtsc / dms;
    if (tsc_per_ms == 0) { window_reset(tsc, ms); return; }   // TSC стоит — отчёта не будет

    fs_report_t r;
    r.window_ms = (uint32_t)dms;
    r.frames    = g_frames;
    r.fps_x10   = (uint32_t)((uint64_t)g_frames * 10000u / dms);
    r.wakeups   = g_wakeups;
    for (int i = 0; i < FS_PHASES; i++) {
        // SLEEP делим на число пробуждений, остальное — на число кадров:
        // так сумма фаз сходится с FRAME, а не зависит от числа вызовов.
        uint32_t div = (i == FS_SLEEP) ? g_wakeups : g_frames;
        r.avg_us[i] = div ? (uint32_t)(g_sum[i] * 1000u / tsc_per_ms / div) : 0;
        r.max_us[i] = (uint32_t)(g_max[i] * 1000u / tsc_per_ms);
    }
    uint64_t sleep_pm = g_sum[FS_SLEEP] * 1000u / dtsc;
    r.busy_permille   = sleep_pm >= 1000 ? 0 : (uint32_t)(1000 - sleep_pm);
    r.px_per_frame    = g_frames ? (uint32_t)(g_pixels / g_frames) : 0;
    r.spins_per_frame = g_frames
        ? (uint32_t)((virtq_wait_spins() - g_win_spins0) / g_frames) : 0;

    r.hw_cursor_moves = (uint32_t)(virtio_gpu_cursor_moves() - g_win_hwmoves0);

    g_last = r;
    g_have = true;
    if (g_serial) print_report(&r);
    window_reset(tsc, ms);
}

bool fs_last_report(fs_report_t* out) {
    if (!g_have) return false;
    if (out) *out = g_last;
    return true;
}
