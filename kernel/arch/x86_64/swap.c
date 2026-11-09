// ============================================================================
// swap.c — реализация pagefile swap через ATA напрямую.
// ============================================================================
#include "swap.h"
#include "disk_io.h"
#include "ata.h"
#include "serial.h"
#include <stddef.h>

static uint8_t swap_bitmap[SWAP_MAX_PAGES / 8];
static bool    swap_enabled = false;

void swap_init(void) {
    if (!ata_present()) {
        serial_puts("[swap] no disk — disabled\n");
        return;
    }
    // Проверим, что диск достаточно большой.
    const ata_info_t* di = ata_info();
    uint64_t need = SWAP_LBA_BASE + (uint64_t)SWAP_MAX_PAGES * (SWAP_PAGE_SIZE / 512);
    if (di->total_sectors < need) {
        serial_puts("[swap] disk too small for swap area — disabled\n");
        return;
    }

    for (size_t i = 0; i < sizeof(swap_bitmap); i++) swap_bitmap[i] = 0;
    swap_enabled = true;

    serial_puts("[swap] enabled @ LBA ");
    serial_print_dec(SWAP_LBA_BASE);
    serial_puts(" (");
    serial_print_dec(SWAP_MAX_PAGES);
    serial_puts(" slots, ");
    serial_print_dec((SWAP_MAX_PAGES * SWAP_PAGE_SIZE) / (1024 * 1024));
    serial_puts(" MB)\n");
}

bool swap_is_enabled(void) { return swap_enabled; }

uint32_t swap_out_page(uint64_t phys_addr) {
    if (!swap_enabled) return (uint32_t)-1;

    int slot = -1;
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (!(swap_bitmap[i / 8] & (1 << (i % 8)))) { slot = i; break; }
    }
    if (slot < 0) {
        serial_puts("[swap] OOM (all slots used)\n");
        return (uint32_t)-1;
    }
    swap_bitmap[slot / 8] |= (1 << (slot % 8));

    // Пишем 4 KB = 8 секторов.
    uint64_t lba = SWAP_LBA_BASE + (uint64_t)slot * (SWAP_PAGE_SIZE / 512);
    if (!disk_write_sectors(lba, SWAP_PAGE_SIZE / 512, (const void*)(uintptr_t)phys_addr)) {
        // Откат
        swap_bitmap[slot / 8] &= ~(1 << (slot % 8));
        return (uint32_t)-1;
    }
    return (uint32_t)slot;
}

int swap_in_page(uint32_t slot, uint64_t phys_addr) {
    if (!swap_enabled || slot >= SWAP_MAX_PAGES) return -1;
    if (!(swap_bitmap[slot / 8] & (1 << (slot % 8)))) return -1;

    uint64_t lba = SWAP_LBA_BASE + (uint64_t)slot * (SWAP_PAGE_SIZE / 512);
    if (!disk_read_sectors(lba, SWAP_PAGE_SIZE / 512, (void*)(uintptr_t)phys_addr)) return -1;

    swap_bitmap[slot / 8] &= ~(1 << (slot % 8));
    return 0;
}

void swap_free_slot(uint32_t slot) {
    if (slot < SWAP_MAX_PAGES) swap_bitmap[slot / 8] &= ~(1 << (slot % 8));
}

uint32_t swap_used_slots(void) {
    uint32_t n = 0;
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (swap_bitmap[i / 8] & (1 << (i % 8))) n++;
    }
    return n;
}
