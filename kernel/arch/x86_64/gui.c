// ============================================================================
// gui.c — реализация минимального GUI compositor.
// ============================================================================
#include "gui.h"
#include "fb.h"
#include "io.h"
#include "pit.h"
#include "pmm.h"
#include "heap.h"
#include "serial.h"

// ----------------------------------------------------------------------------
// Тема (фирменная AMPLIFY-OS palette).
// ----------------------------------------------------------------------------
#define COLOR_DESKTOP_BG     0xFF0A1220
#define COLOR_DESKTOP_GRAD   0xFF1A2438
#define COLOR_LOGO_LINE      0xFF60D0F0
#define COLOR_TEXT_PRIMARY   0xFFFFFFFF
#define COLOR_TEXT_DIM       0xFF8A95B0

#define COLOR_WINDOW_BORDER  0xFF60D0F0
#define COLOR_WINDOW_SHADOW  0xFF050A14

// ----------------------------------------------------------------------------
static gui_window_t g_windows[GUI_MAX_WINDOWS];
static int          g_nwindows = 0;

void gui_init(void) {
    g_nwindows = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) g_windows[i].visible = false;
    serial_puts("[gui] initialized\n");
}

int gui_window_add(const gui_window_t* src) {
    if (g_nwindows >= GUI_MAX_WINDOWS || !src) return -1;
    g_windows[g_nwindows] = *src;
    g_windows[g_nwindows].visible = true;
    return g_nwindows++;
}

// ----------------------------------------------------------------------------
// Низкоуровневый rect-fill (через fb_put_pixel).  Не самый быстрый, но
// достаточный для статичного demo.
// ----------------------------------------------------------------------------
static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_put_pixel(xx, yy, color);
        }
    }
}

// Рамка (1 пиксель).
static void stroke_rect(int x, int y, int w, int h, uint32_t color) {
    for (int xx = x; xx < x + w; xx++) {
        fb_put_pixel(xx, y, color);
        fb_put_pixel(xx, y + h - 1, color);
    }
    for (int yy = y; yy < y + h; yy++) {
        fb_put_pixel(x, yy, color);
        fb_put_pixel(x + w - 1, yy, color);
    }
}

// ----------------------------------------------------------------------------
// Desktop background — вертикальный gradient + AMPLIFY logo.
// ----------------------------------------------------------------------------
static void draw_desktop_bg(void) {
    const aos64_fb_t* st = fb_state();
    if (!st->ok) return;

    // Gradient top → bottom.
    for (uint32_t y = 0; y < st->height; y++) {
        // Линейная интерполяция между COLOR_DESKTOP_GRAD и COLOR_DESKTOP_BG.
        uint32_t t = (y * 256) / st->height;
        uint8_t r1 = (COLOR_DESKTOP_GRAD >> 16) & 0xFF;
        uint8_t g1 = (COLOR_DESKTOP_GRAD >>  8) & 0xFF;
        uint8_t b1 = (COLOR_DESKTOP_GRAD      ) & 0xFF;
        uint8_t r2 = (COLOR_DESKTOP_BG   >> 16) & 0xFF;
        uint8_t g2 = (COLOR_DESKTOP_BG   >>  8) & 0xFF;
        uint8_t b2 = (COLOR_DESKTOP_BG        ) & 0xFF;
        uint8_t r = (uint8_t)((r1 * (256 - t) + r2 * t) / 256);
        uint8_t g = (uint8_t)((g1 * (256 - t) + g2 * t) / 256);
        uint8_t b = (uint8_t)((b1 * (256 - t) + b2 * t) / 256);
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (uint32_t x = 0; x < st->width; x++) {
            fb_put_pixel((int)x, (int)y, c);
        }
    }

    // Большой logo по центру (треугольник).
    int cx = (int)st->width / 2;
    int cy = (int)st->height / 3;
    int size = 80;
    // Внешний треугольник.
    for (int i = 0; i < size; i++) {
        int x_left  = cx - (i * 4) / 5;
        int x_right = cx + (i * 4) / 5;
        fb_put_pixel(x_left,  cy + i, COLOR_LOGO_LINE);
        fb_put_pixel(x_right, cy + i, COLOR_LOGO_LINE);
    }
    // Базовая черта.
    for (int x = cx - (size * 4) / 5; x <= cx + (size * 4) / 5; x++) {
        fb_put_pixel(x, cy + size, COLOR_LOGO_LINE);
    }
    // Внутренний штрих.
    for (int i = 0; i < size / 2; i++) {
        int xl = cx - (i * 4) / 5;
        int xr = cx + (i * 4) / 5;
        fb_put_pixel(xl, cy + size / 2 + i, COLOR_LOGO_LINE);
        fb_put_pixel(xr, cy + size / 2 + i, COLOR_LOGO_LINE);
    }

    // Подпись.
    const char* title = "AMPLIFY-OS x86_64";
    int tw = (int)((sizeof("AMPLIFY-OS x86_64") - 1) * 8);
    fb_draw_string(cx - tw / 2, cy + size + 16, title,
                   COLOR_TEXT_PRIMARY, 0xFF000000);
}

// ----------------------------------------------------------------------------
// Один window draw.
// ----------------------------------------------------------------------------
static void draw_window(const gui_window_t* w) {
    if (!w->visible) return;

    // Тень (1px смещение вниз-вправо).
    fill_rect(w->x + 4, w->y + 4, w->w, w->h, COLOR_WINDOW_SHADOW);

    // Background.
    fill_rect(w->x, w->y, w->w, w->h, w->bg);

    // Title bar.
    int title_h = 24;
    fill_rect(w->x, w->y, w->w, title_h, w->accent);

    // Title text.
    fb_draw_string(w->x + 8, w->y + 4, w->title,
                   COLOR_TEXT_PRIMARY, w->accent);

    // Border.
    stroke_rect(w->x, w->y, w->w, w->h, COLOR_WINDOW_BORDER);
    stroke_rect(w->x, w->y, w->w, title_h, COLOR_WINDOW_BORDER);

    // Content callback.
    if (w->draw_content) {
        w->draw_content(w->x + 4, w->y + title_h + 4,
                        w->w - 8, w->h - title_h - 8);
    }
}

void gui_compose(void) {
    draw_desktop_bg();
    for (int i = 0; i < g_nwindows; i++) {
        draw_window(&g_windows[i]);
    }
}

// ----------------------------------------------------------------------------
// Demo окна
// ----------------------------------------------------------------------------
static void content_about(int x, int y, int w, int h) {
    (void)w; (void)h;
    fb_draw_string(x + 4, y + 4,  "AMPLIFY-OS 64-bit",
                   COLOR_TEXT_PRIMARY, 0xFF1A2438);
    fb_draw_string(x + 4, y + 20, "Build: stage 37.6b",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 36, "Long Mode + APIC + SMP",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 52, "VFS + initrd + FAT32",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 68, "Net: ARP/IPv4/UDP/TCP/DHCP",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 84, "Audio: AC97  Disk: ATA",
                   COLOR_TEXT_DIM, 0xFF1A2438);
}

static void content_matrix(int x, int y, int w, int h) {
    (void)w; (void)h;
    pmm_stats_t  ps = pmm_get_stats();
    heap_stats_t hs = heap_get_stats();
    char buf[64]; int n;

    n = 0; { const char* s = "free pages: "; while (*s) buf[n++] = *s++; }
    {
        uint64_t v = ps.free_pages;
        char t[16]; int ti = 0;
        if (v == 0) t[ti++] = '0';
        else { while (v) { t[ti++] = '0' + v % 10; v /= 10; } }
        while (ti) buf[n++] = t[--ti];
    }
    buf[n] = 0;
    fb_draw_string(x + 4, y + 4, buf, COLOR_TEXT_PRIMARY, 0xFF1A2438);

    n = 0; { const char* s = "heap used: "; while (*s) buf[n++] = *s++; }
    {
        uint64_t v = hs.used_size;
        char t[16]; int ti = 0;
        if (v == 0) t[ti++] = '0';
        else { while (v) { t[ti++] = '0' + v % 10; v /= 10; } }
        while (ti) buf[n++] = t[--ti];
    }
    buf[n++] = ' '; buf[n++] = 'B'; buf[n] = 0;
    fb_draw_string(x + 4, y + 20, buf, COLOR_TEXT_PRIMARY, 0xFF1A2438);

    n = 0; { const char* s = "blocks: "; while (*s) buf[n++] = *s++; }
    {
        uint64_t v = hs.used_blocks;
        char t[16]; int ti = 0;
        if (v == 0) t[ti++] = '0';
        else { while (v) { t[ti++] = '0' + v % 10; v /= 10; } }
        while (ti) buf[n++] = t[--ti];
    }
    buf[n] = 0;
    fb_draw_string(x + 4, y + 36, buf, COLOR_TEXT_PRIMARY, 0xFF1A2438);
}

static void content_welcome(int x, int y, int w, int h) {
    (void)w; (void)h;
    fb_draw_string(x + 4, y + 4,  "Welcome to AMPLIFY-OS!",
                   COLOR_TEXT_PRIMARY, 0xFF1A2438);
    fb_draw_string(x + 4, y + 20, "  64-bit kernel running",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 36, "  Compositor + WM stage 37.6b",
                   COLOR_TEXT_DIM, 0xFF1A2438);
    fb_draw_string(x + 4, y + 52, "  Static demo (mouse @ 37.6c)",
                   COLOR_TEXT_DIM, 0xFF1A2438);
}

// ----------------------------------------------------------------------------
// Cursor sprite — классическая стрелка 10×16 (hot-spot 0,0).
// 'X' = чёрный outline, '.' = белый fill, ' ' = transparent.
// ----------------------------------------------------------------------------
static const char* cursor_sprite[16] = {
    "X         ",
    "XX        ",
    "X.X       ",
    "X..X      ",
    "X...X     ",
    "X....X    ",
    "X.....X   ",
    "X......X  ",
    "X.......X ",
    "X........X",
    "X.....XXXX",
    "X..X..X   ",
    "X.X X..X  ",
    "XX  X..X  ",
    "X    X..X ",
    "      XX  ",
};

void gui_draw_cursor(int x, int y) {
    for (int dy = 0; dy < 16; dy++) {
        const char* row = cursor_sprite[dy];
        for (int dx = 0; dx < 10; dx++) {
            char c = row[dx];
            if (c == 'X')      fb_put_pixel(x + dx, y + dy, 0xFF000000);
            else if (c == '.') fb_put_pixel(x + dx, y + dy, 0xFFFFFFFF);
        }
    }
}

#include "mouse.h"

// Drag state.
static int  drag_id    = -1;
static int  drag_off_x = 0;
static int  drag_off_y = 0;

// Точечная проверка: попадает ли (mx, my) в title-bar окна?
static bool point_in_title(const gui_window_t* w, int mx, int my) {
    return mx >= w->x && mx < w->x + w->w &&
           my >= w->y && my < w->y + 24;
}

static void process_mouse(void) {
    const mouse_state_t* m = mouse_get_state();
    bool left = (m->buttons & MOUSE_BTN_LEFT) != 0;

    if (left && drag_id < 0) {
        // Начало drag — ищем окно под курсором (с конца, top-most).
        for (int i = g_nwindows - 1; i >= 0; i--) {
            if (g_windows[i].visible && point_in_title(&g_windows[i], m->x, m->y)) {
                drag_id = i;
                drag_off_x = m->x - g_windows[i].x;
                drag_off_y = m->y - g_windows[i].y;
                break;
            }
        }
    } else if (!left && drag_id >= 0) {
        drag_id = -1;
    } else if (left && drag_id >= 0) {
        // Двигаем окно следом за курсором.
        g_windows[drag_id].x = m->x - drag_off_x;
        g_windows[drag_id].y = m->y - drag_off_y;
    }
}

void gui_demo_run(void) {
    gui_init();

    // Demo окна.
    gui_window_t w1 = {
        .x = 80, .y = 120, .w = 380, .h = 160,
        .title = "Welcome — AMPLIFY-OS",
        .bg = 0xFF1A2438, .accent = 0xFF2A4068,
        .draw_content = content_welcome,
    };
    gui_window_add(&w1);

    gui_window_t w2 = {
        .x = 520, .y = 220, .w = 360, .h = 160,
        .title = "About",
        .bg = 0xFF1A2438, .accent = 0xFF38598C,
        .draw_content = content_about,
    };
    gui_window_add(&w2);

    gui_window_t w3 = {
        .x = 200, .y = 400, .w = 360, .h = 130,
        .title = "matrix — system stats",
        .bg = 0xFF1A2438, .accent = 0xFF504890,
        .draw_content = content_matrix,
    };
    gui_window_add(&w3);

    serial_puts("[gui] demo running with 3 windows + mouse\n");

    // Главный loop: redraw при mouse event ИЛИ раз в 200ms (чтобы статус-строка
    // обновлял uptime). Первый кадр без задержки.
    uint64_t last_tick = 0;
    bool first = true;
    for (;;) {
        uint64_t now = pit_ticks();
        bool need_redraw = first || mouse_has_event() || (now - last_tick >= 20);

        if (need_redraw) {
            process_mouse();
            mouse_clear_event();
            gui_compose();
            const mouse_state_t* m = mouse_get_state();
            gui_draw_cursor(m->x, m->y);
            last_tick = now;
            first = false;
        }

        __asm__ __volatile__("sti; hlt");
    }
}
