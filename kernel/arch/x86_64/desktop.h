// ============================================================================
// desktop.h — рабочий стол AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Включает:
//   * Фоновый градиент + большой треугольный логотип AMPLIFY.
//   * Сетку desktop_icon_t (имя, glyph, on_click). По двойному клику →
//     приложение. На 37.6c-2 поддерживаем одинарный клик (упрощение).
//   * Этап F: taskbar и меню «Пуск» удалены. Запуск — rail + плитки Core,
//     а реестр иконок ниже — единый источник правды для Core.
// ============================================================================
#ifndef AOS_X86_64_DESKTOP_H
#define AOS_X86_64_DESKTOP_H

#include "graphics.h"
#include <stdint.h>
#include <stdbool.h>

#define DESKTOP_MAX_ICONS  32
#define DESKTOP_ICON_W     80
#define DESKTOP_ICON_H     80
#define DESKTOP_ICON_PAD   20

typedef void (*desktop_click_fn)(void);

typedef struct {
    char     name[24];
    char     glyph;          // одиночный символ-иконка
    uint32_t accent;         // legacy: цвет «плитки» (больше не рисуется)
    int      icon_id;        // ui_icon_id_t — растровая иконка темы «Pearl»
    desktop_click_fn on_click;
    int      x, y, w, h;     // вычисляется при desktop_init
} desktop_icon_t;

void desktop_init(void);
int  desktop_add_icon(const char* name, char glyph, uint32_t accent,
                      desktop_click_fn on_click);
void desktop_draw(const gfx_target_t* tgt);
uint64_t desktop_state_key(void);              // [41.2] ключ кэша слоя рабочего стола
bool desktop_handle_click(int mx, int my);     // true если клик по иконке/кнопке
void desktop_set_pointer(int mx, int my);      // hover для rail/status controls
bool desktop_handle_key(char c);               // 1..9 + Enter navigation

// Реестр приложений (этап F): Core строит свои плитки из этого списка.
int  desktop_icon_count(void);
const desktop_icon_t* desktop_icon_at(int index);
bool desktop_launch_named(const char* name);

#endif
