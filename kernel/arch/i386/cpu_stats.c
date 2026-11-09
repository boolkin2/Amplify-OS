// ============================================================================
// cpu_stats.c — реализация per-CPU счётчиков.
// ============================================================================

#include "cpu_stats.h"
#include "lapic.h"

cpu_stat_t cpu_stats[MAX_CPUS];

void cpu_stats_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_stats[i].lapic_id   = (uint32_t)-1;
        cpu_stats[i].busy_ticks = 0;
        cpu_stats[i].idle_ticks = 0;
        cpu_stats[i].online     = false;
    }
    // BSP-слот заполняется первым; реальный LAPIC-ID назначим в smp_init,
    // но slot 0 резервируем под BSP всегда.
    cpu_stats[0].lapic_id = lapic_get_id();
    cpu_stats[0].online   = true;
}

int cpu_stats_index_of(uint32_t lapic_id) {
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpu_stats[i].online && cpu_stats[i].lapic_id == lapic_id) return i;
    }
    return -1;
}

void cpu_stats_mark_busy_bsp(void) {
    cpu_stats[0].busy_ticks++;
}
