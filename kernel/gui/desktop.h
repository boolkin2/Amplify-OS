// ============================================================================
// desktop.h - Иконки рабочего стола для MyOS
// ============================================================================
// Система иконок на рабочем столе. Каждая иконка — цветной квадрат 48×48
// с подписью внизу. Клик по иконке запускает приложение (создаёт окно).
// ============================================================================

#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"

// ============================================================================
// Константы
// ============================================================================

#define DESKTOP_MAX_ICONS       16
#define DESKTOP_ICON_SIZE       48      // Размер иконки (пикселей)
#define DESKTOP_ICON_SPACING    90      // Расстояние между иконками
#define DESKTOP_ICON_START_X    30      // Отступ слева
#define DESKTOP_ICON_START_Y    30      // Отступ сверху
#define DESKTOP_ICONS_PER_COL   6       // Иконок в столбце

// ============================================================================
// Типы
// ============================================================================

// Callback-функция приложения: получает окно, рисует содержимое
typedef void (*app_callback_t)(window_t* win);

// Callback-функция обновления (для живых приложений типа часов)
typedef void (*app_update_t)(window_t* win);

// Callback-функция обработки клика внутри окна
typedef void (*app_click_t)(window_t* win, int32_t x, int32_t y);

// Callback-функция обработки клавиши (для интерактивных приложений)
typedef void (*app_key_t)(window_t* win, char key);

// Иконка на рабочем столе
typedef struct {
    char name[32];              // Название (подпись под иконкой)
    uint32_t icon_color;        // Цвет иконки
    char icon_symbol[4];        // Символ на иконке (1-2 символа)
    int32_t grid_x;             // Позиция в сетке (столбец)
    int32_t grid_y;             // Позиция в сетке (строка)
    bool active;                // Иконка зарегистрирована
    
    // Параметры создаваемого окна
    char window_title[64];
    int32_t window_w;
    int32_t window_h;
    
    // Callbacks
    app_callback_t on_create;   // Вызывается при создании окна (рисует начальное содержимое)
    app_update_t on_update;     // Вызывается периодически (для живых приложений)
    app_click_t on_click;       // Вызывается при клике в содержимое окна
    app_key_t on_key;           // Вызывается при нажатии клавиши (когда окно в фокусе)
    
    // Ссылка на открытое окно (NULL если не открыто)
    window_t* opened_window;
} desktop_icon_t;

// ============================================================================
// Функции
// ============================================================================

// Инициализация рабочего стола
void desktop_init(void);

// Добавить иконку на рабочий стол
// Возвращает индекс иконки или -1 при ошибке
int desktop_add_icon(const char* name, const char* symbol, uint32_t color,
                     const char* win_title, int32_t win_w, int32_t win_h,
                     app_callback_t on_create, app_update_t on_update,
                     app_click_t on_click, app_key_t on_key);

// Отрисовка всех иконок на рабочем столе (в double buffer)
void desktop_draw_icons(void);

// Обработка клика по рабочему столу (проверяет попадание в иконки)
// Возвращает true, если клик попал в иконку
bool desktop_handle_click(int32_t mx, int32_t my);

// Обработка клика внутри окна приложения
void desktop_handle_window_click(window_t* win, int32_t x, int32_t y);

// Обновление живых приложений (вызывается из wm_update периодически)
void desktop_update_apps(void);

// Уведомление о закрытии окна (очищает ссылку)
void desktop_notify_window_closed(window_t* win);

// Уведомление о ресайзе окна (maximize/restore). Находит иконку по win и
// переподнимает on_create, чтобы приложение перерисовало содержимое под
// новый win->width/height. Для multi-instance приложений (Notes, File
// Explorer) on_create правильно обрабатывает повторный вызов.
void desktop_notify_window_resized(window_t* win);

// Обработка нажатия клавиши (передаёт приложению в фокусированном окне)
// Возвращает true если приложение обработало клавишу
bool desktop_handle_key(window_t* win, char key);

// Получить количество зарегистрированных иконок
int desktop_get_icon_count(void);

// Доступ к полям иконки по индексу (для меню Пуск).
const char* desktop_get_icon_name(int idx);
const char* desktop_get_icon_symbol(int idx);
uint32_t    desktop_get_icon_color(int idx);

// Запустить приложение по индексу (эквивалент клика по иконке). Создаёт
// окно (если ещё не открыто) и выводит его в WM. Возвращает true при успехе.
bool desktop_launch_icon(int idx);

// ============================================================================
// Пользовательские файлы на рабочем столе (Этап 22++)
// ============================================================================
// Файлы хранятся в in-memory хранилище (не персистентны между загрузками —
// для настоящего сохранения нужен FAT32-write). У каждого файла есть имя
// и текстовое содержимое. Отображаются на рабочем столе как иконки
// специального цвета; клик — открытие в простом редакторе (Notes).

#define DESKTOP_MAX_USER_FILES    12
#define DESKTOP_FILE_NAME_LEN     24
#define DESKTOP_FILE_CONTENT_LEN  4096

typedef struct {
    bool used;
    char name[DESKTOP_FILE_NAME_LEN];
    char content[DESKTOP_FILE_CONTENT_LEN];
} desktop_file_t;

// Создать новый файл на рабочем столе с дефолтным именем/пустым содержимым.
// Возвращает индекс файла или -1 при переполнении.
int desktop_create_file(void);

// Получить указатель на файл по индексу (или NULL).
desktop_file_t* desktop_get_file(int idx);

// Сохранить содержимое файла (копирует buf длины len). Возвращает true
// при успехе.
bool desktop_save_file(int idx, const char* buf, int len);

// Удалить файл.
void desktop_delete_file(int idx);

// Количество занятых слотов в user-files.
int desktop_get_file_count(void);

// ============================================================================
// Контекстное меню рабочего стола (правый клик)
// ============================================================================

bool desktop_context_menu_is_open(void);
bool desktop_context_menu_point_inside(int32_t mx, int32_t my);
void desktop_context_menu_open(int32_t x, int32_t y);
void desktop_context_menu_close(void);
void desktop_context_menu_draw(void);
bool desktop_context_menu_handle_click(int32_t mx, int32_t my);

#endif
