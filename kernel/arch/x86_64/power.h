// ============================================================================
// power.h — управление питанием (reboot / shutdown) для x86_64-ядра.
// ----------------------------------------------------------------------------
// Единый API для shell (cycle/freeze) и GUI (меню Пуск).  Текстовая «шторка»
// с таймером — простая реализация поверх fb_console (полноценный графический
// curtain с логотипом появится в подэтапе 37.6c-2 вместе с doublebuf).
// ============================================================================
#ifndef AOS_X86_64_POWER_H
#define AOS_X86_64_POWER_H

// Аппаратный reset через 8042 keyboard controller (порт 0x64, команда 0xFE).
void power_reboot(void) __attribute__((noreturn));

// QEMU ACPI soft-off (0x604/0xB004) или VirtualBox (0x4004).
// Если ничего не сработало → cli; hlt навсегда.
void power_shutdown(void) __attribute__((noreturn));

// Текстовая «шторка»: рисует полноэкранное сообщение через fb_console,
// удерживает ~0.8 сек, потом вызывает реальный power_*.
void power_shutdown_with_curtain(void) __attribute__((noreturn));
void power_reboot_with_curtain(void)   __attribute__((noreturn));

#endif
