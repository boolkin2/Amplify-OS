// ============================================================================
// io.h - Функции для работы с портами ввода-вывода (I/O ports)
// ============================================================================
// Порты I/O используются для взаимодействия с аппаратными устройствами
// Инструкции IN/OUT работают с портами (0x0000 - 0xFFFF)
// ============================================================================

#ifndef IO_H
#define IO_H

#include <stdint.h>

// ============================================================================
// Функции для работы с портами
// ============================================================================

// Чтение байта из порта
// Параметры:
//   port - номер порта (0-65535)
// Возвращает:
//   Прочитанный байт
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Запись байта в порт
// Параметры:
//   port - номер порта (0-65535)
//   data - байт для записи
static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

// Чтение слова (2 байта) из порта
// Параметры:
//   port - номер порта (0-65535)
// Возвращает:
//   Прочитанное слово (16 бит)
static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Запись слова (2 байта) в порт
// Параметры:
//   port - номер порта (0-65535)
//   data - слово для записи
static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile ("outw %0, %1" : : "a"(data), "Nd"(port));
}

// Чтение двойного слова (4 байта) из порта
// Параметры:
//   port - номер порта (0-65535)
// Возвращает:
//   Прочитанное двойное слово (32 бита)
static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Запись двойного слова (4 байта) в порт
// Параметры:
//   port - номер порта (0-65535)
//   data - двойное слово для записи
static inline void outl(uint16_t port, uint32_t data) {
    __asm__ volatile ("outl %0, %1" : : "a"(data), "Nd"(port));
}

// Задержка на один I/O цикл (для старых устройств)
// Используется для синхронизации с медленными устройствами
// Обычно записывается в неиспользуемый порт 0x80
static inline void io_wait(void) {
    outb(0x80, 0);
}

// ============================================================================
// Примеры использования портов
// ============================================================================

// Порты PIC (Programmable Interrupt Controller)
#define PIC1_COMMAND    0x20        // Командный порт главного PIC
#define PIC1_DATA       0x21        // Порт данных главного PIC
#define PIC2_COMMAND    0xA0        // Командный порт подчинённого PIC
#define PIC2_DATA       0xA1        // Порт данных подчинённого PIC

// Порты PIT (Programmable Interval Timer)
#define PIT_CHANNEL0    0x40        // Канал 0 (системный таймер)
#define PIT_CHANNEL1    0x41        // Канал 1 (не используется)
#define PIT_CHANNEL2    0x42        // Канал 2 (PC Speaker)
#define PIT_COMMAND     0x43        // Командный порт

// Порты клавиатуры (PS/2)
#define KEYBOARD_DATA   0x60        // Порт данных клавиатуры
#define KEYBOARD_STATUS 0x64        // Порт статуса клавиатуры
#define KEYBOARD_COMMAND 0x64       // Командный порт клавиатуры

// Порты последовательного порта (COM1)
#define COM1_PORT       0x3F8       // COM1 базовый адрес

// Порты VGA
#define VGA_MISC_WRITE  0x3C2       // Miscellaneous Output
#define VGA_CRTC_INDEX  0x3D4       // CRT Controller Index
#define VGA_CRTC_DATA   0x3D5       // CRT Controller Data

#endif // IO_H
