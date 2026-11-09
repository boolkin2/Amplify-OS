// ============================================================================
// wm.h — оконный менеджер для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Обязанности WM:
//   * Хранит Z-order (двусвязный список окон, head = самое нижнее, tail =
//     самое верхнее / focused).
//   * Обрабатывает события мыши: drag по title bar, click по close btn,
//     focus при клике, передача in-window клика приложению через
//     window_t.on_click.
//   * Каждый кадр: doublebuf_clear → desktop_draw → window_render всех окон
//     в Z-order → cursor_draw → swap.
//
// Главный цикл: wm_main_loop() — не возвращается.
// ============================================================================
#ifndef AOS_X86_64_WM_H
#define AOS_X86_64_WM_H

#include "window.h"

void      wm_init(void);
void      wm_add_window(window_t* w);          // окно становится top + focused
void      wm_remove_window(window_t* w);       // не освобождает память окна
void      wm_focus(window_t* w);               // ставит окно top + focused

window_t* wm_focused(void);
int       wm_count(void);
window_t* wm_iterate(window_t* prev);          // bottom-to-top, NULL для start

// [41.2] Попросить перерисовать сцену в ближайшем кадре — для кода, который
// меняет картинку без события клавиатуры или мыши.
void      wm_invalidate(void);
// [48.2] Насос кадров для долгих операций приложения (загрузка страницы,
// проверка DNS): рисует кадр сцены и двигает курсор, НЕ раздавая ввод окнам.
// Esc превращается в net_cancel(), остальные клавиши и щелчки отбрасываются.
// Зовётся из net_idle() (netif.h) не чаще раза в ~33 мс.
void      wm_pump(void);

// Главный цикл: бесконечный. mouse + redraw.
void      wm_main_loop(void) __attribute__((noreturn));

#endif
