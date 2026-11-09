// ============================================================================
// desktop.c - Реализация иконок рабочего стола для MyOS
// ============================================================================

#include "desktop.h"
#include "graphics.h"
#include "font.h"
#include "wm.h"
#include "framebuffer.h"
#include "../memory/memory.h"
#include <stddef.h>

// ============================================================================
// Внутреннее состояние
// ============================================================================

static desktop_icon_t icons[DESKTOP_MAX_ICONS];
static int icon_count = 0;

// Счётчик для авто-размещения иконок в сетке
static int next_grid_x = 0;
static int next_grid_y = 0;

// Пользовательские файлы на рабочем столе (см. desktop.h).
static desktop_file_t user_files[DESKTOP_MAX_USER_FILES];
static int user_file_count = 0;

// Контекстное меню (правый клик по рабочему столу).
static bool    ctx_menu_open   = false;
static int32_t ctx_menu_x      = 0;
static int32_t ctx_menu_y      = 0;
#define CTX_MENU_W  180
#define CTX_MENU_H  96
#define CTX_MENU_ROW_H 32

// Notes-редактор: apps.c предоставляет способ открыть его для конкретного
// пользовательского файла рабочего стола.
extern void notes_open_for_file(int file_idx);

// Forward-декларации локальных утилит (сами функции определены ниже,
// но используются раньше — в desktop_draw_icons / desktop_handle_click).
static void user_file_screen_pos(int slot, int32_t* out_x, int32_t* out_y);
static void draw_user_file_icons(void);

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void str_ncopy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

// Получить экранные координаты иконки
static void icon_screen_pos(desktop_icon_t* icon, int32_t* out_x, int32_t* out_y) {
    *out_x = DESKTOP_ICON_START_X + icon->grid_x * DESKTOP_ICON_SPACING;
    *out_y = DESKTOP_ICON_START_Y + icon->grid_y * DESKTOP_ICON_SPACING;
}

// Отрисовка одной иконки
static void draw_icon(desktop_icon_t* icon) {
    int32_t ix, iy;
    icon_screen_pos(icon, &ix, &iy);
    
    // Тень иконки
    gfx_fill_rect(ix + 3, iy + 3, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, 0x00102030);
    
    // Фон иконки (с лёгким градиентом — верх чуть светлее)
    uint32_t light = color_lighten(icon->icon_color, 60);
    uint32_t dark = color_darken(icon->icon_color, 180);
    gfx_fill_rect_gradient(ix, iy, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, light, dark);
    
    // Рамка
    gfx_draw_rect(ix, iy, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, COLOR_WHITE);
    
    // Символ на иконке (центрированный)
    int sym_w = font_string_width(icon->icon_symbol);
    int sym_x = ix + (DESKTOP_ICON_SIZE - sym_w) / 2;
    int sym_y = iy + (DESKTOP_ICON_SIZE - FONT_HEIGHT) / 2;
    font_draw_string(sym_x, sym_y, icon->icon_symbol, COLOR_WHITE, 0xFF000000);
    
    // Подпись под иконкой (центрированная)
    int name_w = font_string_width(icon->name);
    int name_x = ix + (DESKTOP_ICON_SIZE - name_w) / 2;
    int name_y = iy + DESKTOP_ICON_SIZE + 4;
    
    // Тень текста
    font_draw_string(name_x + 1, name_y + 1, icon->name, 0x00000000, 0xFF000000);
    // Текст
    font_draw_string(name_x, name_y, icon->name, COLOR_WHITE, 0xFF000000);
}

// ============================================================================
// Внешний API
// ============================================================================

void desktop_init(void) {
    memset(icons, 0, sizeof(icons));
    icon_count = 0;
    next_grid_x = 0;
    next_grid_y = 0;
}

int desktop_add_icon(const char* name, const char* symbol, uint32_t color,
                     const char* win_title, int32_t win_w, int32_t win_h,
                     app_callback_t on_create, app_update_t on_update,
                     app_click_t on_click, app_key_t on_key) {
    if (icon_count >= DESKTOP_MAX_ICONS) return -1;
    
    desktop_icon_t* icon = &icons[icon_count];
    
    str_ncopy(icon->name, name, 32);
    str_ncopy(icon->icon_symbol, symbol, 4);
    str_ncopy(icon->window_title, win_title, 64);
    icon->icon_color = color;
    icon->window_w = win_w;
    icon->window_h = win_h;
    icon->on_create = on_create;
    icon->on_update = on_update;
    icon->on_click = on_click;
    icon->on_key = on_key;
    icon->opened_window = NULL;
    icon->active = true;
    
    // Авто-размещение в сетке
    icon->grid_x = next_grid_x;
    icon->grid_y = next_grid_y;
    
    next_grid_y++;
    if (next_grid_y >= DESKTOP_ICONS_PER_COL) {
        next_grid_y = 0;
        next_grid_x++;
    }
    
    return icon_count++;
}

void desktop_draw_icons(void) {
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].active) {
            draw_icon(&icons[i]);
        }
    }
    // После системных — пользовательские файлы (правая колонка).
    draw_user_file_icons();
}

bool desktop_handle_click(int32_t mx, int32_t my) {
    // 1) Системные иконки приложений.
    for (int i = 0; i < icon_count; i++) {
        if (!icons[i].active) continue;
        
        int32_t ix, iy;
        icon_screen_pos(&icons[i], &ix, &iy);
        
        if (mx >= ix && mx < ix + DESKTOP_ICON_SIZE &&
            my >= iy && my < iy + DESKTOP_ICON_SIZE + 20) {
            desktop_launch_icon(i);
            return true;
        }
    }

    // 2) Иконки пользовательских файлов (правая колонка) → открыть в Notes.
    int slot = 0;
    for (int i = 0; i < DESKTOP_MAX_USER_FILES; i++) {
        if (!user_files[i].used) continue;
        int32_t ix, iy;
        user_file_screen_pos(slot, &ix, &iy);
        slot++;
        if (mx >= ix && mx < ix + DESKTOP_ICON_SIZE &&
            my >= iy && my < iy + DESKTOP_ICON_SIZE + 20) {
            notes_open_for_file(i);
            return true;
        }
    }
    return false;
}

void desktop_handle_window_click(window_t* win, int32_t x, int32_t y) {
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].opened_window == win && icons[i].on_click) {
            icons[i].on_click(win, x, y);
            return;
        }
    }
}

void desktop_update_apps(void) {
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].active && icons[i].opened_window && icons[i].on_update) {
            icons[i].on_update(icons[i].opened_window);
        }
    }
}

void desktop_notify_window_closed(window_t* win) {
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].opened_window == win) {
            icons[i].opened_window = NULL;
            return;
        }
    }
}

void desktop_notify_window_resized(window_t* win) {
    // После window_toggle_maximize размер окна изменился, пиксельный
    // буфер — очищен. Находим соответствующее приложение и просим его
    // перерисовать всё заново через on_create.
    if (!win) return;
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].opened_window == win && icons[i].on_create) {
            icons[i].on_create(win);
            return;
        }
    }
}

bool desktop_handle_key(window_t* win, char key) {
    if (!win || key == 0) return false;

    for (int i = 0; i < icon_count; i++) {
        if (icons[i].opened_window == win && icons[i].on_key) {
            icons[i].on_key(win, key);
            return true;
        }
    }
    return false;
}

int desktop_get_icon_count(void) {
    return icon_count;
}

const char* desktop_get_icon_name(int idx) {
    if (idx < 0 || idx >= icon_count || !icons[idx].active) return NULL;
    return icons[idx].name;
}

const char* desktop_get_icon_symbol(int idx) {
    if (idx < 0 || idx >= icon_count || !icons[idx].active) return NULL;
    return icons[idx].icon_symbol;
}

uint32_t desktop_get_icon_color(int idx) {
    if (idx < 0 || idx >= icon_count || !icons[idx].active) return 0xFF888888u;
    return icons[idx].icon_color;
}

bool desktop_launch_icon(int idx) {
    if (idx < 0 || idx >= icon_count || !icons[idx].active) return false;
    if (icons[idx].opened_window) {
        // Если окно уже открыто — просто поднимаем его на передний план
        // (WM отследит фокус).
        wm_require_redraw();
        return true;
    }

    int32_t screen_w = (int32_t)framebuffer_get_width();
    int32_t screen_h = (int32_t)framebuffer_get_height();
    int32_t wx = (screen_w - icons[idx].window_w) / 2 + (idx % 3) * 30;
    int32_t wy = (screen_h - icons[idx].window_h) / 2 + (idx % 4) * 20 - 60;
    if (wy < 30) wy = 30;

    window_t* win = window_create(icons[idx].window_title, wx, wy,
                                  icons[idx].window_w, icons[idx].window_h);
    if (!win) return false;
    icons[idx].opened_window = win;
    if (icons[idx].on_create) icons[idx].on_create(win);
    wm_add_window(win);
    return true;
}

// ============================================================================
// Пользовательские файлы
// ============================================================================

int desktop_create_file(void) {
    for (int i = 0; i < DESKTOP_MAX_USER_FILES; i++) {
        if (!user_files[i].used) {
            user_files[i].used = true;
            // Имя по умолчанию: "note_N.txt" (номер слота +1)
            user_files[i].name[0] = 'n';
            user_files[i].name[1] = 'o';
            user_files[i].name[2] = 't';
            user_files[i].name[3] = 'e';
            user_files[i].name[4] = '_';
            int n = i + 1;
            char digit = (char)('0' + (n % 10));
            user_files[i].name[5] = digit;
            user_files[i].name[6] = '.';
            user_files[i].name[7] = 't';
            user_files[i].name[8] = 'x';
            user_files[i].name[9] = 't';
            user_files[i].name[10] = '\0';
            user_files[i].content[0] = '\0';
            user_file_count++;
            return i;
        }
    }
    return -1;
}

desktop_file_t* desktop_get_file(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_USER_FILES) return NULL;
    if (!user_files[idx].used) return NULL;
    return &user_files[idx];
}

bool desktop_save_file(int idx, const char* buf, int len) {
    desktop_file_t* f = desktop_get_file(idx);
    if (!f) return false;
    if (len < 0) len = 0;
    if (len >= DESKTOP_FILE_CONTENT_LEN) len = DESKTOP_FILE_CONTENT_LEN - 1;
    for (int i = 0; i < len; i++) f->content[i] = buf[i];
    f->content[len] = '\0';
    return true;
}

void desktop_delete_file(int idx) {
    if (idx < 0 || idx >= DESKTOP_MAX_USER_FILES) return;
    if (user_files[idx].used) {
        user_files[idx].used = false;
        user_files[idx].name[0] = '\0';
        user_files[idx].content[0] = '\0';
        user_file_count--;
    }
}

int desktop_get_file_count(void) {
    return user_file_count;
}

// Экранная позиция иконки пользовательского файла: размещаем их в правом
// столбце, чтобы не мешали системным иконкам (левая колонка).
static void user_file_screen_pos(int slot, int32_t* out_x, int32_t* out_y) {
    int32_t screen_w = (int32_t)framebuffer_get_width();
    *out_x = screen_w - DESKTOP_ICON_START_X - DESKTOP_ICON_SIZE;
    *out_y = DESKTOP_ICON_START_Y + slot * DESKTOP_ICON_SPACING;
}

static void draw_user_file_icon(int slot, desktop_file_t* f) {
    int32_t ix, iy;
    user_file_screen_pos(slot, &ix, &iy);

    // Стиль «документ»: белый лист с загнутым уголком.
    uint32_t paper   = 0xFFF5F6FA;
    uint32_t shadow  = 0x00102030;
    uint32_t fold    = 0xFFC9CCD4;
    uint32_t line    = 0xFFA0A4B0;

    // Тень
    gfx_fill_rect(ix + 3, iy + 3, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, shadow);
    // Лист
    gfx_fill_rect(ix, iy, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, paper);
    // Уголок (верхний-правый)
    int fold_sz = 12;
    for (int y = 0; y < fold_sz; y++) {
        for (int x = fold_sz - y; x < fold_sz; x++) {
            gfx_put_pixel(ix + DESKTOP_ICON_SIZE - fold_sz + x, iy + y, fold);
        }
    }
    // Линии «текста»
    for (int r = 0; r < 4; r++) {
        int ly = iy + 18 + r * 7;
        gfx_hline(ix + 6, ly, DESKTOP_ICON_SIZE - 12 - (r == 3 ? 10 : 0), line);
    }
    // Рамка
    gfx_draw_rect(ix, iy, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, 0xFFA0A4B0);

    // Имя
    int name_w = font_string_width(f->name);
    int name_x = ix + (DESKTOP_ICON_SIZE - name_w) / 2;
    int name_y = iy + DESKTOP_ICON_SIZE + 4;
    font_draw_string(name_x + 1, name_y + 1, f->name, 0x00000000, 0xFF000000);
    font_draw_string(name_x, name_y, f->name, 0xFFFFFFFF, 0xFF000000);
}

// Рисует все user-file иконки после системных (вызывается из desktop_draw_icons).
static void draw_user_file_icons(void) {
    int slot = 0;
    for (int i = 0; i < DESKTOP_MAX_USER_FILES; i++) {
        if (user_files[i].used) {
            draw_user_file_icon(slot, &user_files[i]);
            slot++;
        }
    }
}

// ============================================================================
// Контекстное меню
// ============================================================================

bool desktop_context_menu_is_open(void) { return ctx_menu_open; }

bool desktop_context_menu_point_inside(int32_t mx, int32_t my) {
    if (!ctx_menu_open) return false;
    return mx >= ctx_menu_x && mx < ctx_menu_x + CTX_MENU_W &&
           my >= ctx_menu_y && my < ctx_menu_y + CTX_MENU_H;
}

void desktop_context_menu_open(int32_t x, int32_t y) {
    int32_t sw = (int32_t)framebuffer_get_width();
    int32_t sh = (int32_t)framebuffer_get_height();
    // Клипуем, чтобы меню не вылезало за экран
    if (x + CTX_MENU_W > sw) x = sw - CTX_MENU_W - 4;
    if (y + CTX_MENU_H > sh) y = sh - CTX_MENU_H - 4;
    ctx_menu_x = x;
    ctx_menu_y = y;
    ctx_menu_open = true;
    wm_require_redraw();
}

void desktop_context_menu_close(void) {
    if (!ctx_menu_open) return;
    ctx_menu_open = false;
    wm_require_redraw();
}

void desktop_context_menu_draw(void) {
    if (!ctx_menu_open) return;

    // Тень
    for (int d = 1; d < 6; d++) {
        int a = (6 - d) * 16;
        (void)a;
        gfx_fill_rect(ctx_menu_x + d, ctx_menu_y + d,
                      CTX_MENU_W, CTX_MENU_H, 0xFF101218);
    }
    // Фон
    gfx_fill_rect_gradient(ctx_menu_x, ctx_menu_y, CTX_MENU_W, CTX_MENU_H,
                           0xFF2C3244, 0xFF1A1E2A);
    gfx_draw_rect(ctx_menu_x, ctx_menu_y, CTX_MENU_W, CTX_MENU_H, 0xFF4A5068);

    // Пункты
    const char* items[3] = {
        "Новый файл",
        "Обновить",
        "Закрыть",
    };
    for (int i = 0; i < 3; i++) {
        int ry = ctx_menu_y + 4 + i * CTX_MENU_ROW_H;
        font_draw_string(ctx_menu_x + 14, ry + (CTX_MENU_ROW_H - FONT_HEIGHT) / 2,
                         items[i], 0xFFEEEEF2, 0xFF000000);
        if (i < 2) {
            gfx_hline(ctx_menu_x + 6, ry + CTX_MENU_ROW_H - 1,
                      CTX_MENU_W - 12, 0xFF3A4058);
        }
    }
}

bool desktop_context_menu_handle_click(int32_t mx, int32_t my) {
    if (!ctx_menu_open) return false;
    if (!desktop_context_menu_point_inside(mx, my)) {
        desktop_context_menu_close();
        return false;
    }

    int row = (my - ctx_menu_y - 4) / CTX_MENU_ROW_H;
    if (row < 0) row = 0;
    if (row > 2) row = 2;

    switch (row) {
        case 0: {
            int idx = desktop_create_file();
            if (idx >= 0) {
                notes_open_for_file(idx);
            }
            break;
        }
        case 1:
            wm_require_redraw();
            break;
        case 2:
        default:
            break;
    }
    desktop_context_menu_close();
    return true;
}
