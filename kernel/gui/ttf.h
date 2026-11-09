// ============================================================================
// ttf.h - Рендеринг TrueType-шрифтов (.ttf) для MyOS (Этап 21)
// ============================================================================
// Высокоуровневая обёртка над stb_truetype:
//   - загрузка шрифта из памяти (буфер байт .ttf)
//   - рендеринг строк произвольного размера со сглаживанием (alpha-coverage)
//   - измерение ширины строки
// ============================================================================

#ifndef TTF_H
#define TTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Непрозрачный тип — внутри хранит stbtt_fontinfo и владеемый буфер шрифта
typedef struct ttf_font ttf_font_t;

// ============================================================================
// Инициализация подсистемы TTF
// ============================================================================

// Вызывается один раз при старте ядра. Пытается загрузить шрифт по умолчанию
// из initrd (путь /default.ttf или /fonts/default.ttf).
// Возвращает true если системный шрифт загружен.
bool ttf_system_init(void);

// Получение системного (дефолтного) шрифта. Может вернуть NULL.
ttf_font_t* ttf_get_system_font(void);

// ============================================================================
// API загрузки / выгрузки шрифта
// ============================================================================

// Загрузка шрифта из памяти.
// data  — указатель на содержимое .ttf файла
// size  — размер буфера в байтах
// Буфер данных КОПИРУЕТСЯ внутрь (чтобы шрифт оставался валидным).
// Возвращает указатель на ttf_font_t или NULL при ошибке.
ttf_font_t* ttf_load(const uint8_t* data, size_t size);

// Загрузка шрифта из файла в VFS.
ttf_font_t* ttf_load_from_path(const char* path);

// Освобождение шрифта
void ttf_free(ttf_font_t* font);

// ============================================================================
// Рендеринг
// ============================================================================

// Отрисовка одного символа (ASCII / Unicode codepoint).
// x, y         — позиция базовой линии (baseline). Если вы хотите, чтобы
//                (x,y) был верхним левым углом, используйте ttf_draw_string_top.
// codepoint    — символ Unicode (для ASCII = значение char)
// pixel_height — высота кегля в пикселях (например, 14.0f, 24.0f)
// color        — цвет текста 0x00RRGGBB
// Возвращает горизонтальное продвижение (advance) в пикселях.
int ttf_draw_char(ttf_font_t* font, int32_t x, int32_t y,
                  uint32_t codepoint, float pixel_height, uint32_t color);

// Отрисовка строки с базовой линией в точке (x, y).
int ttf_draw_string(ttf_font_t* font, int32_t x, int32_t y,
                    const char* str, float pixel_height, uint32_t color);

// Отрисовка строки, где (x, y) — верхний левый угол блока текста.
// Внутренне сдвигает Y на ascent.
int ttf_draw_string_top(ttf_font_t* font, int32_t x, int32_t y,
                        const char* str, float pixel_height, uint32_t color);

// Ширина строки в пикселях при заданном размере.
int ttf_string_width(ttf_font_t* font, const char* str, float pixel_height);

// Высота строки (ascent + descent + line_gap) в пикселях.
int ttf_line_height(ttf_font_t* font, float pixel_height);

// Callback-рендеринг: вместо одного цвета, для каждого пикселя глифа
// вызывается cb(sx, sy, coverage) — позволяет рисовать текст с
// градиентом, бликами и любыми масками. coverage ∈ [0..255].
typedef void (*ttf_pixel_cb_t)(int sx, int sy, uint8_t coverage, void* user);

// Отрисовывает строку через callback. (x,y) — верхний левый угол текста.
// Возвращает итоговую ширину (advance).
int ttf_draw_string_cb(ttf_font_t* font, int32_t x, int32_t y,
                       const char* str, float pixel_height,
                       ttf_pixel_cb_t cb, void* user);

#endif // TTF_H
