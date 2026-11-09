// ============================================================================
// wm.c - Реализация оконного менеджера (Window Manager) для MyOS
// ============================================================================

#include "wm.h"
#include "graphics.h"
#include "font.h"
#include "doublebuf.h"
#include "cursor.h"
#include "framebuffer.h"
#include "desktop.h"
#include "startmenu.h"
#include "ui_theme.h"   // AUTO-GENERATED: цвета темы от AI-генератора
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/rtc.h"
#include <stddef.h>

// Глобальные настройки обоев (задаются Control Panel в apps.c).
extern uint32_t g_desktop_top_color;
extern uint32_t g_desktop_bottom_color;

// Notes-хук при закрытии окна (apps.c).
extern void notes_notify_close(window_t* win);
extern void explorer_notify_close(window_t* win);

// ============================================================================
// Состояние оконного менеджера
// ============================================================================

// Двусвязный список окон (порядок отрисовки: от bottom к top)
static window_t* wm_bottom_window = NULL;
static window_t* wm_top_window = NULL;

// Флаги «грязи»:
//   scene_dirty  — полная сцена (фон + окна) нуждается в пересборке. Дорого.
//   frame_dirty  — нужно презентовать новый кадр (сцена + курсор). Дёшево,
//                  если сцена не dirty — обойдёмся memcpy из scene_buf.
static bool wm_scene_dirty = true;
static bool wm_frame_dirty = true;

// Состояние мыши на предыдущем кадре
static bool last_left_btn = false;
static bool last_right_btn = false;

// Окно, которое сейчас перетаскивается
static window_t* dragging_window = NULL;

// Геометрия панели задач (taskbar) — единый источник правды.
static int taskbar_height(void) { return TASKBAR_HEIGHT; }

// Прямоугольник кнопки «Пуск».
static void startbtn_rect(int32_t* out_x, int32_t* out_y,
                          int32_t* out_w, int32_t* out_h) {
    int32_t w = (int32_t)framebuffer_get_width();
    int32_t h = (int32_t)framebuffer_get_height();
    (void)w;
    int tb = taskbar_height();
    *out_w = STARTBTN_SIZE;
    *out_h = STARTBTN_SIZE;
    *out_x = 6;
    *out_y = h - tb + (tb - STARTBTN_SIZE) / 2;
}

// Прямоугольник i-й кнопки окна на панели задач. Возвращает false, если
// кнопка не помещается (overflow).
static bool taskbar_btn_rect(int idx, int32_t* out_x, int32_t* out_y,
                             int32_t* out_w, int32_t* out_h) {
    int32_t w = (int32_t)framebuffer_get_width();
    int32_t h = (int32_t)framebuffer_get_height();
    int tb = taskbar_height();

    int btn_w = 160;
    int btn_h = tb - 10;
    int start_x = 6 + STARTBTN_SIZE + 10;
    int btn_x = start_x + idx * (btn_w + 4);
    // Резервируем место справа под часы/название системы.
    int right_reserve = 200;
    if (btn_x + btn_w > w - right_reserve) return false;

    *out_x = btn_x;
    *out_y = h - tb + (tb - btn_h) / 2;
    *out_w = btn_w;
    *out_h = btn_h;
    return true;
}

// Счётчик окон
static int window_count = 0;

// Тики последнего обновления приложений
static uint32_t last_app_update_tick = 0;

// Для обнаружения drag-рисования в Paint (удержание кнопки)
static bool is_painting_drag = false;

// ============================================================================
// Внутренние функции
// ============================================================================

// Вывести окно на передний план (сделать его верхним в списке)
static void wm_bring_to_front(window_t* win) {
    if (!win || win == wm_top_window) return;
    
    // Извлекаем из текущего места
    if (win->prev) win->prev->next = win->next;
    else wm_bottom_window = win->next;
    
    if (win->next) win->next->prev = win->prev;
    else wm_top_window = win->prev;
    
    // Вставляем в конец списка (top)
    win->prev = wm_top_window;
    win->next = NULL;
    
    if (wm_top_window) wm_top_window->next = win;
    wm_top_window = win;
    
    if (!wm_bottom_window) wm_bottom_window = win;
    
    // Снимаем фокус со всех окон
    for (window_t* w = wm_bottom_window; w; w = w->next) {
        w->is_focused = false;
    }
    // Даем фокус нашему окну
    win->is_focused = true;
    wm_scene_dirty = true;
    wm_frame_dirty = true;
}

// Проверка попадания точки в прямоугольник
static bool point_in_rect(int32_t px, int32_t py, int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

// (wm_find_window_at заменён на wm_find_visible_window_at, см. ниже).

// Закрыть и удалить окно
static void wm_close_window(window_t* win) {
    if (!win) return;

    // Хук для Notes (автосохранение + освобождение instance)
    notes_notify_close(win);

    // Хук для File Explorer (освобождение instance)
    explorer_notify_close(win);

    // Уведомляем рабочий стол
    desktop_notify_window_closed(win);

    // Удаляем из WM
    wm_remove_window(win);

    // Освобождаем память
    window_destroy(win);
}

// Отрисовка фона рабочего стола (обои) и панели задач.
static void wm_draw_desktop(void) {
    int32_t w = framebuffer_get_width();
    int32_t h = framebuffer_get_height();

    // Вертикальный градиент обоев (настраивается Control Panel).
    gfx_fill_rect_gradient(0, 0, w, h, g_desktop_top_color, g_desktop_bottom_color);

    // Тонкий паттерн «звёздочек» для текстуры (редкая сетка точек).
    uint32_t star = color_lighten(g_desktop_top_color, 40);
    for (int32_t py = 20; py < h - TASKBAR_HEIGHT; py += 34) {
        for (int32_t px = 20; px < w; px += 34) {
            gfx_put_pixel(px, py, star);
        }
    }

    // Иконки рабочего стола
    desktop_draw_icons();

    // ======== Панель задач ========
    int tb = taskbar_height();
    int tb_y = h - tb;

    // Основной градиент панели (тонированное стекло).
    // Цвета панели задач — из AI-темы (ui_theme.h).
    gfx_fill_rect_gradient(0, tb_y, w, tb, UI_THEME_TASKBAR_TOP, UI_THEME_TASKBAR_BOTTOM);
    gfx_hline(0, tb_y, w, UI_THEME_TASKBAR_EDGE);
    gfx_hline(0, tb_y + 1, w, 0xFF2B2F42);

    // Кнопка «Пуск» — стилизованный логотип (треугольник как в splash).
    int32_t bx, by, bw, bh;
    startbtn_rect(&bx, &by, &bw, &bh);
    bool hovered = startmenu_is_open();
    startmenu_draw_button(bx, by, bw, hovered);

    // Кнопки открытых окон на панели задач
    int idx = 0;
    for (window_t* win = wm_bottom_window; win; win = win->next) {
        int32_t ex, ey, ew, eh;
        if (!taskbar_btn_rect(idx, &ex, &ey, &ew, &eh)) break;

        // Подложка (градиент в зависимости от фокуса + свёрнутости).
        uint32_t bg_top, bg_bot, border;
        if (win->is_minimized) {
            bg_top = 0xFF252838; bg_bot = 0xFF181A28; border = 0xFF353848;
        } else if (win->is_focused) {
            bg_top = 0xFF556080; bg_bot = 0xFF333C56; border = 0xFF8899BB;
        } else {
            bg_top = 0xFF333844; bg_bot = 0xFF22252E; border = 0xFF454858;
        }
        gfx_fill_rect_gradient(ex, ey, ew, eh, bg_top, bg_bot);
        gfx_draw_rect(ex, ey, ew, eh, border);

        // Индикатор активного окна (тонкая светящаяся полоска сверху).
        if (win->is_focused) {
            gfx_hline(ex, ey, ew, 0xFF8EB1FF);
        }

        // Текст заголовка (обрезаем)
        const char* t = win->title;
        char buf[32];
        int copy_len = 0;
        while (t[copy_len] && copy_len < 30) { buf[copy_len] = t[copy_len]; copy_len++; }
        buf[copy_len] = '\0';
        font_draw_string(ex + 10, ey + (eh - FONT_HEIGHT) / 2,
                         buf, 0xFFE8EAF2, 0xFF000000);

        idx++;
    }

    // Часы — реальное время из RTC (CMOS), а не uptime.
    // Формат HH:MM:SS, 24-часовой, всегда из календарного времени машины.
    rtc_time_t now;
    rtc_read(&now);
    char time_str[16];
    time_str[0] = (char)('0' + (now.hour   / 10) % 10);
    time_str[1] = (char)('0' +  now.hour   % 10);
    time_str[2] = ':';
    time_str[3] = (char)('0' + (now.minute / 10) % 10);
    time_str[4] = (char)('0' +  now.minute % 10);
    time_str[5] = ':';
    time_str[6] = (char)('0' + (now.second / 10) % 10);
    time_str[7] = (char)('0' +  now.second % 10);
    time_str[8] = '\0';

    // Фон часов — маленький чип-плашка
    int clock_x = w - 100;
    int clock_y = tb_y + (tb - 22) / 2;
    gfx_fill_rect_gradient(clock_x, clock_y, 92, 22, 0xFF2A3050, 0xFF1C2040);
    gfx_draw_rect(clock_x, clock_y, 92, 22, 0xFF4A5070);
    font_draw_string(clock_x + 10, clock_y + (22 - FONT_HEIGHT) / 2,
                     time_str, 0xFFBBCCFF, 0xFF000000);

    // Версия слева от часов
    font_draw_string(clock_x - 80, clock_y + (22 - FONT_HEIGHT) / 2,
                     "Amplify OS", 0xFF8899BB, 0xFF000000);
}

// ============================================================================
// Внешний API
// ============================================================================

void wm_init(void) {
    wm_bottom_window = NULL;
    wm_top_window = NULL;
    wm_scene_dirty = true;
    wm_frame_dirty = true;
    last_left_btn = false;
    dragging_window = NULL;
    window_count = 0;
    last_app_update_tick = 0;
    is_painting_drag = false;
}

void wm_add_window(window_t* win) {
    if (!win) return;
    
    if (!wm_bottom_window) {
        wm_bottom_window = win;
        wm_top_window = win;
        win->prev = NULL;
        win->next = NULL;
        win->is_focused = true;
    } else {
        // Вставляем как новый top (не через bring_to_front, т.к. ещё не в списке)
        win->prev = wm_top_window;
        win->next = NULL;
        if (wm_top_window) wm_top_window->next = win;
        wm_top_window = win;
        
        // Снимаем фокус со всех, даём новому
        for (window_t* w = wm_bottom_window; w; w = w->next) {
            w->is_focused = false;
        }
        win->is_focused = true;
    }
    window_count++;
    wm_scene_dirty = true;
    wm_frame_dirty = true;
}

void wm_remove_window(window_t* win) {
    if (!win) return;
    
    if (win->prev) win->prev->next = win->next;
    else wm_bottom_window = win->next;
    
    if (win->next) win->next->prev = win->prev;
    else wm_top_window = win->prev;
    
    if (dragging_window == win) {
        dragging_window = NULL;
    }
    
    // Если удалили сфокусированное окно, даем фокус следующему верхнему
    if (win->is_focused && wm_top_window) {
        wm_top_window->is_focused = true;
    }
    
    window_count--;
    wm_scene_dirty = true;
    wm_frame_dirty = true;
}

void wm_require_redraw(void) {
    wm_scene_dirty = true;
    wm_frame_dirty = true;
}

void wm_require_present(void) {
    wm_frame_dirty = true;
}

window_t* wm_get_focused_window(void) {
    return wm_top_window;
}

int wm_get_window_count(void) {
    return window_count;
}

// Перебор всех окон, начиная с переднего плана, через next-связи.
window_t* wm_iterate_windows(window_t* prev) {
    if (!prev) return wm_top_window;
    return prev->next;
}

// Helpers для wm_update (объявлены локально для единообразия).
static bool wm_point_in_startbtn(int32_t mx, int32_t my) {
    int32_t bx, by, bw, bh;
    startbtn_rect(&bx, &by, &bw, &bh);
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

static bool wm_point_in_taskbar(int32_t mx __attribute__((unused)), int32_t my) {
    int32_t h = (int32_t)framebuffer_get_height();
    return my >= h - taskbar_height();
}

// Найти, в какую кнопку окна на панели задач попали (или -1).
static window_t* wm_taskbar_button_at(int32_t mx, int32_t my) {
    int idx = 0;
    for (window_t* win = wm_bottom_window; win; win = win->next) {
        int32_t ex, ey, ew, eh;
        if (!taskbar_btn_rect(idx, &ex, &ey, &ew, &eh)) break;
        if (mx >= ex && mx < ex + ew && my >= ey && my < ey + eh) return win;
        idx++;
    }
    return NULL;
}

// Поиск окна под курсором с учётом минимизации (свёрнутые — не видны).
static window_t* wm_find_visible_window_at(int32_t x, int32_t y) {
    for (window_t* w = wm_top_window; w; w = w->prev) {
        if (w->is_minimized) continue;
        if (point_in_rect(x, y, w->x - 1, w->y - 1,
                          w->width + 2, w->height + WINDOW_TITLE_HEIGHT + 2)) {
            return w;
        }
    }
    return NULL;
}

void wm_update(void) {
    // 1. Обработка событий мыши
    mouse_state_t mouse = mouse_get_state();
    
    // Обновляем приложения периодически (каждые ~10 тиков = 100ms)
    uint32_t current_tick = (uint32_t)timer_get_ticks();
    if (current_tick - last_app_update_tick >= 10) {
        last_app_update_tick = current_tick;
        desktop_update_apps();
        
        // Принудительная перерисовка каждую секунду (для часов на панели задач)
        if (current_tick % 100 < 10) {
            wm_scene_dirty = true;
            wm_frame_dirty = true;
        }
    }
    
    // 1.5 Обработка клавиатуры — передаём клавиши фокусированному окну
    while (keyboard_has_key()) {
        char key = keyboard_getchar_nonblock();
        if (key != 0 && wm_top_window && wm_top_window->is_focused
            && !wm_top_window->is_minimized) {
            desktop_handle_key(wm_top_window, key);
        }
    }

    // === RIGHT CLICK (edge-trigger) — контекстное меню рабочего стола ===
    if (mouse.right_btn && !last_right_btn) {
        // Закрываем start menu, если открыто
        startmenu_close();
        // Если клик в окне — игнорируем (контекст меню только для десктопа).
        window_t* rw = wm_find_visible_window_at(mouse.x, mouse.y);
        if (!rw && !wm_point_in_taskbar(mouse.x, mouse.y)) {
            desktop_context_menu_open(mouse.x, mouse.y);
        } else {
            desktop_context_menu_close();
        }
    }

    // === LEFT CLICK (edge-trigger) ===
    if (mouse.left_btn && !last_left_btn) {
        is_painting_drag = false;

        // 1) Если открыто контекстное меню — приоритет у него.
        if (desktop_context_menu_is_open()) {
            desktop_context_menu_handle_click(mouse.x, mouse.y);
            goto after_click;
        }

        // 2) Если открыто меню Пуск — приоритет у него.
        if (startmenu_is_open()) {
            if (!startmenu_handle_click(mouse.x, mouse.y)) {
                // Клик мимо меню — оно закроется, но событие не потеряем
                // и продолжим обрабатывать клик дальше.
            } else {
                goto after_click;
            }
        }

        // 3) Клик по панели задач
        if (wm_point_in_taskbar(mouse.x, mouse.y)) {
            if (wm_point_in_startbtn(mouse.x, mouse.y)) {
                int32_t bx, by, bw, bh;
                startbtn_rect(&bx, &by, &bw, &bh);
                int32_t h = (int32_t)framebuffer_get_height();
                startmenu_toggle(bx, h - taskbar_height());
                goto after_click;
            }
            // Клик по кнопке окна на панели?
            window_t* tw = wm_taskbar_button_at(mouse.x, mouse.y);
            if (tw) {
                if (tw->is_minimized) {
                    window_set_minimized(tw, false);
                    wm_bring_to_front(tw);
                } else if (tw->is_focused) {
                    // Активное окно — сворачиваем.
                    window_set_minimized(tw, true);
                } else {
                    // Неактивное окно — выводим на передний план.
                    wm_bring_to_front(tw);
                }
                wm_scene_dirty = true;
                wm_frame_dirty = true;
            }
            goto after_click;
        }

        // 4) Обычный клик — по окну или рабочему столу
        window_t* clicked = wm_find_visible_window_at(mouse.x, mouse.y);
        if (clicked) {
            // Traffic-light кнопки
            if (window_is_close_button_clicked(clicked, mouse.x, mouse.y)) {
                wm_close_window(clicked);
            } else if (window_is_minimize_button_clicked(clicked, mouse.x, mouse.y)) {
                window_set_minimized(clicked, true);
                wm_scene_dirty = true;
                wm_frame_dirty = true;
            } else if (window_is_maximize_button_clicked(clicked, mouse.x, mouse.y)) {
                window_toggle_maximize(clicked);
                // Буфер окна был реаллоцирован и очищен — просим приложение
                // перерисоваться в новом размере.
                desktop_notify_window_resized(clicked);
                wm_scene_dirty = true;
                wm_frame_dirty = true;
            } else {
                wm_bring_to_front(clicked);

                // Клик в заголовок — начинаем drag (но только если не
                // maximized, чтобы не унести окно в сторону).
                if (!clicked->is_maximized &&
                    point_in_rect(mouse.x, mouse.y, clicked->x, clicked->y,
                                  clicked->width, WINDOW_TITLE_HEIGHT)) {
                    dragging_window = clicked;
                    clicked->is_dragging = true;
                    clicked->drag_offset_x = mouse.x - clicked->x;
                    clicked->drag_offset_y = mouse.y - clicked->y;
                } else {
                    // Клик в содержимое окна — передаём приложению
                    int32_t local_x = mouse.x - clicked->x;
                    int32_t local_y = mouse.y - clicked->y - WINDOW_TITLE_HEIGHT;
                    if (local_y >= 0) {
                        desktop_handle_window_click(clicked, local_x, local_y);
                        // Только окна, явно подписавшиеся на drag-режим, получат
                        // повторные события «мышь движется при зажатой кнопке».
                        // Для всего остального (кнопки калькулятора, тулбары,
                        // списки) — строго один клик = один вызов callback.
                        if (clicked->wants_mouse_drag) {
                            is_painting_drag = true;
                        }
                        wm_scene_dirty = true;
                        wm_frame_dirty = true;
                    }
                }
            }
        } else {
            // Клик по рабочему столу (иконки или пустое место).
            if (!desktop_handle_click(mouse.x, mouse.y)) {
                for (window_t* w = wm_bottom_window; w; w = w->next) {
                    if (w->is_focused) {
                        w->is_focused = false;
                        wm_scene_dirty = true;
                        wm_frame_dirty = true;
                    }
                }
            }
        }
    }
after_click:
    
    // Перетаскивание мыши (удерживание)
    if (mouse.left_btn && dragging_window) {
        int32_t new_x = mouse.x - dragging_window->drag_offset_x;
        int32_t new_y = mouse.y - dragging_window->drag_offset_y;
        
        if (new_x != dragging_window->x || new_y != dragging_window->y) {
            dragging_window->x = new_x;
            dragging_window->y = new_y;
            wm_scene_dirty = true;
            wm_frame_dirty = true;
        }
    }
    
    // Drag-рисование (удержание кнопки в Paint)
    if (mouse.left_btn && is_painting_drag && !dragging_window) {
        window_t* over = wm_find_visible_window_at(mouse.x, mouse.y);
        if (over && over->is_focused) {
            int32_t local_x = mouse.x - over->x;
            int32_t local_y = mouse.y - over->y - WINDOW_TITLE_HEIGHT;
            if (local_y >= 0) {
                desktop_handle_window_click(over, local_x, local_y);
                wm_scene_dirty = true;
                wm_frame_dirty = true;
            }
        }
    }
    
    // Отпускание мыши
    if (!mouse.left_btn && last_left_btn) {
        if (dragging_window) {
            dragging_window->is_dragging = false;
            dragging_window = NULL;
        }
        is_painting_drag = false;
    }
    
    last_left_btn  = mouse.left_btn;
    last_right_btn = mouse.right_btn;

    // 2. Композитинг (Этап 22+: scene-cache).
    //    Две стадии:
    //      A. Если сцена dirty — перерисовываем фон + все окна в compose-буфер
    //         и сохраняем получившийся кадр в scene_buf.
    //      B. Если кадр dirty (курсор переехал или сцена перестроилась) —
    //         восстанавливаем сцену (одним memcpy) и рисуем курсор поверх.
    //    Плюс frame-rate cap ~60Hz, чтобы мышиные IRQ'ы не топили CPU.
    static uint32_t last_frame_tick = 0;
    uint32_t now_tick = (uint32_t)timer_get_ticks();

    if (wm_scene_dirty) {
        wm_scene_dirty = false;

        // 2A.1 Фон рабочего стола.
        wm_draw_desktop();

        // 2A.2 Окна (от задних к передним) с тенью + скруглёнными углами.
        //      Пропускаем свёрнутые — их «представляет» только кнопка на
        //      панели задач.
        for (window_t* w = wm_bottom_window; w; w = w->next) {
            if (w->is_minimized) continue;
            window_draw_to_doublebuf(w);
        }

        // 2A.3 Overlay: меню Пуск + контекстное меню desktop'а рисуются ПОВЕРХ
        //      окон, потому что являются модальными UI.
        startmenu_draw();
        desktop_context_menu_draw();

        // 2A.4 Сохраняем «готовую сцену» (без курсора) для быстрых cursor-only
        //      обновлений.
        doublebuf_save_scene();
        wm_frame_dirty = true;
    }

    if (wm_frame_dirty) {
        // Cap ~100 FPS (тайм-база 100 Hz → минимум 1 тик ≈ 10 мс между
        // кадрами). Не блокируем — если ещё рано, просто выходим и вернёмся
        // на следующей итерации цикла ядра.
        if (now_tick == last_frame_tick) return;
        last_frame_tick = now_tick;

        wm_frame_dirty = false;

        if (doublebuf_has_scene()) {
            // БЫСТРЫЙ ПУТЬ для cursor-only обновлений: блит scene → fb
            // напрямую (один memcpy вместо двух) и рисуем курсор прямо на
            // framebuffer поверх. Тиринг курсора здесь не важен — кадр
            // меняется в течение <1 мс и глаз не успевает поймать артефакт.
            doublebuf_present_scene();
            cursor_draw_to_framebuffer();
        } else {
            // Pervyj kadr, scena escho ne sokhranena: cursor v compose + obychnyj swap.
            cursor_draw();
            doublebuf_swap();
        }
    }
}
