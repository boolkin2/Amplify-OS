#include "cpu_stats.h"

cpu_stats_t cpu_stats[MAX_CPUS];

void cpu_stats_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_stats[i].lapic_id   = 0;
        cpu_stats[i].online     = false;
        cpu_stats[i].busy_ticks = 0;
        cpu_stats[i].idle_ticks = 0;
    }
}
