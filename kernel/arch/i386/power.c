// ============================================================================
// power.c - reboot / shutdown
// ============================================================================

#include "power.h"
#include "../../io.h"
#include "../../gui/graphics.h"
#include "../../gui/font.h"
#include "../../gui/framebuffer.h"
#include "../../gui/doublebuf.h"
#include "../../drivers/timer.h"
#include <stdint.h>

void power_reboot(void) {
    // Ждём, пока буфер ввода 8042 освободится.
    uint8_t status = 0x02;
    while (status & 0x02) {
        status = inb(0x64);
    }
    // Команда 0xFE — pulse reset line CPU.
    outb(0x64, 0xFE);
    // На случай если порт не сработал — бесконечный hlt.
    for (;;) __asm__ volatile("cli; hlt");
}

void power_shutdown(void) {
    // QEMU: ACPI soft-off через порт 0x604 (isa-debug-exit) / 0xB004 (legacy).
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    // VirtualBox:
    outw(0x4004, 0x3400);
    // Если ничего не помогло — замораживаем CPU. Экран остаётся в последнем
    // состоянии (curtain «Shutting down...», если мы нарисовали его).
    for (;;) __asm__ volatile("cli; hlt");
}

// ============================================================================
// «Шторка» — фоновый красивый экран при выключении / перезагрузке
// ============================================================================

// Маленький встроенный «логотип» AMPLIFY-OS — стилизованный треугольник
// с восклицательным знаком (имитация фирменной иконки в нижней панели).
static void draw_amplify_logo(int32_t cx, int32_t cy, int32_t size) {
    // Сам треугольник — градиентом
    int32_t s = size;
    // Заливаем построчно: треугольник равнобедренный, основание внизу.
    for (int32_t y = 0; y < s; y++) {
        int32_t half_w = (y * (s/2)) / s;
        int32_t color_t = 0xFFFFAA22;   // янтарный сверху
        int32_t color_b = 0xFFCC4422;   // оранжевый снизу
        // Линейная интерполяция R/G/B
        uint32_t a = (uint32_t)color_t, b = (uint32_t)color_b;
        uint32_t cr = (((a >> 16) & 0xFF) * (s - y) + ((b >> 16) & 0xFF) * y) / s;
        uint32_t cg = (((a >>  8) & 0xFF) * (s - y) + ((b >>  8) & 0xFF) * y) / s;
        uint32_t cb = (((a      ) & 0xFF) * (s - y) + ((b      ) & 0xFF) * y) / s;
        uint32_t col = 0xFF000000 | (cr << 16) | (cg << 8) | cb;
        gfx_fill_rect(cx - half_w, cy - s/2 + y, half_w * 2, 1, col);
    }
    // Восклицательный знак внутри
    int32_t bar_w = s / 8; if (bar_w < 2) bar_w = 2;
    int32_t bar_h = (s * 4) / 10;
    gfx_fill_rect(cx - bar_w/2, cy - bar_h/2 + 4, bar_w, bar_h, 0xFF15171E);
    int32_t dot = bar_w + 1;
    gfx_fill_rect(cx - dot/2, cy + bar_h/2 + 4 + 4, dot, dot, 0xFF15171E);
}

// Нарисовать одно полное состояние шторки: фон + логотип + текст +
// строка точек (сколько точек определяется параметром `dots` 0..3) +
// «спиннер» (вращающийся индикатор) — фаза от phase 0..7.
static void draw_curtain(const char* main_text, int dots, int spinner_phase) {
    int32_t W = (int32_t)framebuffer_get_width();
    int32_t H = (int32_t)framebuffer_get_height();

    // Глубокий тёмно-синий фон с тонким вертикальным градиентом
    gfx_fill_rect_gradient(0, 0, W, H, 0xFF0A1428, 0xFF050A14);

    // Логотип в центре сверху
    int32_t logo_size = 100;
    int32_t cy = H / 2 - 60;
    draw_amplify_logo(W / 2, cy, logo_size);

    // «AMPLIFY-OS» — крупная надпись (используем встроенный 8x16 шрифт,
    // увеличиваем за счёт повторного рендера: рисуем 4 раза с фейковым шейдом).
    const char* brand = "AMPLIFY-OS";
    int32_t brand_w = font_string_width(brand) * 2;
    // Имитация bold: три прохода с лёгким смещением
    int32_t bx = (W - brand_w) / 2;
    int32_t by = cy + logo_size / 2 + 16;
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            // Один проход «обычным» размером, центрируя
            font_draw_string(bx + dx + (brand_w / 4), by + dy,
                             brand, 0xFF101622, 0xFF000000);
        }
    }
    font_draw_string(bx + (brand_w / 4), by, brand, 0xFFFFE0A0, 0xFF000000);

    // Главный текст
    int32_t mw = font_string_width(main_text);
    int32_t my = by + 32;
    font_draw_string((W - mw) / 2, my, main_text, 0xFFE8EAF2, 0xFF000000);

    // Анимация точек (после main_text)
    char dots_buf[8];
    int p = 0;
    for (int i = 0; i < dots && p < 6; i++) dots_buf[p++] = '.';
    while (p < 3) dots_buf[p++] = ' ';
    dots_buf[p] = 0;
    font_draw_string((W - mw) / 2 + mw + 4, my,
                     dots_buf, 0xFFE8EAF2, 0xFF000000);

    // Спиннер — крутящаяся «звёздочка» из 8 точек
    int32_t sx = W / 2;
    int32_t sy = my + 36;
    int32_t r  = 14;
    // Простая таблица направлений (8 углов, 0..7)
    static const int8_t dxs[8] = {  0,  10,  14,  10,   0, -10, -14, -10 };
    static const int8_t dys[8] = {-14, -10,   0,  10,  14,  10,   0, -10 };
    for (int i = 0; i < 8; i++) {
        // Яркость убывает по фазе
        int phase = (spinner_phase + i) & 7;
        uint8_t lum = (uint8_t)(60 + phase * 24);
        uint32_t c = 0xFF000000 | ((uint32_t)lum << 16) |
                                  ((uint32_t)lum <<  8) |
                                   (uint32_t)lum;
        // Мини-квадрат 4x4
        gfx_fill_rect(sx + dxs[i] - 2, sy + dys[i] - 2, 4, 4, c);
    }
    // Обнуляем переиспользование переменной r (компилятор подсветит unused)
    (void)r;

    // Подпись внизу
    const char* tip = "Не выключайте питание до конца операции";
    int32_t tw = font_string_width(tip);
    font_draw_string((W - tw) / 2, H - 40, tip,
                     0xFF55668A, 0xFF000000);

    // Выводим кадр на экран (минуем композитор окна)
    doublebuf_swap();
}

// Подождать ~ms миллисекунд через timer_get_ticks (1 тик ≈ 10 мс @ 100 Гц).
static void busy_sleep_ms(uint32_t ms) {
    uint64_t target = timer_get_uptime_ms() + ms;
    while (timer_get_uptime_ms() < target) {
        __asm__ volatile ("sti; hlt");
    }
}

static void run_curtain(const char* main_text) {
    // 9 шагов по ~100 мс = ~0.9 секунды красивой анимации
    for (int step = 0; step < 9; step++) {
        int dots = (step % 4);
        int spinner = step;
        draw_curtain(main_text, dots, spinner);
        busy_sleep_ms(100);
    }
}

void power_shutdown_with_curtain(void) {
    run_curtain("Завершение работы AMPLIFY-OS");
    power_shutdown();
}

void power_reboot_with_curtain(void) {
    run_curtain("Перезагрузка AMPLIFY-OS");
    power_reboot();
}
