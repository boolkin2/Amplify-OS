// ============================================================================
// graphics.h - Графические примитивы для MyOS
// ============================================================================
// Функции рисования поверх фреймбуфера / двойного буфера:
//   - put_pixel     — установка одного пикселя
//   - draw_line     — линия (алгоритм Брезенхэма)
//   - draw_rect     — прямоугольник (только контур)
//   - fill_rect     — залитый прямоугольник
//   - draw_circle   — окружность (алгоритм Брезенхэма)
//   - fill_circle   — залитый круг
// ============================================================================

#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

// ============================================================================
// Макросы для создания цветов (формат 0x00RRGGBB)
// ============================================================================

#define RGB(r, g, b)    ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

// ============================================================================
// Предопределённые цвета
// ============================================================================

#define COLOR_BLACK         0x00000000
#define COLOR_WHITE         0x00FFFFFF
#define COLOR_RED           0x00FF0000
#define COLOR_GREEN         0x0000FF00
#define COLOR_BLUE          0x000000FF
#define COLOR_YELLOW        0x00FFFF00
#define COLOR_CYAN          0x0000FFFF
#define COLOR_MAGENTA       0x00FF00FF
#define COLOR_ORANGE        0x00FF8C00
#define COLOR_DARK_RED      0x008B0000
#define COLOR_DARK_GREEN    0x00006400
#define COLOR_DARK_BLUE     0x0000008B
#define COLOR_DARK_CYAN     0x00008B8B
#define COLOR_DARK_GREY     0x00404040
#define COLOR_GREY          0x00808080
#define COLOR_LIGHT_GREY    0x00C0C0C0
#define COLOR_DARK_SLATE    0x002F4F4F
#define COLOR_STEEL_BLUE    0x004682B4
#define COLOR_NAVY          0x00000080
#define COLOR_TEAL          0x00008080
#define COLOR_PURPLE        0x00800080
#define COLOR_OLIVE         0x00808000
#define COLOR_BROWN         0x008B4513
#define COLOR_SILVER        0x00C0C0C0
#define COLOR_GOLD          0x00FFD700
#define COLOR_SKY_BLUE      0x0087CEEB
#define COLOR_CORAL         0x00FF7F50
#define COLOR_PINK          0x00FFC0CB
#define COLOR_LIME          0x0032CD32

// Цвета рабочего стола
#define COLOR_DESKTOP_BG    0x00206090  // Синий фон рабочего стола
#define COLOR_TASKBAR_BG    0x00303030  // Тёмно-серая панель задач
#define COLOR_TITLE_BAR     0x003366AA  // Синий заголовок окна
#define COLOR_TITLE_BAR_INACTIVE 0x00666666  // Серый заголовок неактивного окна
#define COLOR_WINDOW_BG     0x00F0F0F0  // Светлый фон окна
#define COLOR_WINDOW_BORDER 0x00808080  // Серая рамка окна

// ============================================================================
// Функции графических примитивов
// ============================================================================

// Установка пикселя в буфер отрисовки (двойной буфер или фреймбуфер)
void gfx_put_pixel(int32_t x, int32_t y, uint32_t color);

// Рисование горизонтальной линии (оптимизированная)
// Параметры: x, y — начало; w — ширина; color — цвет
void gfx_hline(int32_t x, int32_t y, int32_t w, uint32_t color);

// Рисование вертикальной линии (оптимизированная)
void gfx_vline(int32_t x, int32_t y, int32_t h, uint32_t color);

// Рисование произвольной линии (алгоритм Брезенхэма)
// Параметры: x1,y1 — начало; x2,y2 — конец; color — цвет
void gfx_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);

// Рисование контура прямоугольника
// Параметры: x,y — верхний левый угол; w,h — размер; color — цвет
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// Рисование залитого прямоугольника
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

// Рисование контура окружности (алгоритм Брезенхэма)
// Параметры: cx,cy — центр; r — радиус; color — цвет
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color);

// Рисование залитого круга
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color);

// Рисование градиентного прямоугольника (вертикальный градиент)
// Параметры: x,y — верхний левый угол; w,h — размер;
//            color_top — цвет верха; color_bottom — цвет низа
void gfx_fill_rect_gradient(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color_top, uint32_t color_bottom);

// ============================================================================
// Вспомогательные функции для работы с цветом
// ============================================================================

// Линейная интерполяция двух цветов
// t = 0 → color1, t = 255 → color2
uint32_t color_lerp(uint32_t color1, uint32_t color2, uint8_t t);

// Затемнение цвета (factor: 0 = чёрный, 255 = без изменений)
uint32_t color_darken(uint32_t color, uint8_t factor);

// Осветление цвета (factor: 0 = без изменений, 255 = белый)
uint32_t color_lighten(uint32_t color, uint8_t factor);

// ============================================================================
// Alpha-compositing (Этап 22)
// ============================================================================

// «Source Over Destination» — классический SVG/CSS blending.
// src имеет формат 0xAARRGGBB (AA — альфа от 0 = прозрачно до 255 = непрозрачно).
// dst тоже 0xAARRGGBB; альфа результата объединяется как 1 - (1-a_s)(1-a_d).
// Возвращает смешанный пиксель в том же формате.
uint32_t color_blend_over(uint32_t src, uint32_t dst);

// Упрощённая версия: смешать с фиксированной альфой (альфа канала src игнорируется).
// alpha = 0 → полностью dst; alpha = 255 → полностью src.
uint32_t color_blend_alpha(uint32_t src, uint32_t dst, uint8_t alpha);

#endif // GRAPHICS_H
