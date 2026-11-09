// ============================================================================
// timer.h — высокоуровневый таймер-адаптер для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Сидит поверх pit_ticks() (10 мс на тик при PIT @100 Hz). Предоставляет
// минимально совместимый с kernel/drivers/timer.h API, чтобы перенесённые
// общие модули (audio, http, shell-команды) собирались без правок.
//
//   timer_get_uptime_ms()  — миллисекунды с момента pit_init().
//   timer_sleep(ms)        — busy-wait через sti+hlt.
//   timer_register_callback(cb, ms_period) — таймерный callback (для tasks).
//
// На 37.6c-1 callback'и хранятся в небольшой таблице, обновляются ВНУТРИ
// pit_isr через timer_tick() (вызывается из main pit_isr).
// ============================================================================
#ifndef AOS_X86_64_TIMER_H
#define AOS_X86_64_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#define TIMER_MAX_CALLBACKS 16

typedef void (*timer_callback_fn)(void);

void     timer_init(void);

// Время с момента pit_init() в миллисекундах (10 мс на тик при 100 Hz).
uint64_t timer_get_uptime_ms(void);

// Короткое время с момента pit_init() в тиках (1 тик = 10 мс).
uint64_t timer_get_ticks(void);

// Блокирующая задержка через sti+hlt (НЕ используйте до sti!).
void     timer_sleep(uint32_t ms);

// Зарегистрировать периодический callback. period_ms округляется вверх до
// числа PIT-тиков (10 мс шаг). Возвращает id (0..N-1) или -1 при переполнении.
int      timer_register_callback(timer_callback_fn cb, uint32_t period_ms);

// Вызывается из pit_isr() — продвигает счётчики и дёргает callback'и.
// Объявлено публично, чтобы pit.c мог его вызвать.
void     timer_tick(void);

// [41.2] Источник времени с разрешением 1 мс (таймер LAPIC, lapic.c).
void     timer_ms_source_start(void);
void     timer_ms_tick(void);
bool     timer_ms_active(void);

#endif
