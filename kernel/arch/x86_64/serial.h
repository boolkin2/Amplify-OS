// ============================================================================
// serial.h — простой драйвер COM1 для ранней отладки 64-битного ядра.
// ============================================================================
#ifndef AOS_X86_64_SERIAL_H
#define AOS_X86_64_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_print_hex(uint64_t v, int digits);
void serial_print_dec(uint64_t v);

// [этап 49] Журнал ядра: последние 64 КБ всего, что ушло в serial.
// klog_read_tail копирует до max-1 последних символов в out (+ NUL),
// возвращает их число. klog_total — сколько символов записано всего.
uint64_t klog_read_tail(char* out, uint64_t max);
uint64_t klog_total(void);

#endif
