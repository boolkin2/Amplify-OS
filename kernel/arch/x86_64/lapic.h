// ============================================================================
// lapic.h — Local APIC для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// MMIO base — acpi_lapic_phys (по умолчанию 0xFEE00000), identity-mapped.
// На каждом ядре lapic_init() вызывается отдельно.
// ============================================================================
#ifndef AOS_X86_64_LAPIC_H
#define AOS_X86_64_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

// Регистры LAPIC (offset от base).
#define LAPIC_ID            0x0020
#define LAPIC_VERSION       0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_EOI           0x00B0
#define LAPIC_LDR           0x00D0
#define LAPIC_DFR           0x00E0
#define LAPIC_SVR           0x00F0
#define LAPIC_ESR           0x0280
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TMR       0x0320
#define LAPIC_LVT_THERM     0x0330
#define LAPIC_LVT_PERF      0x0340
#define LAPIC_LVT_LINT0     0x0350
#define LAPIC_LVT_LINT1     0x0360
#define LAPIC_LVT_ERR       0x0370
#define LAPIC_TMRINITCNT    0x0380
#define LAPIC_TMRCURRCNT    0x0390
#define LAPIC_TMRDIV        0x03E0

// IPI delivery modes.
#define LAPIC_DELIV_FIXED   0
#define LAPIC_DELIV_NMI     4
#define LAPIC_DELIV_INIT    5
#define LAPIC_DELIV_SIPI    6

void     lapic_init(void);

// [41.2] Запустить таймер LAPIC на 1000 Гц и передать ему время от PIT.
// Звать на BSP, когда прерывания разрешены и PIT тикает. false — остались на PIT.
bool lapic_timer_start(void);
void     lapic_eoi(void);
uint32_t lapic_get_id(void);
void     lapic_send_ipi(uint32_t lapic_id, uint8_t vector,
                        uint8_t delivery_mode, uint8_t level, uint8_t trigger);

uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t value);

#endif
