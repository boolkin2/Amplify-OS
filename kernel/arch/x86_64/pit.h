// ============================================================================
// pit.h — Programmable Interval Timer 8253/8254 на ~100 Hz.
// ============================================================================
#ifndef AOS_X86_64_PIT_H
#define AOS_X86_64_PIT_H

#include <stdint.h>

void     pit_init(uint32_t hz);
uint64_t pit_ticks(void);

// [41.2] Передача тика таймеру LAPIC (см. pit.c, lapic_timer_start).
void pit_handover_to_lapic(void);
void pit_tick_from_lapic(void);          // тики с момента pit_init

#endif
