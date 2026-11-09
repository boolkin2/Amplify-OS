// ============================================================================
// cpu_stats.h — per-CPU счётчики активности для визуализации SMP-нагрузки.
// ----------------------------------------------------------------------------
// Каждое ядро (BSP и AP'ы) ведёт два счётчика:
//   busy_ticks  — увеличивается, когда ядро в "работе"
//   idle_ticks  — увеличивается, когда ядро в pause
// «Embers» (наш htop) делает два снимка с интервалом ~200 мс и считает
// процент загрузки = busy_delta / (busy_delta + idle_delta).
// ============================================================================
#ifndef CPU_STATS_H
#define CPU_STATS_H

#include <stdint.h>
#include <stdbool.h>
#include "acpi.h"   // MAX_CPUS

typedef struct {
    volatile uint32_t lapic_id;
    volatile uint32_t busy_ticks;
    volatile uint32_t idle_ticks;
    volatile bool     online;       // ядро отрапортовало о готовности
} cpu_stat_t;

extern cpu_stat_t cpu_stats[MAX_CPUS];

// Инициализация (вызывается из BSP перед smp_init).
void cpu_stats_init(void);

// Найти индекс в cpu_stats[] для текущего LAPIC-ID. -1 если не найдено.
int cpu_stats_index_of(uint32_t lapic_id);

// Вызывается BSP'ом из таймерного IRQ — отметить «занят».
void cpu_stats_mark_busy_bsp(void);

#endif // CPU_STATS_H
