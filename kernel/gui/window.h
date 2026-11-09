// ============================================================================
// window.h - Структуры и функции для окон
// ============================================================================

#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stdbool.h>

#define WINDOW_TITLE_HEIGHT 28
#define WINDOW_CLOSE_BTN_SIZE 14
#define WINDOW_CORNER_RADIUS 10
#define WINDOW_SHADOW_SIZE   18   // половина диаметра мягкой тени (в пикселях)
#define WINDOW_SHADOW_OFFSET 4    // сдвиг тени вниз для «подвисшего» эффекта
#define MAX_WINDOW_TITLE    64

// Высота нижней панели задач — используется окнами в «максимизированном»
// режиме, чтобы не залезать на неё.
#define TASKBAR_HEIGHT 48

typedef struct window {
    int32_t id;
    char title[MAX_WINDOW_TITLE];
    
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    
    // Пиксельный буфер окна (содержимое без заголовка), формат 0xAARRGGBB.
    // AA = 0xFF по умолчанию (непрозрачный пиксель). Приложения могут
    // записывать альфа-значения и получать «дырчатый» контент.
    uint32_t* buffer;

    // Общая прозрачность окна (Этап 22): 0 = невидимо, 255 = полностью видимо.
    // Применяется поверх per-pixel alpha буфера при композитинге.
    uint8_t opacity;
    
    // Состояние
    bool is_focused;
    bool is_dragging;
    bool close_requested;     // Запрос на закрытие
    bool is_maximized;        // Окно развёрнуто на весь экран (кроме taskbar)
    bool is_minimized;        // Окно свёрнуто (не рисуется, только на панели)
    int32_t drag_offset_x;
    int32_t drag_offset_y;

    // Если true — окно хочет получать события «мышь зажата + двигается»
    // повторно (для рисования в Paint и т.п.). По умолчанию false: тогда
    // click_callback вызывается строго ОДИН раз на нажатие (edge trigger),
    // и кнопки не «нажимаются дважды».
    bool wants_mouse_drag;

    // Сохранённая геометрия до максимизации (чтобы восстановить её по клику).
    int32_t saved_x, saved_y, saved_w, saved_h;
    
    // Для связного списка Z-order
    struct window* next; // Окно выше (ближе к пользователю)
    struct window* prev; // Окно ниже (дальше от пользователя)
} window_t;

// ============================================================================
// Функции
// ============================================================================

// Создать новое окно
window_t* window_create(const char* title, int32_t x, int32_t y, int32_t w, int32_t h);

// Удалить окно и освободить память
void window_destroy(window_t* win);

// Нарисовать пиксель внутри окна (локальные координаты)
void window_put_pixel(window_t* win, int32_t x, int32_t y, uint32_t color);

// Залить окно цветом
void window_fill(window_t* win, uint32_t color);

// Отрисовать окно (заголовок + контент) в двойной буфер экрана
// Эта функция вызывается оконным менеджером
void window_draw_to_doublebuf(window_t* win);

// Проверка попадания клика в кнопку закрытия (экранные координаты)
bool window_is_close_button_clicked(window_t* win, int32_t mx, int32_t my);

// Проверки кликов в жёлтую (minimize) и зелёную (maximize) кнопки заголовка.
bool window_is_minimize_button_clicked(window_t* win, int32_t mx, int32_t my);
bool window_is_maximize_button_clicked(window_t* win, int32_t mx, int32_t my);

// Переключить между обычным и максимизированным размером (сохраняет/
// восстанавливает geometry). Буфер окна НЕ переаллоцируется: мы
// масштабируем отображение; приложение продолжает рисовать в свой
// исходный буфер (simplest approach для kernel-only WM).
void window_toggle_maximize(window_t* win);

// Свернуть/развернуть окно. Свёрнутое окно не рисуется в композите, но
// остаётся в списке WM и на панели задач; клик по кнопке на панели —
// восстанавливает его.
void window_set_minimized(window_t* win, bool minimized);

#endif // WINDOW_H
