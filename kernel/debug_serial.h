// ============================================================================
// debug_serial.h - Мини-драйвер COM1 для отладочных сообщений (Этап 21)
// ============================================================================
// В ядре нет console.log(). Используем COM1 (порт 0x3F8) для вывода.
// Запускать QEMU с `-serial stdio` чтобы увидеть вывод в терминале хоста.
// ============================================================================

#ifndef DEBUG_SERIAL_H
#define DEBUG_SERIAL_H

#include <stdint.h>
#include "io.h"

static inline void dbg_init(void) {
    // COM1: 115200 baud, 8N1
    outb(0x3F8 + 1, 0x00);    // выкл. прерывания
    outb(0x3F8 + 3, 0x80);    // enable DLAB
    outb(0x3F8 + 0, 0x01);    // divisor low  = 115200
    outb(0x3F8 + 1, 0x00);    // divisor high
    outb(0x3F8 + 3, 0x03);    // 8N1, отключ. DLAB
    outb(0x3F8 + 2, 0xC7);    // FIFO
    outb(0x3F8 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static inline void dbg_putc(char c) {
    // ждём пока TX буфер освободится
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
}

static inline void dbg_puts(const char* s) {
    while (*s) {
        if (*s == '\n') dbg_putc('\r');
        dbg_putc(*s++);
    }
}

static inline void dbg_hex(uint32_t v) {
    static const char* digits = "0123456789ABCDEF";
    dbg_putc('0'); dbg_putc('x');
    for (int i = 28; i >= 0; i -= 4) {
        dbg_putc(digits[(v >> i) & 0xF]);
    }
}

#endif // DEBUG_SERIAL_H
