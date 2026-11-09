// ============================================================================
// acpi.h — RSDP/XSDT/MADT парсер для 64-битного AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Источник RSDP — Multiboot2 tag 14 (ACPI 1.0) или tag 15 (ACPI 2.0+ XSDP).
// Это надёжнее чем поиск в EBDA/0xE0000-0xFFFFF (как делает kernel/arch/
// i386/acpi.c) — GRUB сам нашёл и положил.
// ============================================================================
#ifndef AOS_X86_64_ACPI_H
#define AOS_X86_64_ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"
#include "cpu_stats.h"

// ----------------------------------------------------------------------------
// RSDP / XSDP
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    char     signature[8];      // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          // 0 = ACPI 1.0, 2+ = ACPI 2.0+
    uint32_t rsdt_address;
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    acpi_rsdp_t v1;
    uint32_t    length;
    uint64_t    xsdt_address;   // 64-битный указатель на XSDT
    uint8_t     ext_checksum;
    uint8_t     reserved[3];
} acpi_xsdp_t;

// ----------------------------------------------------------------------------
// SDT header (общий для всех ACPI таблиц)
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// ----------------------------------------------------------------------------
// MADT (signature "APIC")
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t          local_apic_addr;
    uint32_t          flags;
    // Далее переменное количество MADT-entries:
    //   uint8_t type, uint8_t length, ...
} acpi_madt_t;

// MADT entry types.
#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_INT_OVERRIDE   2
#define MADT_NMI_SOURCE     3
#define MADT_LAPIC_NMI      4
#define MADT_LAPIC_OVERRIDE 5
#define MADT_X2APIC         9

// ----------------------------------------------------------------------------
// Глобальные данные, заполняемые acpi_init()
// ----------------------------------------------------------------------------
extern uint64_t  acpi_lapic_phys;            // физ. адрес MMIO LAPIC (default 0xFEE00000)
extern uint64_t  acpi_ioapic_phys;           // физ. адрес MMIO IOAPIC (0xFEC00000)
extern uint8_t   acpi_ioapic_id;
extern uint32_t  acpi_ioapic_gsi_base;
extern uint32_t  acpi_lapic_ids[MAX_CPUS];   // список enabled LAPIC ID
extern int       acpi_num_cpus;
extern uint32_t  acpi_bsp_lapic_id;

// IRQ → GSI override (для legacy IRQs которые перенаправлены через INT_OVERRIDE).
extern uint8_t   acpi_irq_to_gsi[16];

void acpi_init(const mb2_info_t* mb2);

#endif
