// ============================================================================
// ioapic.c — IOAPIC чтение/запись, маршрутизация IRQ.
// Логика 1:1 как kernel/arch/i386/ioapic.c, base — uint64_t.
// ============================================================================
#include "ioapic.h"
#include "acpi.h"
#include "serial.h"

int ioapic_max_redir = 0;

bool ioapic_present(void) { return acpi_ioapic_phys != 0; }

uint32_t ioapic_read(uint8_t reg) {
    if (!acpi_ioapic_phys) return 0;
    *(volatile uint32_t*)(uintptr_t)(acpi_ioapic_phys + 0x00) = reg;
    return *(volatile uint32_t*)(uintptr_t)(acpi_ioapic_phys + 0x10);
}

void ioapic_write(uint8_t reg, uint32_t value) {
    if (!acpi_ioapic_phys) return;
    *(volatile uint32_t*)(uintptr_t)(acpi_ioapic_phys + 0x00) = reg;
    *(volatile uint32_t*)(uintptr_t)(acpi_ioapic_phys + 0x10) = value;
}

void ioapic_init(void) {
    if (!acpi_ioapic_phys) {
        serial_puts("[ioapic] not present (PIC-only routing)\n");
        return;
    }

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;

    // Маскируем все redirection entries — IRQ продолжают идти через PIC.
    for (int i = 0; i < ioapic_max_redir; i++) {
        ioapic_write(IOAPIC_REG_REDTBL + 2 * i,     0x00010000);  // mask=1
        ioapic_write(IOAPIC_REG_REDTBL + 2 * i + 1, 0x00000000);
    }

    serial_puts("[ioapic] base=0x");
    serial_print_hex(acpi_ioapic_phys, 16);
    serial_puts(" id=");        serial_print_dec(acpi_ioapic_id);
    serial_puts(" gsi_base=");  serial_print_dec(acpi_ioapic_gsi_base);
    serial_puts(" entries=");   serial_print_dec(ioapic_max_redir);
    serial_puts("\n");
}

void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id, bool masked) {
    if (!acpi_ioapic_phys) return;
    if ((int)gsi >= ioapic_max_redir) return;

    uint32_t low  = (uint32_t)vector;
    if (masked) low |= (1u << 16);
    uint32_t high = ((uint32_t)lapic_id) << 24;

    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi + 1, high);
    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi,     low);
}

void ioapic_set_mask(uint8_t gsi, bool masked) {
    if (!acpi_ioapic_phys) return;
    if ((int)gsi >= ioapic_max_redir) return;
    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL + 2 * gsi);
    if (masked) low |=  (1u << 16);
    else        low &= ~(1u << 16);
    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi, low);
}
