// ============================================================================
// elftest/main.c - Тестовая программа для ELF + GUI (Этап 18)
// ============================================================================
// Демонстрирует:
//   - Создание GUI-окна из ELF-программы
//   - Рисование графики через syscall'ы
//   - Полноценный ELF userspace процесс
// ============================================================================

#include "../libmyos/myos.h"

// ============================================================================
// Рисование градиента в окне
// ============================================================================
static void draw_gradient(uint32_t win, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Создаём красивый градиент
            uint8_t r = (uint8_t)((x * 255) / width);
            uint8_t g = (uint8_t)((y * 255) / height);
            uint8_t b = (uint8_t)(128 + ((x ^ y) & 127));
            uint32_t color = 0xFF000000 | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | b;
            myos_put_pixel(win, x, y, color);
        }
    }
}

// ============================================================================
// Рисование текста (побуквенно через пиксели, 3x5 шрифт)
// ============================================================================

// Простейший 3x5 шрифт для цифр и букв
static const uint8_t mini_font[][5] __attribute__((unused)) = {
    // 'E'
    {0x07, 0x04, 0x07, 0x04, 0x07},
    // 'L'
    {0x04, 0x04, 0x04, 0x04, 0x07},
    // 'F'
    {0x07, 0x04, 0x07, 0x04, 0x04},
};

// ============================================================================
// Главная функция
// ============================================================================

int main(void) {
    // Выводим информацию в консоль
    myos_print("[ELF Test] Zapusk graficheskogo testa...\n");

    // Создаём окно
    int win_w = 200;
    int win_h = 150;
    uint32_t win = myos_create_window("ELF Gradient", 300, 200, win_w, win_h);

    if (win == 0) {
        myos_print("[ELF Test] Oshibka sozdaniya okna!\n");
        return 1;
    }

    myos_print("[ELF Test] Okno sozdano, risovanie gradienta...\n");

    // Рисуем градиент
    draw_gradient(win, win_w, win_h);

    // Рисуем рамку
    for (int x = 0; x < win_w; x++) {
        myos_put_pixel(win, x, 0, 0xFFFFFFFF);
        myos_put_pixel(win, x, win_h - 1, 0xFFFFFFFF);
    }
    for (int y = 0; y < win_h; y++) {
        myos_put_pixel(win, 0, y, 0xFFFFFFFF);
        myos_put_pixel(win, win_w - 1, y, 0xFFFFFFFF);
    }

    // Запрашиваем перерисовку
    myos_redraw();

    myos_print("[ELF Test] Gradient narisovan!\n");

    // Бесконечный цикл (программа остаётся активной)
    while (1) {
        // Используем пустой цикл — в будущем заменим на myos_sleep()
        for (volatile int i = 0; i < 1000000; i++);
    }

    return 0;
}
