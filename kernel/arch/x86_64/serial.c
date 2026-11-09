// ============================================================================
// serial.c — COM1 (0x3F8) на 38400 8N1 для дампа логов из QEMU `-serial stdio`.
// ============================================================================
#include "serial.h"
#include "io.h"

#define COM1 0x3F8

// ----------------------------------------------------------------------------
// [этап 49] Журнал ядра в памяти (klog).
// Всё, что уходит в COM1, дублируется в кольцевой буфер 64 КБ. До этапа 49
// логи жили только на хосте (QEMU -serial), и сама ОС не могла их прочитать —
// а AMPLIFY MIND должен уметь «посмотреть журнал». Индекс head растёт
// атомарно (fetch_add), поэтому писать можно из IRQ и с любого ядра без
// блокировок: в худшем случае одновременные записи перемешают символы, но
// не повредят память и не приведут к взаимоблокировке (спинлок в serial_putc
// из обработчика прерывания повесил бы ядро, державшее его).
// ----------------------------------------------------------------------------
#define KLOG_SIZE  65536u                    // степень двойки: маска вместо %
static char              g_klog[KLOG_SIZE];
static volatile uint64_t g_klog_head;        // всего записано символов

static inline void klog_putc(char c) {
    uint64_t i = __atomic_fetch_add(&g_klog_head, 1, __ATOMIC_RELAXED);
    g_klog[i & (KLOG_SIZE - 1)] = c;
}

uint64_t klog_total(void) { return g_klog_head; }

uint64_t klog_read_tail(char* out, uint64_t max) {
    if (!out || max == 0) return 0;
    uint64_t head = __atomic_load_n(&g_klog_head, __ATOMIC_ACQUIRE);
    uint64_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    uint64_t n = avail < max - 1 ? avail : max - 1;
    uint64_t start = head - n;
    for (uint64_t k = 0; k < n; k++) out[k] = g_klog[(start + k) & (KLOG_SIZE - 1)];
    out[n] = 0;
    return n;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);   // выключаем прерывания
    outb(COM1 + 3, 0x80);   // DLAB = 1
    outb(COM1 + 0, 0x03);   // делитель = 3 → 38400 baud (low)
    outb(COM1 + 1, 0x00);   // делитель high
    outb(COM1 + 3, 0x03);   // 8 bit, no parity, 1 stop, DLAB = 0
    outb(COM1 + 2, 0xC7);   // FIFO enable, очистить, порог 14 байт
    outb(COM1 + 4, 0x0B);   // IRQs enabled, RTS/DSR set
}

void serial_putc(char c) {
    if (c != '\r') klog_putc(c);
    while ((inb(COM1 + 5) & 0x20) == 0) { /* ждём пустого THR */ }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_print_hex(uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    // Префикс "0x" пишет вызывающий, чтобы не было "0x0x..." при двойном вызове.
    for (int i = digits - 1; i >= 0; i--) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

void serial_print_dec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}
