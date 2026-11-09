// ============================================================================
// cpu_stats.h — per-CPU счётчики (busy/idle ticks, online/offline, lapic_id).
// Та же модель что в kernel/arch/i386/cpu_stats.h, но 64-битные счётчики.
// ============================================================================
#ifndef AOS_X86_64_CPU_STATS_H
#define AOS_X86_64_CPU_STATS_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_CPUS  16

typedef struct {
    uint32_t  lapic_id;
    bool      online;
    uint64_t  busy_ticks;
    uint64_t  idle_ticks;
} cpu_stats_t;

extern cpu_stats_t cpu_stats[MAX_CPUS];

void cpu_stats_init(void);

#endif
