// ============================================================================
// window.c - Реализация окон для MyOS (Этап 22: compositing WM)
// ============================================================================
// Новый механизм отрисовки:
//   • Мягкая тень под окном — альфа-затухание от центра окна к краю shadow-box.
//   • Скруглённые углы — окно отсекается по rounded-rect маске.
//   • Per-pixel alpha в буфере окна + глобальная opacity — SRC-OVER компоузинг.
//   • Современный стиль заголовка: полупрозрачный фон, traffic-light закрытие.
// ============================================================================

#include "window.h"
#include "../memory/heap.h"
#include "graphics.h"
#include "font.h"
#include "doublebuf.h"
#include "framebuffer.h"

static int32_t next_window_id = 1;

// ----------------------------------------------------------------------------
// Мини-хелперы
// ----------------------------------------------------------------------------

static void str_copy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// (утилиты imin/imax/iabs больше не нужны — тень переписана без них)

// Блит в back-буфер с SRC-OVER смешиванием и scale=1:1. Формат пикселей
// 0xAARRGGBB. Если dst-пиксель вне экрана — молча пропускается.
static inline void blit_blend_pixel(int sx, int sy, uint32_t src) {
    if (sx < 0 || sy < 0) return;
    framebuffer_t* fb = framebuffer_get();
    if (sx >= (int)fb->width || sy >= (int)fb->height) return;
    uint32_t* buf = doublebuf_get_buffer();
    uint32_t  pitch = fb->pitch / 4;
    uint32_t  off = (uint32_t)sy * pitch + (uint32_t)sx;

    // Fast-path для полностью непрозрачных пикселей: пропускаем blend.
    uint32_t a = (src >> 24) & 0xFFu;
    if (a == 0xFFu) {
        buf[off] = src;
        return;
    }
    if (a == 0u) return;

    buf[off] = color_blend_over(src, buf[off]);
}

static void blit_const_alpha_pixel(int sx, int sy, uint32_t rgb, uint8_t a) {
    if (a == 0) return;
    blit_blend_pixel(sx, sy, (((uint32_t)a) << 24) | (rgb & 0x00FFFFFF));
}

// Маска-скругление: возвращает 255 (полностью видимо) внутри rounded-rect,
// 0 (прозрачно) снаружи, промежуточные значения на границе для сглаживания.
// Координаты lx, ly относительно rect (0..w-1, 0..h-1).
static uint8_t rounded_rect_mask(int lx, int ly, int w, int h, int r) {
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) return 0;
    if (r <= 0) return 255;

    // Определяем, в каком из 4-х углов мы находимся.
    int cx = -1, cy = -1;
    if (lx < r && ly < r)                 { cx = r - 1;       cy = r - 1; }
    else if (lx >= w - r && ly < r)       { cx = w - r;       cy = r - 1; }
    else if (lx < r && ly >= h - r)       { cx = r - 1;       cy = h - r; }
    else if (lx >= w - r && ly >= h - r)  { cx = w - r;       cy = h - r; }
    else return 255;   // не в углу — полностью видимо

    int dx = lx - cx;
    int dy = ly - cy;
    int d2 = dx * dx + dy * dy;
    int r2 = r * r;
    if (d2 > r2)        return 0;
    // Простое anti-aliasing на одной строке: близко к границе — убавляем.
    int edge = r2 - d2;           // 0 на краю, r^2 в центре
    if (edge < r * 2) {
        // примерно линейно
        return (uint8_t)((edge * 255) / (r * 2));
    }
    return 255;
}

// ----------------------------------------------------------------------------
// Создание / уничтожение
// ----------------------------------------------------------------------------

window_t* window_create(const char* title, int32_t x, int32_t y, int32_t w, int32_t h) {
    window_t* win = (window_t*)kmalloc(sizeof(window_t));
    if (!win) return NULL;

    win->id = next_window_id++;
    str_copy(win->title, title, MAX_WINDOW_TITLE);

    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;

    win->opacity = 0xFF;
    win->is_focused = false;
    win->is_dragging = false;
    win->wants_mouse_drag = false;
    win->close_requested = false;
    win->is_maximized = false;
    win->is_minimized = false;
    win->drag_offset_x = 0;
    win->drag_offset_y = 0;
    win->saved_x = x;
    win->saved_y = y;
    win->saved_w = w;
    win->saved_h = h;
    win->next = NULL;
    win->prev = NULL;

    uint32_t buffer_size = (uint32_t)w * (uint32_t)h * sizeof(uint32_t);
    win->buffer = (uint32_t*)kmalloc(buffer_size);
    if (!win->buffer) { kfree(win); return NULL; }

    window_fill(win, 0xFFFFFFFF);
    return win;
}

void window_destroy(window_t* win) {
    if (!win) return;
    if (win->buffer) kfree(win->buffer);
    kfree(win);
}

void window_put_pixel(window_t* win, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || x >= win->width || y < 0 || y >= win->height) return;
    win->buffer[y * win->width + x] = color;
}

void window_fill(window_t* win, uint32_t color) {
    int count = win->width * win->height;
    for (int i = 0; i < count; i++) win->buffer[i] = color;
}

// ----------------------------------------------------------------------------
// Traffic lights (close / minimize / maximize) в левом верхнем углу
// ----------------------------------------------------------------------------
//
// Три круглые кнопки в стиле macOS. Расположение слева направо:
//   close (красный)  → minimize (жёлтый)  → maximize (зелёный)
// Центры: BTN_SPACING пикселей друг от друга, радиус 7 px, верт.центр = title/2.
#define TRAFFIC_BTN_R      (WINDOW_CLOSE_BTN_SIZE / 2)
#define TRAFFIC_BTN_MARGIN 10       // отступ первой кнопки от левого края
#define TRAFFIC_BTN_GAP    8        // расстояние между центрами кнопок

static inline int32_t traffic_cx(window_t* win, int idx) {
    return win->x + TRAFFIC_BTN_MARGIN + TRAFFIC_BTN_R
           + idx * (WINDOW_CLOSE_BTN_SIZE + TRAFFIC_BTN_GAP);
}

static inline int32_t traffic_cy(window_t* win) {
    return win->y + WINDOW_TITLE_HEIGHT / 2;
}

static bool traffic_hit(window_t* win, int idx, int32_t mx, int32_t my) {
    int32_t cx = traffic_cx(win, idx);
    int32_t cy = traffic_cy(win);
    int32_t dx = mx - cx;
    int32_t dy = my - cy;
    int32_t r  = TRAFFIC_BTN_R + 2;
    return (dx * dx + dy * dy) <= r * r;
}

bool window_is_close_button_clicked(window_t* win, int32_t mx, int32_t my) {
    if (!win) return false;
    return traffic_hit(win, 0, mx, my);
}

bool window_is_minimize_button_clicked(window_t* win, int32_t mx, int32_t my) {
    if (!win) return false;
    return traffic_hit(win, 1, mx, my);
}

bool window_is_maximize_button_clicked(window_t* win, int32_t mx, int32_t my) {
    if (!win) return false;
    return traffic_hit(win, 2, mx, my);
}

// Вспомогательная: реально ресайзим пиксельный буфер окна до new_w×new_h.
// Возвращает true при успехе; содержимое нового буфера очищается (белым),
// приложение обязано перерисовать контент через on_create-хук.
static bool window_resize_buffer(window_t* win, int32_t new_w, int32_t new_h) {
    if (new_w <= 0 || new_h <= 0) return false;
    uint32_t new_size = (uint32_t)new_w * (uint32_t)new_h * sizeof(uint32_t);
    uint32_t* new_buf = (uint32_t*)krealloc(win->buffer, new_size);
    if (!new_buf) return false;
    win->buffer = new_buf;
    win->width  = new_w;
    win->height = new_h;
    // Инициализация нового буфера (белый фон — совместим с
    // window_fill() в начале большинства app-draw колбэков).
    int count = new_w * new_h;
    for (int i = 0; i < count; i++) new_buf[i] = 0xFFFFFFFF;
    return true;
}

void window_toggle_maximize(window_t* win) {
    if (!win) return;
    framebuffer_t* fb = framebuffer_get();
    int32_t screen_w = (int32_t)fb->width;
    int32_t screen_h = (int32_t)fb->height;

    if (!win->is_maximized) {
        // Сохраняем текущую геометрию
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->width;
        win->saved_h = win->height;

        // Максимальный размер: от верха экрана до верха taskbar.
        // width окна включает рамки/title → берём весь экран.
        // Контент-область приложения = height - WINDOW_TITLE_HEIGHT.
        int32_t max_w = screen_w;
        int32_t max_h = screen_h - TASKBAR_HEIGHT;
        if (max_w <= 0 || max_h <= 0) return;

        if (!window_resize_buffer(win, max_w, max_h)) {
            // Не смогли выделить новый буфер — не меняем состояние
            return;
        }
        win->x = 0;
        win->y = 0;
        win->is_maximized = true;
    } else {
        // Возврат к прежним размерам
        if (!window_resize_buffer(win, win->saved_w, win->saved_h)) return;
        win->x = win->saved_x;
        win->y = win->saved_y;
        win->is_maximized = false;
    }
    (void)screen_w; (void)screen_h;
}

void window_set_minimized(window_t* win, bool minimized) {
    if (!win) return;
    win->is_minimized = minimized;
    if (minimized) {
        // Свёрнутое окно теряет фокус
        win->is_focused = false;
    }
}

// ----------------------------------------------------------------------------
// Мягкая тень (Этап 22)
// ----------------------------------------------------------------------------
// Рисуется снаружи прямоугольника окна как «гало» из 4 полос (top/bottom/
// left/right) + 4 уголков. Обходим только реально видимые пиксели тени,
// а не весь ограничивающий прямоугольник (было ~280K итераций, стало ~40K).
//
// Профиль alpha зависит от расстояния до ближайшего края окна:
//   alpha(d) = 120 * ((S-d)/S)^2,   d ∈ (0..S)
// Для скругления в углах используем евклидово расстояние (с аппроксимацией
// sqrt через max + min/2).
static inline int shadow_alpha_from_dist(int d, int S) {
    if (d <= 0 || d >= S) return 0;
    int a = (S - d);
    a = (a * a * 120) / (S * S);
    return a > 120 ? 120 : a;
}

// Inline-помощник: смешать «чёрный пиксель с alpha a» с dst-пикселем в buf.
// Границы клипуются вызывающей стороной.
static inline void shadow_blend_at(uint32_t* buf, uint32_t pitch_px,
                                   int fbw, int fbh, int x, int y, uint32_t a) {
    if ((unsigned)x >= (unsigned)fbw) return;
    if ((unsigned)y >= (unsigned)fbh) return;
    uint32_t* p = buf + (uint32_t)y * pitch_px + (uint32_t)x;
    uint32_t dst = *p;
    // src = чёрный (RGB = 0) с альфой a. Формула SRC-OVER упрощается:
    //   out_rgb = dst_rgb * (255 - a) / 255
    uint32_t inv = 255 - a;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db =  dst        & 0xFF;
    dr = (dr * inv) / 255;
    dg = (dg * inv) / 255;
    db = (db * inv) / 255;
    *p = 0xFF000000u | (dr << 16) | (dg << 8) | db;
}

static void draw_window_shadow(int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    const int S  = WINDOW_SHADOW_SIZE;
    const int OY = WINDOW_SHADOW_OFFSET;

    framebuffer_t* fb = framebuffer_get();
    uint32_t* buf      = doublebuf_get_buffer();
    uint32_t  pitch_px = fb->pitch / 4;
    int       fbw      = (int)fb->width;
    int       fbh      = (int)fb->height;

    int rx0 = wx;
    int ry0 = wy + OY;
    int rx1 = wx + ww;           // exclusive
    int ry1 = wy + wh + OY;      // exclusive

    // --- Верхняя и нижняя полосы ---
    for (int d = 1; d < S; d++) {
        int a = shadow_alpha_from_dist(d, S);
        if (a == 0) continue;

        int yt = ry0 - d;
        int yb = ry1 - 1 + d;
        for (int x = rx0; x < rx1; x++) {
            shadow_blend_at(buf, pitch_px, fbw, fbh, x, yt, (uint32_t)a);
            shadow_blend_at(buf, pitch_px, fbw, fbh, x, yb, (uint32_t)a);
        }
    }

    // --- Левая и правая полосы ---
    for (int d = 1; d < S; d++) {
        int a = shadow_alpha_from_dist(d, S);
        if (a == 0) continue;

        int xl = rx0 - d;
        int xr = rx1 - 1 + d;
        for (int y = ry0; y < ry1; y++) {
            shadow_blend_at(buf, pitch_px, fbw, fbh, xl, y, (uint32_t)a);
            shadow_blend_at(buf, pitch_px, fbw, fbh, xr, y, (uint32_t)a);
        }
    }

    // --- 4 уголка ---
    for (int dy = 1; dy < S; dy++) {
        for (int dx = 1; dx < S; dx++) {
            int mn = (dx < dy) ? dx : dy;
            int mx = (dx > dy) ? dx : dy;
            int d  = mx + mn / 2;
            int a  = shadow_alpha_from_dist(d, S);
            if (a == 0) continue;

            shadow_blend_at(buf, pitch_px, fbw, fbh, rx0 - dx,     ry0 - dy,     (uint32_t)a);
            shadow_blend_at(buf, pitch_px, fbw, fbh, rx1 - 1 + dx, ry0 - dy,     (uint32_t)a);
            shadow_blend_at(buf, pitch_px, fbw, fbh, rx0 - dx,     ry1 - 1 + dy, (uint32_t)a);
            shadow_blend_at(buf, pitch_px, fbw, fbh, rx1 - 1 + dx, ry1 - 1 + dy, (uint32_t)a);
        }
    }
}

// ----------------------------------------------------------------------------
// Заголовок
// ----------------------------------------------------------------------------

static void draw_title_bar(window_t* win) {
    // Современный градиент: нежный сверху, приглушённый снизу.
    // Для фокуса — слегка холодный синий, иначе — серый.
    uint32_t bg_top, bg_bot;
    if (win->is_focused) {
        bg_top = 0xF6F8FAu; bg_bot = 0xDDE3EAu;
    } else {
        bg_top = 0xEFEFEFu; bg_bot = 0xD7D7D7u;
    }

    for (int ty = 0; ty < WINDOW_TITLE_HEIGHT; ty++) {
        int sy = win->y + ty;
        uint8_t grad_t = (uint8_t)((ty * 255) / (WINDOW_TITLE_HEIGHT - 1));
        uint32_t row_col = color_lerp(bg_top, bg_bot, grad_t) | 0xFF000000;

        for (int tx = 0; tx < win->width; tx++) {
            int sx = win->x + tx;
            // Маска скруглённых углов для верхней части.
            uint8_t m = rounded_rect_mask(tx, ty, win->width,
                                          win->height + WINDOW_TITLE_HEIGHT,
                                          WINDOW_CORNER_RADIUS);
            if (m == 0) continue;
            uint32_t src = (((uint32_t)m) << 24) | (row_col & 0x00FFFFFF);
            blit_blend_pixel(sx, sy, src);
        }
    }

    // Traffic-light кнопки (3): close/minimize/maximize. Цвета в стиле macOS
    // с небольшим градиентом (центр ярче, край тёмнее). Когда окно не в
    // фокусе — все кнопки приглушённо-серые.
    uint32_t btn_colors[3][2] = {
        // { center, edge }
        { 0xFF5F57u, 0xDD4A3Cu },   // close  — красный
        { 0xFEBC2Eu, 0xE5A421u },   // min    — жёлтый
        { 0x28C940u, 0x1FA632u },   // max    — зелёный
    };
    uint32_t btn_off_c = 0xC0C3C7u;
    uint32_t btn_off_e = 0xA7ABB0u;

    int r = TRAFFIC_BTN_R;
    for (int b = 0; b < 3; b++) {
        int32_t cx = traffic_cx(win, b);
        int32_t cy = traffic_cy(win);
        uint32_t ccol = win->is_focused ? btn_colors[b][0] : btn_off_c;
        uint32_t ecol = win->is_focused ? btn_colors[b][1] : btn_off_e;

        for (int dy = -r - 1; dy <= r + 1; dy++) {
            for (int dx = -r - 1; dx <= r + 1; dx++) {
                int d2 = dx * dx + dy * dy;
                int r2 = r * r;
                if (d2 > (r + 1) * (r + 1)) continue;
                uint32_t col;
                if (d2 <= r2 - r)      col = ccol;
                else if (d2 <= r2)     col = ecol;
                else {
                    blit_const_alpha_pixel(cx + dx, cy + dy, ecol,
                        (uint8_t)(255 - ((d2 - r2) * 255) / (2 * r + 1)));
                    continue;
                }
                blit_blend_pixel(cx + dx, cy + dy, 0xFF000000 | col);
            }
        }
    }

    // Hover-глифы на кнопках (крестик / минус / плюс) — рисуем всегда, чтобы
    // пользователь понимал назначение. При расфокусе — глухой серый.
    if (win->is_focused) {
        uint32_t glyph_col = 0x300000u; // тёмный контраст внутри traffic light
        // close: ×
        int32_t c0x = traffic_cx(win, 0);
        int32_t c0y = traffic_cy(win);
        for (int i = -3; i <= 3; i++) {
            blit_blend_pixel(c0x + i, c0y + i, 0xFF000000 | glyph_col);
            blit_blend_pixel(c0x + i, c0y - i, 0xFF000000 | glyph_col);
        }
        // minimize: —
        int32_t c1x = traffic_cx(win, 1);
        int32_t c1y = traffic_cy(win);
        for (int i = -3; i <= 3; i++) {
            blit_blend_pixel(c1x + i, c1y, 0xFF000000 | glyph_col);
        }
        // maximize: + (плюс — значит можно развернуть; когда развернут
        // рисуем — чтобы обозначить, что это «restore»)
        int32_t c2x = traffic_cx(win, 2);
        int32_t c2y = traffic_cy(win);
        if (win->is_maximized) {
            // restore glyph: две маленьких квадратные рамки (кадр-в-кадре)
            for (int i = -3; i <= 1; i++) {
                blit_blend_pixel(c2x + i, c2y - 3, 0xFF000000 | glyph_col);
                blit_blend_pixel(c2x + i, c2y + 1, 0xFF000000 | glyph_col);
            }
            for (int i = -3; i <= 1; i++) {
                blit_blend_pixel(c2x - 3, c2y + i + 1, 0xFF000000 | glyph_col);
                blit_blend_pixel(c2x + 1, c2y + i + 1, 0xFF000000 | glyph_col);
            }
        } else {
            for (int i = -3; i <= 3; i++) {
                blit_blend_pixel(c2x + i, c2y, 0xFF000000 | glyph_col);
                blit_blend_pixel(c2x, c2y + i, 0xFF000000 | glyph_col);
            }
        }
    }

    // Текст заголовка — за тремя кнопками, центрируем по вертикали.
    int32_t tx_start = traffic_cx(win, 2) + WINDOW_CLOSE_BTN_SIZE + 6;
    int32_t ty_start = win->y + (WINDOW_TITLE_HEIGHT - FONT_HEIGHT) / 2;
    uint32_t txt_col = win->is_focused ? 0x203040u : 0x606060u;
    font_draw_string(tx_start, ty_start, win->title, txt_col, 0xFF000000);
}

// ----------------------------------------------------------------------------
// Отрисовка окна (тень → заголовок → контент) с compositing-blending
// ----------------------------------------------------------------------------

void window_draw_to_doublebuf(window_t* win) {
    if (!win || !doublebuf_is_initialized()) return;

    int32_t W = win->width;
    int32_t H = win->height;
    int32_t fullH = H + WINDOW_TITLE_HEIGHT;
    const int R = WINDOW_CORNER_RADIUS;

    // 1) Тень.
    draw_window_shadow(win->x, win->y, W, fullH);

    // 2) Заголовок со скруглёнными верхними углами.
    draw_title_bar(win);

    // 3) Контент окна (под заголовком). Делим по строкам на два режима:
    //     а) «серединные» строки (ly < fullH-R) — маска всюду 255 → быстрый
    //        путь: прямое копирование строки пикселей (rep movsl при
    //        opacity=255, либо простой цикл с blend при полупрозрачности);
    //     б) нижние R строк (закругляют нижние углы) — вызываем полноценный
    //        per-pixel маскирующий путь.
    framebuffer_t* fb = framebuffer_get();
    uint32_t*   back     = doublebuf_get_buffer();
    uint32_t    pitch_px = fb->pitch / 4;
    int         fbw      = (int)fb->width;
    int         fbh      = (int)fb->height;
    uint8_t     wop      = win->opacity;     // глобальная opacity окна

    for (int wy = 0; wy < H; wy++) {
        int sy = win->y + WINDOW_TITLE_HEIGHT + wy;
        if (sy < 0 || sy >= fbh) continue;
        int ly = wy + WINDOW_TITLE_HEIGHT;

        bool corner_row = (ly >= fullH - R);   // строка пересекает нижний угол
        uint32_t* dst_row = back + (uint32_t)sy * pitch_px;
        uint32_t* src_row = win->buffer + wy * W;

        if (!corner_row && wop == 0xFF) {
            // Fast path: полностью непрозрачное окно, строка без округлений.
            // Клипуем по X и копируем через rep movsl.
            int sx0 = win->x;
            int sx1 = win->x + W;
            int wx0 = 0;
            if (sx0 < 0)   { wx0 = -sx0; sx0 = 0; }
            if (sx1 > fbw) { sx1 = fbw; }
            int run = sx1 - sx0;
            if (run <= 0) continue;
            uint32_t* dst = dst_row + sx0;
            uint32_t* src = src_row + wx0;
            uint32_t count = (uint32_t)run;
            __asm__ volatile(
                "cld\n\t"
                "rep movsl\n\t"
                : "+D"(dst), "+S"(src), "+c"(count)
                : : "memory"
            );
            continue;
        }

        // Slow path: per-pixel (нужны маска закруглений или opacity).
        for (int wx = 0; wx < W; wx++) {
            int sx = win->x + wx;
            if (sx < 0 || sx >= fbw) continue;

            uint8_t m = corner_row
                ? rounded_rect_mask(wx, ly, W, fullH, R)
                : 255;
            if (m == 0) continue;

            uint32_t pix = src_row[wx];
            uint32_t a_pix = (pix >> 24) & 0xFF;
            if (a_pix == 0) a_pix = 0xFF;

            uint32_t a = (uint32_t)a_pix * wop / 255;
            a = a * m / 255;
            if (a == 0) continue;

            uint32_t src_px = (a << 24) | (pix & 0x00FFFFFF);
            if (a == 0xFF) {
                dst_row[sx] = src_px;
            } else {
                dst_row[sx] = color_blend_over(src_px, dst_row[sx]);
            }
        }
    }

    // hairline
    if (win->is_focused) {
        uint32_t border = 0xB8BCC0u;
        for (int tx = 0; tx < W; tx++) {
            uint8_t m = rounded_rect_mask(tx, 0, W, fullH, R);
            if (m > 0) blit_const_alpha_pixel(win->x + tx, win->y, border, m);
        }
    }
}
