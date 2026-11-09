// ============================================================================
// ioapic.c — I/O APIC: discovery + минимальный init + маршрутизация IRQ.
// ============================================================================
// IOAPIC регистры — это window-схема:
//   *(base + 0x00) = регистр выбора (IOREGSEL)
//   *(base + 0x10) = окно данных (IOWIN)
// Чтобы прочитать reg N: сначала пишем N в IOREGSEL, потом читаем IOWIN.
// ============================================================================

#include "ioapic.h"
#include "../../vga.h"

uint32_t ioapic_base     = 0;
uint8_t  ioapic_id       = 0;
uint32_t ioapic_gsi_base = 0;
int      ioapic_max_redir = 0;

bool ioapic_present(void) { return ioapic_base != 0; }

uint32_t ioapic_read(uint8_t reg) {
    if (!ioapic_base) return 0;
    *((volatile uint32_t*)(ioapic_base + 0x00)) = reg;
    return *((volatile uint32_t*)(ioapic_base + 0x10));
}

void ioapic_write(uint8_t reg, uint32_t value) {
    if (!ioapic_base) return;
    *((volatile uint32_t*)(ioapic_base + 0x00)) = reg;
    *((volatile uint32_t*)(ioapic_base + 0x10)) = value;
}

void ioapic_init(void) {
    if (!ioapic_base) {
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        vga_println("[IOAPIC] not present (PIC-only routing)");
        return;
    }

    // Прочитаем версию: верхние 8 бит [23:16] — Maximum Redirection Entry.
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;

    // Маскируем все redirection entries — IRQ'и продолжают идти через PIC.
    for (int i = 0; i < ioapic_max_redir; i++) {
        ioapic_write(IOAPIC_REG_REDTBL + 2 * i,     0x00010000);
        ioapic_write(IOAPIC_REG_REDTBL + 2 * i + 1, 0x00000000);
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[IOAPIC] base=");
    vga_print_hex(ioapic_base);
    vga_puts(" id=");
    vga_print_dec(ioapic_id);
    vga_puts(" gsi_base=");
    vga_print_dec(ioapic_gsi_base);
    vga_puts(" entries=");
    vga_print_dec(ioapic_max_redir);
    vga_println("");
}

// ----------------------------------------------------------------------------
// Маршрутизация IRQ. Формат RTE (64 бита, две 32-битные ячейки):
//   low [7:0]    vector
//   low [10:8]   delivery mode (000 = fixed)
//   low [11]     destination mode (0 = physical)
//   low [13]     polarity (0 = active high)
//   low [15]     trigger mode (0 = edge)
//   low [16]     mask (1 = masked)
//   high [31:24] destination LAPIC id (physical mode)
// ----------------------------------------------------------------------------
void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id, bool masked) {
    if (!ioapic_base) return;
    if ((int)gsi >= ioapic_max_redir) return;

    uint32_t low = (uint32_t)vector;
    if (masked) low |= (1u << 16);
    uint32_t high = ((uint32_t)lapic_id) << 24;

    // Сначала high (destination), потом low: пока mask=1, доставка не пойдёт.
    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi + 1, high);
    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi,     low);
}

void ioapic_set_mask(uint8_t gsi, bool masked) {
    if (!ioapic_base) return;
    if ((int)gsi >= ioapic_max_redir) return;
    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL + 2 * gsi);
    if (masked) low |=  (1u << 16);
    else        low &= ~(1u << 16);
    ioapic_write(IOAPIC_REG_REDTBL + 2 * gsi, low);
}
