// ============================================================================
// vga.c - Драйвер VGA text mode для MyOS
// ============================================================================
// Реализует базовый вывод текста на экран через VGA text mode
// Видеопамять находится по адресу 0xB8000
// Формат: каждый символ занимает 2 байта (символ + атрибут цвета)
// ============================================================================

#include "vga.h"

// ============================================================================
// Внутренние переменные
// ============================================================================

static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;  // Указатель на видеопамять
static size_t vga_row = 0;                             // Текущая строка курсора
static size_t vga_column = 0;                          // Текущий столбец курсора
static uint8_t vga_color = 0;                          // Текущий цвет (fg + bg)

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Создание байта цвета из цвета текста и фона
// Формат: 4 бита фон | 4 бита текст
static inline uint8_t vga_entry_color(vga_color_t fg, vga_color_t bg) {
    return fg | (bg << 4);
}

// Создание 16-битного значения для видеопамяти (символ + цвет)
static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

// Прокрутка экрана вверх на одну строку
static void vga_scroll(void) {
    // Копируем все строки на одну вверх
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            size_t src_index = (y + 1) * VGA_WIDTH + x;
            size_t dst_index = y * VGA_WIDTH + x;
            vga_buffer[dst_index] = vga_buffer[src_index];
        }
    }

    // Очищаем последнюю строку
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        size_t index = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
        vga_buffer[index] = vga_entry(' ', vga_color);
    }

    // Курсор на начало последней строки
    vga_row = VGA_HEIGHT - 1;
    vga_column = 0;
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация VGA
void vga_init(void) {
    vga_row = 0;
    vga_column = 0;
    vga_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_clear();
}

// Очистка экрана
void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            size_t index = y * VGA_WIDTH + x;
            vga_buffer[index] = vga_entry(' ', vga_color);
        }
    }
    vga_row = 0;
    vga_column = 0;
}

// Установка цвета
void vga_set_color(vga_color_t fg, vga_color_t bg) {
    vga_color = vga_entry_color(fg, bg);
}

bool vga_redirect_to_term = false;
void (*vga_term_hook)(char c) = NULL;

// Вывод одного символа
void vga_putchar(char c) {
    // Перехват вывода для GUI терминала
    if (vga_redirect_to_term && vga_term_hook) {
        vga_term_hook(c);
        return;
    }

    // Обработка специальных символов
    if (c == '\n') {
        // Перевод строки
        vga_column = 0;
        vga_row++;
    } else if (c == '\r') {
        // Возврат каретки
        vga_column = 0;
    } else if (c == '\t') {
        // Табуляция (4 пробела)
        for (int i = 0; i < 4; i++) {
            vga_putchar(' ');
        }
        return;
    } else if (c == '\b') {
        // Backspace
        if (vga_column > 0) {
            vga_column--;
            size_t index = vga_row * VGA_WIDTH + vga_column;
            vga_buffer[index] = vga_entry(' ', vga_color);
        }
        return;
    } else {
        // Обычный символ
        size_t index = vga_row * VGA_WIDTH + vga_column;
        vga_buffer[index] = vga_entry(c, vga_color);
        vga_column++;
    }

    // Переход на новую строку при достижении конца
    if (vga_column >= VGA_WIDTH) {
        vga_column = 0;
        vga_row++;
    }

    // Прокрутка при достижении конца экрана
    if (vga_row >= VGA_HEIGHT) {
        vga_scroll();
    }
}

// Вывод строки
void vga_puts(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        vga_putchar(str[i]);
    }
}

// Вывод строки с переводом строки
void vga_println(const char* str) {
    vga_puts(str);
    vga_putchar('\n');
}

// Вывод числа в шестнадцатеричном формате
void vga_print_hex(uint32_t num) {
    const char hex_chars[] = "0123456789ABCDEF";
    char buffer[11];  // "0x" + 8 цифр + '\0'
    
    buffer[0] = '0';
    buffer[1] = 'x';
    
    for (int i = 7; i >= 0; i--) {
        buffer[2 + i] = hex_chars[num & 0xF];
        num >>= 4;
    }
    
    buffer[10] = '\0';
    vga_puts(buffer);
}

// Вывод числа в десятичном формате
void vga_print_dec(uint32_t num) {
    if (num == 0) {
        vga_putchar('0');
        return;
    }
    
    char buffer[11];  // Максимум 10 цифр для uint32_t + '\0'
    int i = 0;
    
    // Преобразуем число в строку (в обратном порядке)
    while (num > 0) {
        buffer[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    // Выводим в правильном порядке
    for (int j = i - 1; j >= 0; j--) {
        vga_putchar(buffer[j]);
    }
}

// Установка позиции курсора
void vga_set_cursor(size_t x, size_t y) {
    if (x < VGA_WIDTH && y < VGA_HEIGHT) {
        vga_column = x;
        vga_row = y;
    }
}

// Получение позиции курсора
void vga_get_cursor(size_t* x, size_t* y) {
    if (x) *x = vga_column;
    if (y) *y = vga_row;
}
