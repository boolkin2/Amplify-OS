// ============================================================================
// window.h — окно с собственным offscreen-буфером (AMPLIFY-OS x86_64).
// ----------------------------------------------------------------------------
// Окно владеет:
//   * прямоугольной геометрией (x, y, width, height)
//   * заголовком (32 байта)
//   * собственным буфером ARGB пикселей размера width×height (контент,
//     заголовок рисуется WM сверху)
//   * флагами focus, drag, close_requested, opacity (0..255)
//   * callback'ами on_draw/on_click для приложений
//
// API простой: window_create, window_destroy, window_fill, window_put_pixel.
// Композитинг — ответственность WM (см. wm.h).
// ============================================================================
#ifndef AOS_X86_64_WINDOW_H
#define AOS_X86_64_WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"

#define WINDOW_TITLE_HEIGHT 28
#define WINDOW_CLOSE_BTN    14
#define WINDOW_MAX_TITLE    48

struct window;
typedef void (*window_draw_fn) (struct window* w, const gfx_target_t* tgt);
typedef void (*window_click_fn)(struct window* w, int mx, int my);
// on_key: символ c, специальные клавиши приходят как control codes
// ('\n', '\b', '\t'). NUL ('\0') не доставляется.
typedef void (*window_key_fn)  (struct window* w, char c);
// [48.2] Колесо мыши над окном: notches > 0 — от себя (содержимое вверх).
typedef void (*window_scroll_fn)(struct window* w, int notches);

typedef enum {
    WINDOW_CHROME_CLASSIC = 0,
    WINDOW_CHROME_BORDERLESS,
    WINDOW_CHROME_WORKSPACE
} window_chrome_t;

typedef struct window {
    int       id;
    char      title[WINDOW_MAX_TITLE];

    int       x, y;
    int       width, height;

    uint32_t* buffer;          // ARGB content (без title bar)
    gfx_target_t target;       // дескриптор для рисования в buffer

    bool      visible;
    bool      focused;
    bool      dragging;
    bool      close_requested;
    int       drag_offset_x;
    int       drag_offset_y;

    uint8_t   opacity;         // 0..255
    window_chrome_t chrome;    // Stage B: classic, borderless or workspace

    // [HOTFIX 20.05.2026] Окно может попросить WM рендерить непрерывно
    // (не уходить в 10 FPS idle), даже если нет ввода мыши/клавиатуры.
    // Нужен для анимированных окон: 3D-куб, видеоплеер, игры, любые
    // приложения, которые сами двигают что-то по таймеру.
    // По умолчанию false — статичные окна не тратят CPU зря.
    bool      wants_continuous_redraw;

    // [48.2] Буфер уже содержит кадр on_draw. Во время анимаций композитор
    // «замораживает» окна: показывает готовый буфер, on_draw не зовёт.
    bool      rendered;

    // Цветовая тема (фон контента и акцент title bar).
    uint32_t  bg_color;
    uint32_t  accent_color;

    // Callback приложения. on_draw вызывается WM каждый кадр после
    // отрисовки фона; рисует контент окна в win->target.
    window_draw_fn  on_draw;
    window_click_fn on_click;
    window_key_fn   on_key;
    window_scroll_fn on_scroll;      // [48.2] может быть 0

    // Произвольные пользовательские данные (для apps).
    void* user_data;

    // Этап F: владение состоянием приложения.
    //   owns_user_data == true -> window_destroy() сам делает kfree(user_data).
    //   on_destroy вызывается перед освобождением — для приложений
    //   со вложенными аллокациями (буферы, текстуры).
    bool  owns_user_data;
    void (*on_destroy)(struct window* w);

    // Z-order linked list (управляется wm.c).
    struct window* prev;
    struct window* next;
} window_t;

// Создаёт окно. Аллоцирует buffer width*height*4 в heap. Возвращает NULL при OOM.
window_t* window_create(const char* title, int x, int y, int w, int h);

// Освобождает buffer и saved metadata.
void      window_destroy(window_t* w);

// Чистая заливка контента.
void      window_fill(window_t* w, uint32_t color);

// Прямой piксель (локальные координаты в content area).
void      window_put_pixel(window_t* w, int lx, int ly, uint32_t color);

// Перерисовать окно в указанный target (обычно doublebuf).
// Рисует тень + рамку + title bar + содержимое (через on_draw или
// готовый buffer).
void      window_render(window_t* w, const gfx_target_t* tgt);

// Проверка попадания экранных координат (mx, my) в кнопку закрытия.
bool      window_hit_close(const window_t* w, int mx, int my);

// Проверка попадания в title bar (для drag).
bool      window_hit_titlebar(const window_t* w, int mx, int my);

// Проверка попадания в content area.
bool      window_hit_content(const window_t* w, int mx, int my);
int       window_title_height(const window_t* w);
void      window_set_chrome(window_t* w, window_chrome_t chrome);
bool      window_resize(window_t* w, int width, int height);
bool      window_enter_workspace(window_t* w, int screen_w, int screen_h,
                                 int inset_left, int inset_top);

#endif
