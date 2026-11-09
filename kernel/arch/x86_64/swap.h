// ============================================================================
// swap.h — pagefile-style swap для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// На 37.5d-3a: raw-disk swap — последние 4 MB FAT32-образа используются
// как swap area (вне FAT32 файловой структуры). 1024 4KB страниц = 1024 слота.
// Bitmap слотов в .bss (128 байт).
//
// Дальнейшее (37.6+): wire up в page fault handler — при OOM вытесняем
// LRU-страницу в swap, при доступе к swap'нутой странице → #PF → swap_in.
// ============================================================================
#ifndef AOS_X86_64_SWAP_H
#define AOS_X86_64_SWAP_H

#include <stdint.h>
#include <stdbool.h>

#define SWAP_PAGE_SIZE   4096
#define SWAP_MAX_PAGES   1024            // 4 MB swap

// Расположение swap-области на диске (для 16 MB образа):
//   Sectors 0..23423        — FAT32 (boot + FAT + data)
//   Sectors 24576..32767    — SWAP (8192 sectors = 4 MB) ← мы тут
#define SWAP_LBA_BASE    24576

void     swap_init(void);
bool     swap_is_enabled(void);

// Возвращает индекс слота (0..SWAP_MAX_PAGES-1) или (uint32_t)-1 при ошибке.
uint32_t swap_out_page(uint64_t phys_addr);

// 0 при успехе, -1 при ошибке.
int      swap_in_page(uint32_t slot, uint64_t phys_addr);

// Пометить слот свободным (без чтения данных).
void     swap_free_slot(uint32_t slot);

// Сколько слотов занято.
uint32_t swap_used_slots(void);

#endif
