#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>

void smp_init(void);
extern volatile uint32_t smp_cpus_started;

#endif
