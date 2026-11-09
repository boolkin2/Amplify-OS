// ============================================================================
// mind_widget.h — оверлей AMPLIFY MIND по Ctrl+Space (этап 49, Pearl UI).
// ----------------------------------------------------------------------------
// Панель поверх рабочего стола и окон: журнал реплик (вы / MIND / данные
// инструментов), строка ввода, статус модели. Открытие — Ctrl+Space
// (KBD_KEY_MIND), закрытие — Esc или повторный Ctrl+Space. Генерация идёт
// на AP, поэтому панель остаётся отзывчивой, пока модель «думает».
// ============================================================================
#ifndef AOS_X86_64_MIND_WIDGET_H
#define AOS_X86_64_MIND_WIDGET_H

#include <stdbool.h>
#include "graphics.h"

void mind_widget_init(void);
void mind_widget_toggle(void);
void mind_widget_show(void);
void mind_widget_hide(void);
bool mind_widget_is_open(void);
bool mind_widget_visible(void);         // [48.2] открыта или ещё исчезает
bool mind_widget_active(void);          // нужна частая перерисовка (анимация / генерация)
void mind_widget_tick(void);            // каждый проход главного цикла WM
void mind_widget_draw(const gfx_target_t* t);
bool mind_widget_handle_key(char c);
bool mind_widget_handle_click(int sx, int sy);

#endif
