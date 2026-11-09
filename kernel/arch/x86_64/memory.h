// ============================================================================
// memory.h — общие константы памяти для 64-битного ядра AMPLIFY-OS.
// ============================================================================
#ifndef AOS_X86_64_MEMORY_H
#define AOS_X86_64_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Базовые единицы.
#define KB                  1024UL
#define MB                  (1024UL * KB)
#define GB                  (1024UL * MB)

// Размер страницы (стандарт x86: 4 KB).
#define PAGE_SIZE           4096UL
#define PAGE_MASK           (PAGE_SIZE - 1UL)
#define PAGE_SHIFT          12

// Round-up/round-down до границы страницы.
#define PAGE_ALIGN_DOWN(x)  ((uintptr_t)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    (((uintptr_t)(x) + PAGE_MASK) & ~PAGE_MASK)

// Размер 2-MB страницы (используется в boot.asm).
#define LARGE_PAGE_SIZE     (2UL * MB)

// Максимум физической памяти, который PMM 37.2 умеет учитывать
// (хватит для нашей boot.asm identity-мапы 4 GB).
#define PMM_MAX_PHYS        (4UL * GB)
#define PMM_MAX_PAGES       (PMM_MAX_PHYS / PAGE_SIZE)
#define PMM_BITMAP_BYTES    (PMM_MAX_PAGES / 8)

// ----------------------------------------------------------------------------
// Виртуальный layout 64-битного ядра (нижние 4 GB identity-mapped boot.asm).
// ----------------------------------------------------------------------------
//   0x00000000 .. 0x000FFFFF  : BIOS / VGA / ACPI / EBDA          (reserved)
//   0x00100000 .. _kernel_end : ядро AMPLIFY-OS                   (boot)
//   0x01000000 .. 0x02FFFFFF  : KERNEL HEAP (16 MB .. 48 MB)      (32 MB)
//   0x03000000 .. 0xFFFFFFFF  : свободная динамическая память     (PMM)
// ----------------------------------------------------------------------------
#define KHEAP_START         (16UL * MB)
#define KHEAP_SIZE          (128UL * MB)
#define KHEAP_END           (KHEAP_START + KHEAP_SIZE)

// Символы из linker_x86_64.ld.
extern uint8_t _bss_start[];
extern uint8_t _bss_end[];
extern uint8_t _kernel_end[];

// ----------------------------------------------------------------------------
// [38.1.1] Виртуальный адрес <-> физический.
//
// Образ ядра слинкован на KERNEL_VMA + x, а лежит в памяти по адресу
// boot_kphys_delta + x: сдвиг выбирает загрузчик (GRUB — 0, наш BOOTX64.EFI —
// куда прошивка дала место), узнаёт его входная заглушка boot.asm. Всё
// остальное (куча, страницы pmm, MMIO) пока доступно по тождественному
// отображению: там виртуальный адрес равен физическому.
//
// Поэтому «указатель -> число» больше НЕ даёт физический адрес, если
// указатель смотрит в образ ядра (статический массив, .data, .bss). Всё, что
// уходит устройству (DMA), в CR3 или в VMCS, обязано идти через virt_to_phys.
// phys_to_virt пока тождественна; настоящий physmap появится в 38.1.3.
//
// make HIGHER_HALF=0 — старая раскладка (ядро по адресу 1 МБ): KERNEL_VMA = 0,
// сдвиг 0, обе функции тождественны.
// ----------------------------------------------------------------------------
#ifdef AOS_HIGHER_HALF
#define KERNEL_VMA          0xFFFFFFFF80000000UL
#else
#define KERNEL_VMA          0UL
#endif

// Адрес загрузки (LMA) первого байта образа — 1 МБ, см. линкер-скрипт.
#define KERNEL_LMA_BASE     (1UL * MB)

extern uint64_t boot_kphys_delta;

static inline uint64_t virt_to_phys(const void* p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
#ifdef AOS_HIGHER_HALF
#ifdef AOS_MUT_V2P_NO_DELTA
    // Намеренная порча для теста 38.1.1 («тест умеет падать»): сдвиг забыт.
    // При сдвиге 0 ошибка не видна, при любом другом устройства получают
    // чужие адреса.
    if (v >= KERNEL_VMA) return v - KERNEL_VMA;
#endif
    if (v >= KERNEL_VMA) return v - KERNEL_VMA + boot_kphys_delta;
#endif
    return v;
}

static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(uintptr_t)phys;
}

// Где образ ядра лежит в физической памяти.
static inline uint64_t kernel_phys_start(void) { return KERNEL_LMA_BASE + boot_kphys_delta; }
static inline uint64_t kernel_phys_end(void)   { return virt_to_phys(_kernel_end); }

#endif
