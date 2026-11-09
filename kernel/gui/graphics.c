// ============================================================================
// graphics.c - Реализация графических примитивов для MyOS
// ============================================================================
// Все функции рисуют в двойной буфер (back buffer), а не напрямую
// во фреймбуфер. Для отображения на экране нужно вызвать swap_buffers().
// ============================================================================

#include "graphics.h"
#include "framebuffer.h"
#include "doublebuf.h"

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Модуль числа (абсолютное значение)
static inline int32_t gfx_abs(int32_t x) {
    return x < 0 ? -x : x;
}

// ============================================================================
// Установка пикселя
// ============================================================================

// Установка пикселя в буфер отрисовки
void gfx_put_pixel(int32_t x, int32_t y, uint32_t color) {
    // Рисуем в двойной буфер, если он доступен, иначе напрямую
    if (doublebuf_is_initialized()) {
        doublebuf_put_pixel((uint32_t)x, (uint32_t)y, color);
    } else {
        framebuffer_put_pixel((uint32_t)x, (uint32_t)y, color);
    }
}

// ============================================================================
// Оптимизированные линии
// ============================================================================

// Горизонтальная линия (очень быстрая — одна строка memset)
void gfx_hline(int32_t x, int32_t y, int32_t w, uint32_t color) {
    if (w <= 0) return;

    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized) return;

    // Проверка вертикальных границ
    if (y < 0 || y >= (int32_t)fb->height) return;

    // Обрезка по горизонтали
    if (x < 0) { w += x; x = 0; }
    if (x + w > (int32_t)fb->width) { w = (int32_t)fb->width - x; }
    if (w <= 0) return;

    // Получаем указатель на буфер отрисовки
    uint32_t* buffer;
    uint32_t pitch_px;

    if (doublebuf_is_initialized()) {
        buffer = doublebuf_get_buffer();
        pitch_px = fb->pitch / 4;
    } else {
        buffer = fb->address;
        pitch_px = fb->pitch / 4;
    }

    // Заполняем строку через rep stosl — существенно быстрее, чем цикл C
    // (один микрооп на 32-битный пиксель в QEMU).
    uint32_t* row = buffer + y * pitch_px + x;
    uint32_t count = (uint32_t)w;
    __asm__ volatile(
        "cld\n\t"
        "rep stosl\n\t"
        : "+D"(row), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

// Вертикальная линия
void gfx_vline(int32_t x, int32_t y, int32_t h, uint32_t color) {
    if (h <= 0) return;

    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized) return;

    // Проверка горизонтальных границ
    if (x < 0 || x >= (int32_t)fb->width) return;

    // Обрезка по вертикали
    if (y < 0) { h += y; y = 0; }
    if (y + h > (int32_t)fb->height) { h = (int32_t)fb->height - y; }
    if (h <= 0) return;

    uint32_t* buffer;
    uint32_t pitch_px;

    if (doublebuf_is_initialized()) {
        buffer = doublebuf_get_buffer();
        pitch_px = fb->pitch / 4;
    } else {
        buffer = fb->address;
        pitch_px = fb->pitch / 4;
    }

    uint32_t* pixel = buffer + y * pitch_px + x;
    for (int32_t i = 0; i < h; i++) {
        *pixel = color;
        pixel += pitch_px;
    }
}

// ============================================================================
// Линия (алгоритм Брезенхэма)
// ============================================================================

void gfx_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    // Оптимизация для горизонтальных линий
    if (y1 == y2) {
        int32_t start_x = x1 < x2 ? x1 : x2;
        int32_t w = gfx_abs(x2 - x1) + 1;
        gfx_hline(start_x, y1, w, color);
        return;
    }

    // Оптимизация для вертикальных линий
    if (x1 == x2) {
        int32_t start_y = y1 < y2 ? y1 : y2;
        int32_t h = gfx_abs(y2 - y1) + 1;
        gfx_vline(x1, start_y, h, color);
        return;
    }

    // Алгоритм Брезенхэма для произвольной линии
    int32_t dx = gfx_abs(x2 - x1);
    int32_t dy = -gfx_abs(y2 - y1);
    int32_t sx = x1 < x2 ? 1 : -1;
    int32_t sy = y1 < y2 ? 1 : -1;
    int32_t err = dx + dy;

    while (1) {
        gfx_put_pixel(x1, y1, color);

        if (x1 == x2 && y1 == y2) break;

        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

// ============================================================================
// Прямоугольники
// ============================================================================

// Контур прямоугольника
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    // Верхняя сторона
    gfx_hline(x, y, w, color);
    // Нижняя сторона
    gfx_hline(x, y + h - 1, w, color);
    // Левая сторона
    gfx_vline(x, y, h, color);
    // Правая сторона
    gfx_vline(x + w - 1, y, h, color);
}

// Залитый прямоугольник (оптимизированный — построчное заполнение)
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized) return;

    // Обрезка по границам экрана (clipping)
    int32_t x1 = x;
    int32_t y1 = y;
    int32_t x2 = x + w;
    int32_t y2 = y + h;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)fb->width)  x2 = (int32_t)fb->width;
    if (y2 > (int32_t)fb->height) y2 = (int32_t)fb->height;

    if (x1 >= x2 || y1 >= y2) return;

    int32_t cw = x2 - x1;  // Обрезанная ширина

    uint32_t* buffer;
    uint32_t pitch_px;

    if (doublebuf_is_initialized()) {
        buffer = doublebuf_get_buffer();
        pitch_px = fb->pitch / 4;
    } else {
        buffer = fb->address;
        pitch_px = fb->pitch / 4;
    }

    // Заполняем построчно через rep stosl — в ~10 раз быстрее C-цикла.
    for (int32_t row = y1; row < y2; row++) {
        uint32_t* dst = buffer + row * pitch_px + x1;
        uint32_t count = (uint32_t)cw;
        __asm__ volatile(
            "cld\n\t"
            "rep stosl\n\t"
            : "+D"(dst), "+c"(count)
            : "a"(color)
            : "memory"
        );
    }
}

// ============================================================================
// Окружность (алгоритм Брезенхэма для окружности)
// ============================================================================

void gfx_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    if (r <= 0) return;

    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    // Рисуем 8 симметричных точек одновременно
    while (x <= y) {
        gfx_put_pixel(cx + x, cy + y, color);
        gfx_put_pixel(cx - x, cy + y, color);
        gfx_put_pixel(cx + x, cy - y, color);
        gfx_put_pixel(cx - x, cy - y, color);
        gfx_put_pixel(cx + y, cy + x, color);
        gfx_put_pixel(cx - y, cy + x, color);
        gfx_put_pixel(cx + y, cy - x, color);
        gfx_put_pixel(cx - y, cy - x, color);

        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

// Залитый круг
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    if (r <= 0) return;

    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    // Заполняем горизонтальными линиями (для каждого ряда)
    while (x <= y) {
        // Горизонтальные линии для 4 рядов (используя симметрию)
        gfx_hline(cx - x, cy + y, 2 * x + 1, color);
        gfx_hline(cx - x, cy - y, 2 * x + 1, color);
        gfx_hline(cx - y, cy + x, 2 * y + 1, color);
        gfx_hline(cx - y, cy - x, 2 * y + 1, color);

        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

// ============================================================================
// Градиентный прямоугольник
// ============================================================================

void gfx_fill_rect_gradient(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color_top, uint32_t color_bottom) {
    if (w <= 0 || h <= 0) return;

    for (int32_t row = 0; row < h; row++) {
        // Коэффициент интерполяции: 0 (верх) → 255 (низ)
        uint8_t t = (uint8_t)((row * 255) / (h - 1 > 0 ? h - 1 : 1));
        uint32_t row_color = color_lerp(color_top, color_bottom, t);
        gfx_hline(x, y + row, w, row_color);
    }
}

// ============================================================================
// Функции работы с цветом
// ============================================================================

// Линейная интерполяция двух цветов
uint32_t color_lerp(uint32_t color1, uint32_t color2, uint8_t t) {
    // Извлекаем компоненты
    uint8_t r1 = (color1 >> 16) & 0xFF;
    uint8_t g1 = (color1 >> 8)  & 0xFF;
    uint8_t b1 =  color1        & 0xFF;

    uint8_t r2 = (color2 >> 16) & 0xFF;
    uint8_t g2 = (color2 >> 8)  & 0xFF;
    uint8_t b2 =  color2        & 0xFF;

    // Интерполяция: result = c1 + (c2 - c1) * t / 255
    uint8_t r = r1 + (((int32_t)r2 - (int32_t)r1) * t) / 255;
    uint8_t g = g1 + (((int32_t)g2 - (int32_t)g1) * t) / 255;
    uint8_t b = b1 + (((int32_t)b2 - (int32_t)b1) * t) / 255;

    return RGB(r, g, b);
}

// Затемнение цвета
uint32_t color_darken(uint32_t color, uint8_t factor) {
    uint8_t r = ((color >> 16) & 0xFF) * factor / 255;
    uint8_t g = ((color >> 8)  & 0xFF) * factor / 255;
    uint8_t b = ( color        & 0xFF) * factor / 255;
    return RGB(r, g, b);
}

// Осветление цвета
uint32_t color_lighten(uint32_t color, uint8_t factor) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8)  & 0xFF;
    uint8_t b =  color        & 0xFF;

    r = r + ((255 - r) * factor) / 255;
    g = g + ((255 - g) * factor) / 255;
    b = b + ((255 - b) * factor) / 255;

    return RGB(r, g, b);
}

// ============================================================================
// Alpha-compositing (Этап 22)
// ============================================================================
//
// Формат: 0xAARRGGBB.
//   AA = alpha (0 = прозрачно, 255 = непрозрачно)
//   RR/GG/BB — компоненты (не премультиплицированы).
//
// Все кадры в нашем front/back буфере хранят непрозрачные пиксели, так что
// alpha dst по умолчанию 0xFF. Мы просто смешиваем цвета по формуле src-over.

uint32_t color_blend_over(uint32_t src, uint32_t dst) {
    uint32_t as = (src >> 24) & 0xFF;
    if (as == 0)   return dst;
    if (as == 255) return (src & 0x00FFFFFF) | 0xFF000000;

    uint32_t inv = 255 - as;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb =  src        & 0xFF;

    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db =  dst        & 0xFF;

    // out = (src * as + dst * (255-as)) / 255
    uint32_t r = (sr * as + dr * inv) / 255;
    uint32_t g = (sg * as + dg * inv) / 255;
    uint32_t b = (sb * as + db * inv) / 255;

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

uint32_t color_blend_alpha(uint32_t src, uint32_t dst, uint8_t alpha) {
    if (alpha == 0)   return dst;
    if (alpha == 255) return (src & 0x00FFFFFF) | 0xFF000000;

    uint32_t inv = 255 - alpha;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb =  src        & 0xFF;

    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db =  dst        & 0xFF;

    uint32_t r = (sr * alpha + dr * inv) / 255;
    uint32_t g = (sg * alpha + dg * inv) / 255;
    uint32_t b = (sb * alpha + db * inv) / 255;

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}
