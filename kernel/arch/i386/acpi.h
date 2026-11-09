#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;

typedef struct {
    acpi_header_t header;
    uint32_t pointer_to_other_sdt[];
} __attribute__((packed)) acpi_rsdt_t;

typedef struct {
    acpi_header_t header;
    uint32_t local_apic_addr;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

#define MAX_CPUS 16

extern uint32_t bsp_lapic_id;
extern uint32_t cpu_lapic_ids[MAX_CPUS];
extern int num_cpus;
extern uint32_t local_apic_base;

void acpi_init(void);

#endif // ACPI_H
