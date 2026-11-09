// ============================================================================
// startmenu.c - Меню Пуск
// ============================================================================

#include "startmenu.h"
#include "graphics.h"
#include "font.h"
#include "desktop.h"
#include "wm.h"
#include "framebuffer.h"
#include "doublebuf.h"
#include "../arch/i386/power.h"
#include "../memory/memory.h"
#include <stddef.h>

// ============================================================================
// Состояние меню
// ============================================================================

static bool     menu_open   = false;
static int32_t  menu_x      = 0;
static int32_t  menu_y      = 0;

// ============================================================================
// Утилиты
// ============================================================================

static void blend_rect_alpha(int x, int y, int w, int h, uint32_t rgb, uint8_t a) {
    // Полупрозрачный прямоугольник — для фона меню с лёгкой прозрачностью.
    if (a == 0) return;
    framebuffer_t* fb = framebuffer_get();
    uint32_t* buf = doublebuf_get_buffer();
    uint32_t  pitch = fb->pitch / 4;
    int fbw = (int)fb->width;
    int fbh = (int)fb->height;

    uint32_t inv = 255 - a;
    uint32_t sr = (rgb >> 16) & 0xFF;
    uint32_t sg = (rgb >> 8)  & 0xFF;
    uint32_t sb =  rgb        & 0xFF;
    uint32_t sr_a = sr * a;
    uint32_t sg_a = sg * a;
    uint32_t sb_a = sb * a;

    for (int ry = y; ry < y + h; ry++) {
        if (ry < 0 || ry >= fbh) continue;
        uint32_t* row = buf + ry * pitch;
        for (int rx = x; rx < x + w; rx++) {
            if (rx < 0 || rx >= fbw) continue;
            uint32_t dst = row[rx];
            uint32_t dr = (dst >> 16) & 0xFF;
            uint32_t dg = (dst >> 8)  & 0xFF;
            uint32_t db =  dst        & 0xFF;
            dr = (sr_a + dr * inv) / 255;
            dg = (sg_a + dg * inv) / 255;
            db = (sb_a + db * inv) / 255;
            row[rx] = 0xFF000000u | (dr << 16) | (dg << 8) | db;
        }
    }
}

// Иконка «Пуск» — стилизованный треугольник как в boot-animation.
static void draw_start_triangle(int cx, int cy, int size, uint32_t col_outer, uint32_t col_inner) {
    // Внешний контур — равнобедренный треугольник, apex сверху.
    int half = size / 2;
    int top_y  = cy - half;
    int bot_y  = cy + half;
    // Быстрый filled triangle через сканлайн: для каждой y считаем x-диапазон.
    for (int y = top_y; y <= bot_y; y++) {
        int t = y - top_y;
        int th = bot_y - top_y;
        if (th == 0) th = 1;
        // Ширина полуоснования на уровне y:
        int hw = (t * half) / th;
        for (int x = cx - hw; x <= cx + hw; x++) {
            gfx_put_pixel(x, y, col_outer);
        }
    }
    // Внутренний треугольник-«отверстие»: уменьшенный, сдвинут вниз.
    int inner_half = half * 55 / 100;
    int itop = cy - inner_half + size / 10;
    int ibot = cy + inner_half - size / 15;
    for (int y = itop; y <= ibot; y++) {
        int t = y - itop;
        int th = ibot - itop;
        if (th == 0) th = 1;
        int hw = (t * inner_half) / th;
        for (int x = cx - hw; x <= cx + hw; x++) {
            gfx_put_pixel(x, y, col_inner);
        }
    }
}

// ============================================================================
// Публичный API
// ============================================================================

void startmenu_init(void) {
    menu_open = false;
}

void startmenu_draw_button(int btn_x, int btn_y, int side, bool hovered) {
    // Круглый фон кнопки — полупрозрачный светящийся круг.
    int cx = btn_x + side / 2;
    int cy = btn_y + side / 2;
    int r  = side / 2 - 2;

    uint32_t glow_outer = hovered ? 0xFF8844u : 0xCC4488u;
    uint32_t glow_inner = hovered ? 0xFFBB55u : 0xFF7799u;

    // Радиальное затухание: ярче центр, темнее край.
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            int r2 = r * r;
            if (d2 > r2) continue;
            // t = 0 в центре, 1 на краю
            int t = (d2 * 255) / (r2 > 0 ? r2 : 1);
            if (t > 255) t = 255;
            uint32_t col = color_lerp(glow_inner, glow_outer, (uint8_t)t);
            gfx_put_pixel(cx + dx, cy + dy, col);
        }
    }

    // Треугольник-логотип.
    draw_start_triangle(cx, cy + 1, side - 14, 0xFFFFFFFFu, glow_outer);
}

void startmenu_toggle(int taskbar_x, int taskbar_y_top) {
    if (menu_open) {
        menu_open = false;
        wm_require_redraw();
        return;
    }
    menu_x = taskbar_x;
    menu_y = taskbar_y_top - STARTMENU_HEIGHT - 8;
    if (menu_y < 10) menu_y = 10;
    menu_open = true;
    wm_require_redraw();
}

void startmenu_close(void) {
    if (!menu_open) return;
    menu_open = false;
    wm_require_redraw();
}

bool startmenu_is_open(void) {
    return menu_open;
}

bool startmenu_point_inside(int32_t mx, int32_t my) {
    if (!menu_open) return false;
    return mx >= menu_x && mx < menu_x + STARTMENU_WIDTH &&
           my >= menu_y && my < menu_y + STARTMENU_HEIGHT;
}

// Сколько «системных» пунктов в нижней панели меню (power-buttons).
// Для hit-testing раскладку считаем так же, как в startmenu_draw.
static int app_row_count(void) {
    int n = desktop_get_icon_count();
    if (n > 8) n = 8;   // защита: в меню помещается максимум столько
    return n;
}

static int list_area_top(void)    { return menu_y + 64; }
static int power_bar_y(void)      { return menu_y + STARTMENU_HEIGHT - 56; }
static int row_y(int idx)         { return list_area_top() + idx * STARTMENU_ROW_HEIGHT; }

void startmenu_draw(void) {
    if (!menu_open) return;

    // --- Тень под меню ---
    for (int d = 1; d < 14; d++) {
        int a = (14 - d) * (14 - d) * 90 / (14 * 14);
        if (a <= 0) continue;
        blend_rect_alpha(menu_x - d, menu_y - d,
                         STARTMENU_WIDTH + 2 * d, STARTMENU_HEIGHT + 2 * d,
                         0x000000, (uint8_t)a);
    }

    // --- Фон (тёмный с лёгкой прозрачностью) ---
    blend_rect_alpha(menu_x, menu_y, STARTMENU_WIDTH, STARTMENU_HEIGHT,
                     0x1B1F2A, 245);

    // --- Верхняя плашка: логотип + «Amplify OS» ---
    int header_h = 54;
    blend_rect_alpha(menu_x, menu_y, STARTMENU_WIDTH, header_h, 0x2A3148, 255);

    // Маленький треугольник-логотип слева
    draw_start_triangle(menu_x + 28, menu_y + header_h / 2 + 2, 28,
                        0xFFFFFFFF, 0xFFCC4488);

    // Текст заголовка
    font_draw_string(menu_x + 56, menu_y + 12, "Amplify OS",  // имя бренда
                     0xFFFFFFFF, 0xFF000000);
    font_draw_string(menu_x + 56, menu_y + 30, "Меню",
                     0xFFB8B8CC, 0xFF000000);

    // Разделительная линия
    gfx_hline(menu_x + 10, menu_y + header_h + 2, STARTMENU_WIDTH - 20, 0x3A4058);

    // --- Список приложений ---
    int n = app_row_count();
    for (int i = 0; i < n; i++) {
        // Данные иконки: используем те же name/symbol/color, что и на рабочем
        // столе. Для этого запросим их через desktop API.
        extern const char* desktop_get_icon_name(int idx);
        extern const char* desktop_get_icon_symbol(int idx);
        extern uint32_t    desktop_get_icon_color(int idx);
        const char* name = desktop_get_icon_name(i);
        const char* sym  = desktop_get_icon_symbol(i);
        uint32_t    col  = desktop_get_icon_color(i);
        if (!name) continue;

        int rx = menu_x + 10;
        int ry = row_y(i);

        // Квадратик с символом (24×24).
        int32_t box_w = 26;
        uint32_t light = color_lighten(col, 60);
        uint32_t dark  = color_darken(col, 180);
        gfx_fill_rect_gradient(rx, ry + 4, box_w, box_w, light, dark);
        int sym_w = font_string_width(sym);
        font_draw_string(rx + (box_w - sym_w) / 2, ry + 4 + (box_w - FONT_HEIGHT) / 2,
                         sym, 0xFFFFFFFF, 0xFF000000);

        // Текст справа от квадратика.
        font_draw_string(rx + box_w + 12, ry + 11, name,
                         0xFFF0F0F8, 0xFF000000);
    }

    // --- Разделитель перед power-bar ---
    int pby = power_bar_y();
    gfx_hline(menu_x + 10, pby - 6, STARTMENU_WIDTH - 20, 0x3A4058);

    // --- Power buttons: [Reboot] [Shutdown] ---
    int btn_w = (STARTMENU_WIDTH - 30) / 2;
    int btn_h = 40;

    // Reboot (левая кнопка, оранжевый)
    gfx_fill_rect_gradient(menu_x + 10, pby, btn_w, btn_h, 0xFF7744, 0xCC4422);
    gfx_draw_rect(menu_x + 10, pby, btn_w, btn_h, 0xFFAA77);
    font_draw_string(menu_x + 10 + (btn_w - font_string_width("Перезагр.")) / 2,
                     pby + (btn_h - FONT_HEIGHT) / 2,
                     "Перезагр.", 0xFFFFFFFF, 0xFF000000);

    // Shutdown (правая, красный)
    gfx_fill_rect_gradient(menu_x + 20 + btn_w, pby, btn_w, btn_h, 0xCC3344, 0x991122);
    gfx_draw_rect(menu_x + 20 + btn_w, pby, btn_w, btn_h, 0xDD6677);
    font_draw_string(menu_x + 20 + btn_w + (btn_w - font_string_width("Выкл.")) / 2,
                     pby + (btn_h - FONT_HEIGHT) / 2,
                     "Выкл.", 0xFFFFFFFF, 0xFF000000);
}

// Вызов app по индексу (открывает окно через desktop).
// Объявление снаружи: в desktop.c добавим helper.
extern bool desktop_launch_icon(int idx);

bool startmenu_handle_click(int32_t mx, int32_t my) {
    if (!menu_open) return false;
    if (!startmenu_point_inside(mx, my)) {
        // Клик вне меню — закрываем его и пусть событие идёт дальше.
        startmenu_close();
        return false;
    }

    // Клик по строке приложения?
    int n = app_row_count();
    for (int i = 0; i < n; i++) {
        int rx = menu_x + 10;
        int ry = row_y(i);
        if (mx >= rx && mx < menu_x + STARTMENU_WIDTH - 10 &&
            my >= ry && my < ry + STARTMENU_ROW_HEIGHT) {
            desktop_launch_icon(i);
            startmenu_close();
            return true;
        }
    }

    // Power-кнопки
    int pby = power_bar_y();
    int btn_w = (STARTMENU_WIDTH - 30) / 2;
    int btn_h = 40;
    // Reboot
    if (mx >= menu_x + 10 && mx < menu_x + 10 + btn_w &&
        my >= pby && my < pby + btn_h) {
        power_reboot_with_curtain();
    }
    // Shutdown
    if (mx >= menu_x + 20 + btn_w && mx < menu_x + 20 + btn_w + btn_w &&
        my >= pby && my < pby + btn_h) {
        power_shutdown_with_curtain();
    }

    return true;
}
