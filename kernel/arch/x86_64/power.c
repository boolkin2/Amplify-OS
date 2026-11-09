// ============================================================================
// power.c — reboot / shutdown в 64-битном режиме.
// ============================================================================
// Логика идентична kernel/arch/i386/power.c, но curtain — текстовая (graphics
// curtain с логотипом появится после переноса doublebuf в 37.6c-2).
// ============================================================================

#include "power.h"
#include "io.h"
#include "fb.h"
#include "timer.h"
#include "serial.h"

#include <stdint.h>

void power_reboot(void) {
    serial_puts("[power] reboot via 8042\n");
    // Ждём, пока буфер ввода 8042 освободится.
    uint8_t status = 0x02;
    int spin = 100000;
    while ((status & 0x02) && spin-- > 0) {
        status = inb(0x64);
    }
    // Команда 0xFE — pulse reset line CPU.
    outb(0x64, 0xFE);
    // Если порт не сработал — бесконечный hlt.
    for (;;) __asm__ __volatile__("cli; hlt");
}

void power_shutdown(void) {
    serial_puts("[power] shutdown via ACPI\n");
    // QEMU: ACPI soft-off через порт 0x604 (newer) и 0xB004 (legacy/SeaBIOS).
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    // VirtualBox:
    outw(0x4004, 0x3400);
    // Не помогло — замораживаем CPU.
    for (;;) __asm__ __volatile__("cli; hlt");
}

// ----------------------------------------------------------------------------
// Текстовая «шторка»: рисуем полноэкранный gradient через fb_clear + надпись
// по центру через fb_draw_string. Работает без doublebuf и WM.
// ----------------------------------------------------------------------------
static void draw_text_curtain(const char* main_text) {
    const aos64_fb_t* st = fb_state();
    if (!st->ok) {
        // Fallback в serial: пользователь хотя бы увидит сообщение в логе.
        serial_puts("[power] ");
        serial_puts(main_text);
        serial_puts("\n");
        return;
    }

    // Тёмно-синий фон.
    fb_clear(0xFF0A1428);

    int W = (int)st->width;
    int H = (int)st->height;
    int len = 0;
    while (main_text[len]) len++;

    // Главная надпись: 8×16 шрифт, центрируем.
    int tw = len * 8;
    int tx = (W - tw) / 2;
    int ty = H / 2 - 24;
    fb_draw_string(tx, ty, main_text, 0xFFE8EAF2, 0xFF0A1428);

    // Бренд снизу.
    const char* brand = "AMPLIFY-OS";
    int bw = 10 * 8;
    int bx = (W - bw) / 2;
    int by = H / 2 + 8;
    fb_draw_string(bx, by, brand, 0xFFFFE0A0, 0xFF0A1428);

    // Подсказка.
    const char* tip = "Please do not power off until done";
    int twh = 0; while (tip[twh]) twh++;
    fb_draw_string((W - twh * 8) / 2, H - 40, tip, 0xFF55668A, 0xFF0A1428);
}

static void run_curtain(const char* main_text) {
    draw_text_curtain(main_text);
    // ~0.8 секунды видимости.
    timer_sleep(800);
}

void power_shutdown_with_curtain(void) {
    run_curtain("Shutting down AMPLIFY-OS...");
    power_shutdown();
}

void power_reboot_with_curtain(void) {
    run_curtain("Rebooting AMPLIFY-OS...");
    power_reboot();
}
