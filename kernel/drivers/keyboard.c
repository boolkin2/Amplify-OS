// ============================================================================
// keyboard.c - Реализация драйвера клавиатуры PS/2 для MyOS
// ============================================================================

#include "keyboard.h"
#include "../arch/i386/isr.h"
#include "../arch/i386/idt.h"
#include "../io.h"
#include "../vga.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Кольцевой буфер клавиатуры
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static int buffer_read_pos = 0;     // Позиция чтения
static int buffer_write_pos = 0;    // Позиция записи
static int buffer_count = 0;        // Количество символов в буфере

// Состояние модификаторов
static keyboard_state_t kbd_state = {
    .shift = false,
    .ctrl = false,
    .alt = false,
    .caps_lock = false,
    .num_lock = false,
    .scroll_lock = false
};

// ============================================================================
// Таблица скан-кодов → ASCII (US раскладка)
// ============================================================================

// Таблица для обычных клавиш (без Shift)
static const char scancode_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',     // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', '\t',    // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',      // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,   'a', 's',     // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',      // 0x20-0x27
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',    // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',      // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,        // 0x38-0x3F (Alt, Space, Caps, F1-F5)
    0,   0,   0,   0,   0,   0,   0,   '7',      // 0x40-0x47 (F6-F10, Num, Scroll, Home/7)
    '8', '9', '-', '4', '5', '6', '+', '1',      // 0x48-0x4F (Numpad)
    '2', '3', '0', '.', 0,   0,   0,   0,        // 0x50-0x57 (Numpad, F11)
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x58-0x5F (F12)
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0         // 0x78-0x7F
};

// Таблица для клавиш с Shift
static const char scancode_to_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',     // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', '\t',    // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',      // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',     // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',      // 0x20-0x27
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',      // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',      // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,        // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',      // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',      // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,        // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x58-0x5F
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,        // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0         // 0x78-0x7F
};

// ============================================================================
// Внутренние функции
// ============================================================================

// Добавление символа в буфер
static void keyboard_buffer_put(char c) {
    if (buffer_count < KEYBOARD_BUFFER_SIZE) {
        keyboard_buffer[buffer_write_pos] = c;
        buffer_write_pos = (buffer_write_pos + 1) % KEYBOARD_BUFFER_SIZE;
        buffer_count++;
    }
}

// Извлечение символа из буфера
static char keyboard_buffer_get(void) {
    if (buffer_count > 0) {
        char c = keyboard_buffer[buffer_read_pos];
        buffer_read_pos = (buffer_read_pos + 1) % KEYBOARD_BUFFER_SIZE;
        buffer_count--;
        return c;
    }
    return 0;
}

// Преобразование скан-кода в ASCII
static char scancode_to_char(uint8_t scancode) {
    // Проверяем диапазон
    if (scancode >= 128) {
        return 0;
    }

    char c;
    
    // Выбираем таблицу в зависимости от Shift
    if (kbd_state.shift) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }

    // Применяем Caps Lock для букв
    if (kbd_state.caps_lock && c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';  // Переводим в верхний регистр
    } else if (kbd_state.caps_lock && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';  // Переводим в нижний регистр (если Shift+CapsLock)
    }

    return c;
}

// Обработчик прерывания клавиатуры (IRQ1)
static uint32_t keyboard_irq_handler(registers_t* regs) {
    // Читаем скан-код из порта данных
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    // Проверяем флаг отпускания клавиши
    bool key_released = (scancode & KEY_RELEASE_FLAG) != 0;
    scancode &= ~KEY_RELEASE_FLAG;  // Убираем флаг

    // Обрабатываем модификаторы
    if (scancode == KEY_LEFT_SHIFT || scancode == KEY_RIGHT_SHIFT) {
        kbd_state.shift = !key_released;
        return (uint32_t)regs;
    }
    
    if (scancode == KEY_LEFT_CTRL) {
        kbd_state.ctrl = !key_released;
        return (uint32_t)regs;
    }
    
    if (scancode == KEY_LEFT_ALT) {
        kbd_state.alt = !key_released;
        return (uint32_t)regs;
    }

    // Caps Lock переключается при нажатии (не при отпускании)
    if (scancode == KEY_CAPS_LOCK && !key_released) {
        kbd_state.caps_lock = !kbd_state.caps_lock;
        return (uint32_t)regs;
    }

    // Num Lock переключается при нажатии
    if (scancode == KEY_NUM_LOCK && !key_released) {
        kbd_state.num_lock = !kbd_state.num_lock;
        return (uint32_t)regs;
    }

    // Scroll Lock переключается при нажатии
    if (scancode == KEY_SCROLL_LOCK && !key_released) {
        kbd_state.scroll_lock = !kbd_state.scroll_lock;
        return (uint32_t)regs;
    }

    // Обрабатываем только нажатия (не отпускания)
    if (key_released) {
        return (uint32_t)regs;
    }

    // Преобразуем скан-код в ASCII
    char c = scancode_to_char(scancode);

    // Если получили валидный символ - добавляем в буфер
    if (c != 0) {
        keyboard_buffer_put(c);
    }
    
    return (uint32_t)regs;
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация драйвера клавиатуры
void keyboard_init(void) {
    // Очищаем буфер
    buffer_read_pos = 0;
    buffer_write_pos = 0;
    buffer_count = 0;

    // Сбрасываем состояние модификаторов
    kbd_state.shift = false;
    kbd_state.ctrl = false;
    kbd_state.alt = false;
    kbd_state.caps_lock = false;
    kbd_state.num_lock = false;
    kbd_state.scroll_lock = false;

    // Регистрируем обработчик IRQ1 (клавиатура)
    isr_register_handler(IRQ1, keyboard_irq_handler);
}

// Проверка наличия символа в буфере
bool keyboard_has_key(void) {
    return buffer_count > 0;
}

// Чтение символа из буфера (блокирующее)
char keyboard_getchar(void) {
    // Ожидаем появления символа в буфере
    while (!keyboard_has_key()) {
        // Ждём прерывания от клавиатуры
        __asm__ volatile ("hlt");
    }
    
    return keyboard_buffer_get();
}

// Чтение символа из буфера (неблокирующее)
char keyboard_getchar_nonblock(void) {
    if (keyboard_has_key()) {
        return keyboard_buffer_get();
    }
    return 0;
}

// Чтение строки с клавиатуры (до Enter)
int keyboard_gets(char* buffer, int max_len) {
    int pos = 0;
    
    while (pos < max_len - 1) {
        char c = keyboard_getchar();
        
        // Enter - конец ввода
        if (c == '\n') {
            vga_putchar('\n');
            break;
        }
        
        // Backspace - удаление символа
        if (c == '\b') {
            if (pos > 0) {
                pos--;
                vga_putchar('\b');  // Стираем символ на экране
            }
            continue;
        }
        
        // Обычный символ
        buffer[pos++] = c;
        vga_putchar(c);  // Эхо на экран
    }
    
    buffer[pos] = '\0';  // Завершающий ноль
    return pos;
}

// Получение состояния модификаторов
keyboard_state_t keyboard_get_state(void) {
    return kbd_state;
}

// Очистка буфера клавиатуры
void keyboard_clear_buffer(void) {
    buffer_read_pos = 0;
    buffer_write_pos = 0;
    buffer_count = 0;
}
