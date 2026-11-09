// ============================================================================
// ui_font.h — сглаженная типографика AMPLIFY-OS "Pearl" (этап F).
// ----------------------------------------------------------------------------
// Пиксельный шрифт 8x16 полностью выведен из эксплуатации. Вместо него —
// офлайн-сгенерированные (tools/gen_ui_font.py) антиалиасные атласы глифов:
//   * пропорциональный sans (DejaVu Sans из initrd_content/default.ttf)
//     в размерах 12 / 14 / 16 / 20 / 28 / 44;
//   * монофейс (Liberation Mono) 14 / 16 для консоли, терминала и Pulse.
// Каждый глиф хранится как 4-битная карта покрытия (16 уровней сглаживания)
// с бирингами и advance в 1/4 px; кернинг — разреженная сортированная
// таблица пар. Рантайм не растеризует шрифт и не выделяет память: индексация
// глифа O(1), кэш строится на этапе сборки (атлас = кэш).
// Кодировка входных строк: CP1251 (-fexec-charset=CP1251) либо UTF-8 —
// декодер распознаёт оба варианта.
// ============================================================================
#ifndef AOS_X86_64_UI_FONT_H
#define AOS_X86_64_UI_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"

// Совместимость с кодом, который считает консольную сетку в «ячейках».
#define AOS64_FONT_W 8
#define AOS64_FONT_H 16

typedef enum {
    UI_FONT_12 = 0,     // микро-подписи, uppercase-лейблы
    UI_FONT_14,         // основной текст интерфейса
    UI_FONT_16,         // акцентный текст, кнопки
    UI_FONT_20,         // заголовки карточек
    UI_FONT_28,         // заголовки экранов
    UI_FONT_44,         // дисплейные значения (часы, Ignite)
    UI_FONT_MONO_14,    // консоль/терминал, ячейка 8 px
    UI_FONT_MONO_16,    // Pulse / логи, ячейка 10 px
    UI_FONT_COUNT
} ui_font_id_t;

typedef struct {
    uint8_t  w, h;          // размер bbox глифа в пикселях
    int8_t   left;          // горизонтальный биринг
    int8_t   top;           // высота bbox над базовой линией
    uint16_t advance_q;     // advance в 1/4 px
    uint32_t offset;        // смещение в blob'е покрытия
} ui_glyph_t;

typedef struct {
    uint8_t first;
    uint8_t second;
    int8_t  delta_q;        // поправка кернинга в 1/4 px
} ui_kern_t;

typedef struct {
    const ui_glyph_t*    glyphs;     // 224 записи, индекс = cp1251 - 0x20
    const unsigned char* blob;       // упакованное 4-битное покрытие
    const ui_kern_t*     kern;
    int                  kern_count;
    uint8_t              px;         // номинальный кегль
    uint8_t              ascent;     // px над базовой линией
    uint8_t              line_h;     // ascent + descent
    uint8_t              mono_adv;   // 0 = пропорциональный, иначе ширина ячейки
} ui_face_t;

extern const ui_face_t ui_font_faces[UI_FONT_COUNT];

// Метрики -------------------------------------------------------------------
int  ui_font_line_height(ui_font_id_t id);
int  ui_font_ascent(ui_font_id_t id);
int  ui_font_cap_height(ui_font_id_t id);
int  ui_font_text_width(const char* s, ui_font_id_t id);
int  ui_font_text_width_tracked(const char* s, ui_font_id_t id, int tracking);

// Отрисовка -----------------------------------------------------------------
// *_baseline — y = базовая линия. ui_font_draw — y = верх строки (16*scale-бокс),
// что делает функцию drop-in заменой прежним вызовам 8x16.
int  ui_font_draw_baseline(const gfx_target_t* t, int x, int baseline,
                           const char* s, uint32_t color, ui_font_id_t id);
int  ui_font_draw_baseline_ex(const gfx_target_t* t, int x, int baseline,
                              const char* s, uint32_t color, ui_font_id_t id,
                              int tracking, bool bold);
int  ui_font_draw(const gfx_target_t* t, int x, int y_top, const char* s,
                  uint32_t color, ui_font_id_t id);
int  ui_font_draw_tracked(const gfx_target_t* t, int x, int y_top, const char* s,
                          uint32_t color, ui_font_id_t id, int tracking);
int  ui_font_draw_bold(const gfx_target_t* t, int x, int y_top, const char* s,
                       uint32_t color, ui_font_id_t id);

// Консольная ячейка: заливка фона + сглаженный моноглиф по центру.
void ui_font_draw_cell(const gfx_target_t* t, int x, int y, char c,
                       uint32_t fg, uint32_t bg, ui_font_id_t id);

// Соответствие legacy-масштабов (scale 1/2/3) новой типошкале.
ui_font_id_t ui_font_for_scale(int scale);
// Базовая линия для legacy-вызовов, где y — верх 16*scale-бокса.
int  ui_font_baseline_for_box(ui_font_id_t id, int y_top, int box_h);

#endif
