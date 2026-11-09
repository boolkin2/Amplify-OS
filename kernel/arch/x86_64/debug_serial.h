// ============================================================================
// debug_serial.h — alias на serial.h для совместимости с общими модулями
// ----------------------------------------------------------------------------
// Несколько подсистем (kernel/net/http.c и др.) используют макрос dbg_puts.
// В x86_64-ветке его роль выполняет serial_puts. Этот заголовок — мост.
// ============================================================================
#ifndef AOS_X86_64_DEBUG_SERIAL_H
#define AOS_X86_64_DEBUG_SERIAL_H

#include "serial.h"

#define dbg_puts(s)       serial_puts(s)
#define dbg_putc(c)       serial_putc(c)
#define dbg_print_hex(v)  serial_print_hex((uint64_t)(v), 16)
#define dbg_print_dec(v)  serial_print_dec((uint64_t)(v))

#endif
