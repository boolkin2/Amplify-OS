#include "acpi.h"
#include "ioapic.h"
#include "../../vga.h"
#include <stddef.h>
#include <stdint.h>

uint32_t bsp_lapic_id = 0;
uint32_t cpu_lapic_ids[MAX_CPUS];
int num_cpus = 0;
uint32_t local_apic_base = 0xFEE00000; // По умолчанию

static void* find_rsdp_in_range(uint32_t start, uint32_t length) {
    for (uint32_t p = start; p < start + length; p += 16) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)p;
        if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
            rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
            rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' &&
            rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ') {
            
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum += ((uint8_t*)p)[i];
            if (sum == 0) return rsdp;
        }
    }
    return NULL;
}

// Хелпер: прочитать 16-битное значение по фиксированному низкому адресу.
// Используем inline asm чтобы GCC на -O2 не ругался на "array bounds"
// при разыменовании адресов < 64K (BIOS Data Area).
static inline uint16_t read_word_at(uint32_t addr) {
    uint16_t v;
    __asm__ volatile ("movw (%1), %0" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static acpi_rsdp_t* find_rsdp(void) {
    // В MyOS используем identity mapping, так что физические адреса == виртуальные для нижних 16 МБ
    uint16_t ebda_seg = read_word_at(0x40E);
    void* rsdp = NULL;
    
    if (ebda_seg) {
        rsdp = find_rsdp_in_range(ebda_seg << 4, 1024);
    }
    if (!rsdp) {
        rsdp = find_rsdp_in_range(0x000E0000, 0x0001FFFF);
    }
    return (acpi_rsdp_t*)rsdp;
}

static void parse_madt(acpi_madt_t* madt) {
    local_apic_base = madt->local_apic_addr;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("[ACPI] LAPIC Base: ");
    vga_print_hex(local_apic_base);
    vga_println("");

    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];
        
        if (type == 0) { // Processor Local APIC
            uint8_t apic_id = ptr[3];
            uint32_t flags = *((uint32_t*)&ptr[4]);

            if (flags & 1) { // Установлен бит Enabled
                if (num_cpus < MAX_CPUS) {
                    cpu_lapic_ids[num_cpus] = apic_id;
                    num_cpus++;
                }
            }
        } else if (type == 1) { // I/O APIC
            // Структура: type(1) length(1) ioapic_id(1) reserved(1)
            //            ioapic_addr(4) gsi_base(4)
            if (ioapic_base == 0) {            // берём только первый
                ioapic_id       = ptr[2];
                ioapic_base     = *((uint32_t*)&ptr[4]);
                ioapic_gsi_base = *((uint32_t*)&ptr[8]);
            }
        }
        ptr += length;
    }
    
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[ACPI] Najdeno procesorov: ");
    vga_print_dec(num_cpus);
    vga_println("");
}

// Верхняя граница identity mapping ядра (см. paging.c, KERNEL_ID_MAP_TABLES * 4 MB).
// Используется для отбраковки ACPI-указателей, ведущих в неотображённую память.
#define ACPI_ID_MAP_LIMIT   0x04000000  // 64 MB

void acpi_init(void) {
    acpi_rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ACPI] RSDP ne najden");
        return;
    }

    // Защита: rsdt_address может быть вне identity mapping
    if (rsdp->rsdt_address == 0 || rsdp->rsdt_address >= ACPI_ID_MAP_LIMIT) {
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        vga_println("[ACPI] RSDT vne identity map — propuskayem");
        return;
    }

    acpi_rsdt_t* rsdt = (acpi_rsdt_t*)rsdp->rsdt_address;
    int entries = (rsdt->header.length - sizeof(acpi_header_t)) / 4;

    for (int i = 0; i < entries; i++) {
        uint32_t h_addr = rsdt->pointer_to_other_sdt[i];
        if (h_addr == 0 || h_addr >= ACPI_ID_MAP_LIMIT) continue;
        acpi_header_t* h = (acpi_header_t*)h_addr;
        if (h->signature[0] == 'A' && h->signature[1] == 'P' &&
            h->signature[2] == 'I' && h->signature[3] == 'C') {
            parse_madt((acpi_madt_t*)h);
            return;
        }
    }
}
