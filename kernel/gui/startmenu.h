// ============================================================================
// startmenu.h - Меню Пуск для MyOS
// ============================================================================
// Всплывающая панель слева внизу: список приложений + панель управления
// + кнопки перезагрузки/выключения. Открывается кликом по иконке-логотипу
// в нижней панели задач (она дублирует логотип из boot-splash).
// ============================================================================

#ifndef STARTMENU_H
#define STARTMENU_H

#include <stdint.h>
#include <stdbool.h>

// Геометрия меню (константы — пусть WM знает, куда рисовать).
#define STARTMENU_WIDTH       300
#define STARTMENU_HEIGHT      430
#define STARTMENU_ROW_HEIGHT  38

// Иконка логотипа в панели задач (нарисована треугольником, как в splash).
#define STARTBTN_SIZE         36

// Инициализация — вызывается один раз при запуске.
void startmenu_init(void);

// Рисует логотип-кнопку «Пуск» на панели задач.
// (btn_x, btn_y) — координаты левого-верхнего угла в экране, side = размер.
void startmenu_draw_button(int btn_x, int btn_y, int side, bool hovered);

// Рисует открытое меню Пуск в compose-буфер (если оно открыто).
void startmenu_draw(void);

// Открыть / закрыть меню. При открытии фиксирует позицию относительно
// панели задач (меню всплывает над ней, над кнопкой «Пуск»).
void startmenu_toggle(int taskbar_x, int taskbar_y_top);
void startmenu_close(void);
bool startmenu_is_open(void);

// Обработка клика мышью (экранные координаты). Возвращает true, если клик
// попал в зону меню (тогда WM не должен передавать его окнам).
bool startmenu_handle_click(int32_t mx, int32_t my);

// Проверка попадания точки в область меню Пуск (для перехвата кликов).
bool startmenu_point_inside(int32_t mx, int32_t my);

#endif // STARTMENU_H
