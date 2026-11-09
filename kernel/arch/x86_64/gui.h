// ============================================================================
// gui.h — минимальный compositor + window manager для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// На 37.6b: статичный desktop с фоном, заголовком и 2-3 demo-окнами.
// Каждое окно — title bar (с акцентным цветом и текстом) + content area.
// Без drag-and-drop / focus / mouse — это в 37.6c.
//
// Compositor рисует напрямую в VESA framebuffer (без back buffer на 37.6b —
// окна статичны, нет анимации, нет flickering).  Пиксельная глубина 32-bpp
// gemerated_theme поддерживается через ui_theme.h — но мы не подключаем,
// чтобы не блокировать на python-генераторе.
// ============================================================================
#ifndef AOS_X86_64_GUI_H
#define AOS_X86_64_GUI_H

#include <stdint.h>
#include <stdbool.h>

#define GUI_MAX_WINDOWS  8

typedef struct {
    int      x, y;
    int      w, h;
    char     title[64];
    uint32_t bg;            // цвет content area
    uint32_t accent;        // цвет title bar
    void     (*draw_content)(int x, int y, int w, int h);
    bool     visible;
} gui_window_t;

void gui_init(void);
int  gui_window_add(const gui_window_t* w);   // возвращает id или -1
void gui_compose(void);                        // рисует desktop + все окна
void gui_demo_run(void) __attribute__((noreturn));  // demo-режим с mouse + drag

// Курсор-стрелка (10×16 пикселей, hard-coded).
void gui_draw_cursor(int x, int y);

#endif
