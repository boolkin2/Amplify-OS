#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

void lapic_init(void);
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_eoi(void);
uint32_t lapic_get_id(void);
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger);

// Регистры LAPIC
#define LAPIC_ID            0x0020
#define LAPIC_VERSION       0x0030
#define LAPIC_EOI           0x00B0
#define LAPIC_SVR           0x00F0
#define LAPIC_ESR           0x0280
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TMR       0x0320
#define LAPIC_LVT_PERF      0x0340
#define LAPIC_LVT_LINT0     0x0350
#define LAPIC_LVT_LINT1     0x0360
#define LAPIC_LVT_ERR       0x0370
#define LAPIC_TMRINITCNT    0x0380
#define LAPIC_TMRCURRCNT    0x0390
#define LAPIC_TMRDIV        0x03E0

#endif
