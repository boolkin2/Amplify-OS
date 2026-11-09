// ============================================================================
// ui_kit.h — элементы управления Pearl для workspace-окон (подэтап 48.2).
// ----------------------------------------------------------------------------
// ui_components.h рисует в логической сетке 1600x1000 ЭКРАНА (ui_ux/ui_uy) —
// для содержимого окна это не годится: окно рисует в собственный буфер, в
// своих пикселях. Здесь те же по виду кнопки, поля ввода и карточки, но в
// координатах окна, с типографикой ui_font. Один прямоугольник (pk_rect_t)
// используется и для отрисовки, и для проверки попадания клика — раскладка
// считается в одном месте, рисование и клики не могут разойтись.
// ============================================================================
#ifndef AOS_X86_64_UI_KIT_H
#define AOS_X86_64_UI_KIT_H

#include "graphics.h"
#include "ui_font.h"
#include "ui_theme.h"
#include "window.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct { int x, y, w, h; } pk_rect_t;

static inline bool pk_hit(pk_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

typedef enum { PK_IDLE = 0, PK_HOVER, PK_ACTIVE, PK_DISABLED } pk_state_t;

// Текст: y — верх строки. Возвращает ширину в пикселях.
int  pk_text(const gfx_target_t* t, int x, int y, const char* s, uint32_t color, ui_font_id_t f);
int  pk_text_bold(const gfx_target_t* t, int x, int y, const char* s, uint32_t color, ui_font_id_t f);
// Текст, обрезанный по ширине max_w (с многоточием).
void pk_text_clip(const gfx_target_t* t, int x, int y, int max_w, const char* s,
                  uint32_t color, ui_font_id_t f, bool bold);
// Подпись-рубрика: мелкий шрифт с разрядкой, цвет muted.
void pk_label(const gfx_target_t* t, int x, int y, const char* s);

void pk_card(const gfx_target_t* t, pk_rect_t r);
void pk_button(const gfx_target_t* t, pk_rect_t r, const char* label, bool primary, pk_state_t st);
// Поле ввода: текст, рамка по состоянию (фокус — акцент, ошибка — «ягодный»),
// мигающая черта курсора в конце текста при фокусе.
void pk_field(const gfx_target_t* t, pk_rect_t r, const char* text, const char* placeholder,
              bool focused, bool error, bool disabled, ui_font_id_t f);
// Сегмент переключателя (один из нескольких в ряд).
void pk_segment(const gfx_target_t* t, pk_rect_t r, const char* label, bool selected, pk_state_t st);
// Цветная «таблетка» состояния с точкой.
void pk_chip(const gfx_target_t* t, int x, int y, const char* label, uint32_t dot);

// Состояние элемента по положению указателя (локальные координаты окна).
pk_state_t pk_pointer_state(const window_t* w, pk_rect_t r, bool disabled);
// Положение указателя в координатах содержимого окна.
void pk_pointer(const window_t* w, int* lx, int* ly, bool* left_down);

#endif
