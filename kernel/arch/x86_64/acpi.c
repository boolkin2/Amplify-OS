// ============================================================================
// acpi.c — парсер RSDP→XSDT/RSDT→MADT.
// ============================================================================
#include "acpi.h"
#include "serial.h"

uint64_t  acpi_lapic_phys     = 0xFEE00000ULL;
uint64_t  acpi_ioapic_phys    = 0;
uint8_t   acpi_ioapic_id      = 0;
uint32_t  acpi_ioapic_gsi_base = 0;
uint32_t  acpi_lapic_ids[MAX_CPUS];
int       acpi_num_cpus       = 0;
uint32_t  acpi_bsp_lapic_id   = 0;
uint8_t   acpi_irq_to_gsi[16];

// ----------------------------------------------------------------------------
// Парсинг MADT
// ----------------------------------------------------------------------------
static void parse_madt(const acpi_madt_t* m) {
    acpi_lapic_phys = (uint64_t)m->local_apic_addr;

    serial_puts("[ACPI] MADT: LAPIC base = 0x");
    serial_print_hex(acpi_lapic_phys, 16);
    serial_puts("\n");

    const uint8_t* p   = (const uint8_t*)m + sizeof(acpi_madt_t);
    const uint8_t* end = (const uint8_t*)m + m->header.length;

    while (p < end) {
        uint8_t type   = p[0];
        uint8_t length = p[1];
        if (length == 0) break;

        switch (type) {
        case MADT_LAPIC: {
            // type(1) length(1) acpi_proc_id(1) apic_id(1) flags(4)
            uint8_t  apic_id = p[3];
            uint32_t flags   = *(const uint32_t*)&p[4];
            if ((flags & 1) && acpi_num_cpus < MAX_CPUS) {
                acpi_lapic_ids[acpi_num_cpus++] = apic_id;
            }
            break;
        }
        case MADT_IOAPIC: {
            // type(1) length(1) ioapic_id(1) reserved(1)
            // ioapic_addr(4) gsi_base(4)
            if (acpi_ioapic_phys == 0) {
                acpi_ioapic_id        = p[2];
                acpi_ioapic_phys      = *(const uint32_t*)&p[4];
                acpi_ioapic_gsi_base  = *(const uint32_t*)&p[8];
            }
            break;
        }
        case MADT_INT_OVERRIDE: {
            // type(1) length(1) bus(1) source(1) gsi(4) flags(2)
            uint8_t source = p[3];
            uint32_t gsi   = *(const uint32_t*)&p[4];
            if (source < 16) acpi_irq_to_gsi[source] = (uint8_t)gsi;
            break;
        }
        case MADT_LAPIC_OVERRIDE: {
            // type(1) length(1) reserved(2) lapic_addr(8)
            acpi_lapic_phys = *(const uint64_t*)&p[4];
            break;
        }
        case MADT_X2APIC: {
            // type(1) length(1) reserved(2) x2apic_id(4) flags(4) acpi_uid(4)
            uint32_t apic_id = *(const uint32_t*)&p[4];
            uint32_t flags   = *(const uint32_t*)&p[8];
            if ((flags & 1) && acpi_num_cpus < MAX_CPUS) {
                acpi_lapic_ids[acpi_num_cpus++] = apic_id;
            }
            break;
        }
        default: break;
        }
        p += length;
    }

    serial_puts("[ACPI] CPUs found: ");
    serial_print_dec(acpi_num_cpus);
    serial_puts("\n");
}

// ----------------------------------------------------------------------------
// Walk RSDT (32-bit pointers) или XSDT (64-bit pointers), ищем "APIC".
// ----------------------------------------------------------------------------
static void walk_rsdt(uint64_t rsdt_phys) {
    const acpi_sdt_header_t* h = (const acpi_sdt_header_t*)(uintptr_t)rsdt_phys;
    int n = (h->length - sizeof(*h)) / 4;
    const uint32_t* arr = (const uint32_t*)((const uint8_t*)h + sizeof(*h));

    for (int i = 0; i < n; i++) {
        const acpi_sdt_header_t* t = (const acpi_sdt_header_t*)(uintptr_t)arr[i];
        if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
            t->signature[2] == 'I' && t->signature[3] == 'C') {
            parse_madt((const acpi_madt_t*)t);
        }
    }
}

static void walk_xsdt(uint64_t xsdt_phys) {
    const acpi_sdt_header_t* h = (const acpi_sdt_header_t*)(uintptr_t)xsdt_phys;
    int n = (h->length - sizeof(*h)) / 8;
    const uint64_t* arr = (const uint64_t*)((const uint8_t*)h + sizeof(*h));

    for (int i = 0; i < n; i++) {
        const acpi_sdt_header_t* t = (const acpi_sdt_header_t*)(uintptr_t)arr[i];
        if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
            t->signature[2] == 'I' && t->signature[3] == 'C') {
            parse_madt((const acpi_madt_t*)t);
        }
    }
}

// ----------------------------------------------------------------------------
void acpi_init(const mb2_info_t* mb2) {
    for (int i = 0; i < 16; i++) acpi_irq_to_gsi[i] = (uint8_t)i;

    // Сначала ищем XSDP (ACPI 2.0+) — он точнее.
    const mb2_tag_t* tag = mb2_find_tag(mb2, MB2_TAG_ACPI_NEW);
    if (tag) {
        const mb2_tag_acpi_t* a = (const mb2_tag_acpi_t*)tag;
        const acpi_xsdp_t* xsdp = (const acpi_xsdp_t*)a->rsdp;
        serial_puts("[ACPI] XSDP found via Multiboot2 tag 15\n");
        if (xsdp->v1.revision >= 2 && xsdp->xsdt_address) {
            walk_xsdt(xsdp->xsdt_address);
            return;
        }
        // fallback на RSDT внутри XSDP
        if (xsdp->v1.rsdt_address) {
            walk_rsdt((uint64_t)xsdp->v1.rsdt_address);
            return;
        }
    }

    tag = mb2_find_tag(mb2, MB2_TAG_ACPI_OLD);
    if (tag) {
        const mb2_tag_acpi_t* a = (const mb2_tag_acpi_t*)tag;
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)a->rsdp;
        serial_puts("[ACPI] RSDP found via Multiboot2 tag 14\n");
        if (rsdp->rsdt_address) {
            walk_rsdt((uint64_t)rsdp->rsdt_address);
            return;
        }
    }

    serial_puts("[ACPI] WARNING: no RSDP from GRUB — acpi data unavailable\n");
}
