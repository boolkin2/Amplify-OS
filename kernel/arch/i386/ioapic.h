// ============================================================================
// ioapic.h — I/O APIC: обнаружение, базовое чтение/запись, маршрутизация IRQ.
// ----------------------------------------------------------------------------
// MyOS по умолчанию остаётся на классическом 8259-PIC: он стабильно работает,
// и переключение всех IRQ на IOAPIC — риск без выгоды. Однако обнаружение
// IOAPIC даёт SMP-картинку «у нас есть полноценный APIC-стек», а готовый
// API маршрутизации пригодится для IRQ migration и MSI на будущих этапах.
// ============================================================================
#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL   0x10  // 24 пары по 32 бит каждая

extern uint32_t ioapic_base;      // 0 если IOAPIC не найден
extern uint8_t  ioapic_id;
extern uint32_t ioapic_gsi_base;
extern int      ioapic_max_redir; // (IOAPIC_VER >> 16) & 0xFF

bool     ioapic_present(void);
void     ioapic_init(void);
uint32_t ioapic_read(uint8_t reg);
void     ioapic_write(uint8_t reg, uint32_t value);

// ----------------------------------------------------------------------------
// Маршрутизация IRQ через IOAPIC.
// Использовать осторожно: если включить вход IOAPIC и не замаскировать
// тот же IRQ на PIC, получишь двойную доставку прерывания.
//
//   ioapic_route_irq(gsi, vector, lapic_id, masked)
//       Записывает RTE для GSI=gsi: edge-triggered, fixed delivery, physical
//       destination, активный=high. masked=true сразу маскирует.
//
//   ioapic_set_mask(gsi, masked)
//       Меняет только бит mask, не трогая остальное в RTE.
// ----------------------------------------------------------------------------
void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id, bool masked);
void ioapic_set_mask(uint8_t gsi, bool masked);

#endif // IOAPIC_H
