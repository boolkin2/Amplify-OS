// ============================================================================
// ioapic.h — IOAPIC discovery + минимальная маршрутизация.
// ----------------------------------------------------------------------------
// AMPLIFY-OS по умолчанию остаётся на 8259 PIC: переключение всех IRQ на
// IOAPIC рискованно без полной валидации каждого источника. Однако discovery
// нужен для красивой статистики и для подготовки к этапу 53 (BT/WiFi),
// где MSI-X неизбежен.
// ============================================================================
#ifndef AOS_X86_64_IOAPIC_H
#define AOS_X86_64_IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL   0x10

extern int ioapic_max_redir;

bool     ioapic_present(void);
void     ioapic_init(void);
uint32_t ioapic_read(uint8_t reg);
void     ioapic_write(uint8_t reg, uint32_t value);
void     ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id, bool masked);
void     ioapic_set_mask(uint8_t gsi, bool masked);

#endif
