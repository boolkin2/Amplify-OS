// ============================================================================
// pmm.h — Physical Memory Manager (bitmap) для 64-битного AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Та же идеология, что в kernel/memory/pmm.c:
//   - bitmap (1 бит = 1 страница 4 KB)
//   - per-page refcount (1 байт) для будущего Copy-On-Write
// Адреса теперь 64-битные (uint64_t / uintptr_t).
// Bitmap-аллокатор (страницы, refcount) покрывает нижние PMM_MAX_PHYS (= 4 GB):
// там живут таблицы страниц, DMA-буферы и всё «мелкое».
// [49.1] Память ВЫШЕ 4 ГБ — отдельный список свободных экстентов для больших
// непрерывных блоков (веса AMPLIFY MIND, KV-кэш): pmm_alloc_big. Её
// identity-map строит paging_map_high_identity при pmm_init.
// ============================================================================
#ifndef AOS_X86_64_PMM_H
#define AOS_X86_64_PMM_H

#include "memory.h"
#include "multiboot2.h"

#define PMM_PAGE_OK        ((uint64_t)0)         // 0 = нет страницы (как в i386)
#define PMM_INVALID        ((uint64_t)0)

typedef struct {
    uint64_t total_phys;        // байт RAM, которые мы видим
    uint64_t usable_phys;       // байт RAM типа available (type=1) из mmap
    uint64_t total_pages;       // округлённое total_phys / PAGE_SIZE
    uint64_t usable_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t high_total_pages;  // [49.1] RAM выше 4 ГБ (отображённая)
    uint64_t high_free_pages;
} pmm_stats_t;

// Инициализация: парсит Multiboot2 mmap, помечает свободные регионы,
// резервирует 0..1MB и сам kernel image (1MB .. _kernel_end), плюс bitmap+refcount.
void     pmm_init(const mb2_info_t* info);

// [41.2] Перенести модули MB2 и сам инфоблок из диапазона кучи ядра, если
// загрузчик положил их туда (GRUB в EFI-режиме). Звать сразу после pmm_init
// и ДО heap_init; дальше пользоваться только возвращённым адресом инфоблока.
const mb2_info_t* pmm_relocate_boot_data(const mb2_info_t* info);

// Выделение/освобождение одной страницы. Возвращает 64-битный физический адрес.
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys);

// Несколько последовательных страниц.
uint64_t pmm_alloc_pages(uint64_t count);
void     pmm_free_pages(uint64_t phys, uint64_t count);

// [49.1] Большой непрерывный блок: сначала из памяти выше 4 ГБ (first-fit по
// экстентам), иначе — pmm_alloc_pages ниже 4 ГБ. Только для ядра и только
// CPU-доступ (не для DMA 32-битных устройств). 0 — не нашлось.
uint64_t pmm_alloc_big(uint64_t pages);
void     pmm_free_big(uint64_t phys, uint64_t pages);
// Самый большой блок, который сейчас выдаст pmm_alloc_big (байт).
uint64_t pmm_big_largest_free(void);
// Вся RAM (ниже и выше 4 ГБ): всего и свободно, байт.
uint64_t pmm_total_ram(void);
uint64_t pmm_free_ram(void);

// Резервирование области (помечает занятой). Используется для heap, MMIO, fb.
void     pmm_reserve_region(uint64_t start, uint64_t size);
void     pmm_release_region(uint64_t start, uint64_t size);

// Refcount для COW.
void     pmm_inc_ref(uint64_t phys);
uint8_t  pmm_get_ref(uint64_t phys);

// Диагностика.
pmm_stats_t pmm_get_stats(void);
bool        pmm_is_free(uint64_t phys);

#endif
