// ============================================================================
// keyboard.h - Драйвер клавиатуры PS/2 для MyOS
// ============================================================================
// Обрабатывает IRQ1 (клавиатура)
// Преобразует скан-коды в ASCII символы
// Поддерживает Shift, Caps Lock, Ctrl, Alt
// ============================================================================

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Константы клавиатуры
// ============================================================================

// Порты клавиатуры PS/2
#define KEYBOARD_DATA_PORT      0x60    // Порт данных (чтение скан-кодов)
#define KEYBOARD_STATUS_PORT    0x64    // Порт статуса
#define KEYBOARD_COMMAND_PORT   0x64    // Командный порт

// Размер буфера клавиатуры
#define KEYBOARD_BUFFER_SIZE    256

// Специальные скан-коды (make codes - нажатие)
#define KEY_ESCAPE              0x01
#define KEY_BACKSPACE           0x0E
#define KEY_TAB                 0x0F
#define KEY_ENTER               0x1C
#define KEY_LEFT_CTRL           0x1D
#define KEY_LEFT_SHIFT          0x2A
#define KEY_RIGHT_SHIFT         0x36
#define KEY_LEFT_ALT            0x38
#define KEY_CAPS_LOCK           0x3A
#define KEY_F1                  0x3B
#define KEY_F2                  0x3C
#define KEY_F3                  0x3D
#define KEY_F4                  0x3E
#define KEY_F5                  0x3F
#define KEY_F6                  0x40
#define KEY_F7                  0x41
#define KEY_F8                  0x42
#define KEY_F9                  0x43
#define KEY_F10                 0x44
#define KEY_NUM_LOCK            0x45
#define KEY_SCROLL_LOCK         0x46
#define KEY_F11                 0x57
#define KEY_F12                 0x58

// Флаг отпускания клавиши (break code)
#define KEY_RELEASE_FLAG        0x80

// ============================================================================
// Структуры
// ============================================================================

// Состояние модификаторов клавиатуры
typedef struct {
    bool shift;         // Shift нажат
    bool ctrl;          // Ctrl нажат
    bool alt;           // Alt нажат
    bool caps_lock;     // Caps Lock включён
    bool num_lock;      // Num Lock включён
    bool scroll_lock;   // Scroll Lock включён
} keyboard_state_t;

// ============================================================================
// Функции драйвера клавиатуры
// ============================================================================

// Инициализация драйвера клавиатуры
void keyboard_init(void);

// Проверка наличия символа в буфере
bool keyboard_has_key(void);

// Чтение символа из буфера (блокирующее)
// Ожидает нажатия клавиши и возвращает ASCII символ
char keyboard_getchar(void);

// Чтение символа из буфера (неблокирующее)
// Возвращает символ или 0 если буфер пуст
char keyboard_getchar_nonblock(void);

// Чтение строки с клавиатуры (до Enter)
// Параметры:
//   buffer - буфер для строки
//   max_len - максимальная длина (включая '\0')
// Возвращает количество прочитанных символов
int keyboard_gets(char* buffer, int max_len);

// Получение состояния модификаторов
keyboard_state_t keyboard_get_state(void);

// Очистка буфера клавиатуры
void keyboard_clear_buffer(void);

#endif // KEYBOARD_H
