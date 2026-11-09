// ============================================================================
// vga.h - Заголовочный файл драйвера VGA text mode
// ============================================================================

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Константы VGA
// ============================================================================

// Размеры экрана в текстовом режиме
#define VGA_WIDTH  80                           // Ширина экрана (символов)
#define VGA_HEIGHT 25                           // Высота экрана (строк)

// Адрес видеопамяти VGA text mode
#define VGA_MEMORY 0xB8000

// ============================================================================
// Цвета VGA (4-bit палитра)
// ============================================================================

typedef enum {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,
    VGA_COLOR_WHITE         = 15,
} vga_color_t;

// ============================================================================
// Функции драйвера VGA
// ============================================================================

// Глобальные переменные для перехвата вывода в GUI терминал
#include <stdbool.h>
extern bool vga_redirect_to_term;
extern void (*vga_term_hook)(char c);

// Инициализация VGA (очистка экрана, установка цветов по умолчанию)
void vga_init(void);

// Очистка экрана
void vga_clear(void);

// Установка цвета текста и фона
void vga_set_color(vga_color_t fg, vga_color_t bg);

// Вывод одного символа в текущую позицию курсора
void vga_putchar(char c);

// Вывод строки
void vga_puts(const char* str);

// Вывод строки с переводом строки
void vga_println(const char* str);

// Вывод числа в шестнадцатеричном формате
void vga_print_hex(uint32_t num);

// Вывод числа в десятичном формате
void vga_print_dec(uint32_t num);

// Установка позиции курсора
void vga_set_cursor(size_t x, size_t y);

// Получение текущей позиции курсора
void vga_get_cursor(size_t* x, size_t* y);

#endif // VGA_H
