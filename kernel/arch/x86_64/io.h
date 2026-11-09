// ============================================================================
// io.h — port I/O в 64-битном режиме.
// ----------------------------------------------------------------------------
// В 64-bit ABI inb/outb используют те же 16-bit порты и 8-bit данные, что и
// в 32-bit. Различий на уровне инструкций нет — синтаксис asm идентичен.
// ============================================================================
#ifndef AOS_X86_64_IO_H
#define AOS_X86_64_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// Маленькая задержка, нужная при перемапе PIC и работе с RTC.
static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) { __asm__ __volatile__("cli"); }
static inline void sti(void) { __asm__ __volatile__("sti"); }
static inline void hlt(void) { __asm__ __volatile__("hlt"); }

#endif
