// ============================================================================
// paging.h — 4-level paging API для 64-битного AMPLIFY-OS.
// ----------------------------------------------------------------------------
// PML4 → PDPT → PD → PT, 4 KB страницы. Боoт.asm уже identity-мапит первые
// 4 GB через 2-MB страницы (PS=1) — мы оставляем эту мапу как initial CR3,
// и API map_page() лениво "разламывает" 2-MB страницы на 512 × 4-KB PTE
// при первом обращении к конкретному виртуальному адресу.
//
// Будущие этапы:
//   - 37.4: per-process address spaces (CR3 switch на vm_space.c)
//   - 37.6: higher-half mapping ядра (0xFFFF8000_00000000+)
// ============================================================================
#ifndef AOS_X86_64_PAGING_H
#define AOS_X86_64_PAGING_H

#include "memory.h"

// Биты записи Page Table Entry / Page Directory Entry / etc.
#define PG_PRESENT          (1ULL << 0)
#define PG_WRITE            (1ULL << 1)
#define PG_USER             (1ULL << 2)
#define PG_PWT              (1ULL << 3)   // Page-Write-Through (uncached MMIO)
#define PG_PCD              (1ULL << 4)   // Page-Cache-Disable
#define PG_ACCESSED         (1ULL << 5)
#define PG_DIRTY            (1ULL << 6)
#define PG_PS               (1ULL << 7)   // Page Size (для PD = 2 MB, для PDPT = 1 GB)
#define PG_GLOBAL           (1ULL << 8)
#define PG_NX               (1ULL << 63)  // No Execute (требует EFER.NXE=1)

#define PG_PHYS_MASK        (0x000FFFFFFFFFF000ULL)

// Индексы внутри 64-битного адреса.
//   bits 47..39 = PML4 index
//   bits 38..30 = PDPT index
//   bits 29..21 = PD   index
//   bits 20..12 = PT   index
//   bits 11..0  = page offset
static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index  (uint64_t v) { return (v >> 12) & 0x1FF; }

void     paging_init(void);

// [49.1] Identity-map физической RAM выше 4 ГБ (boot.asm отображает только
// первые 4 ГБ). Дописывает PDPT[4..] общего PML4[0] 2-МБ страницами — эта
// запись PML4 копируется во все адресные пространства (vm.c), так что новая
// память сразу видна ядру в любом CR3 и на всех AP. Страницы — только для
// ядра (без PG_USER). Возврат: граница, до которой всё отображено.
uint64_t paging_map_high_identity(uint64_t top);
uint64_t paging_kernel_pml4_phys(void);
uint64_t* paging_kernel_pml4(void);     // virtual address (== phys, identity-map)

// ----------------------------------------------------------------------------
// Vm-aware варианты (этап 37.4b): принимают PML4 явно, чтобы можно было
// работать не только с boot_pml4, но и с per-process PML4 из vm.c.
// ----------------------------------------------------------------------------
bool     paging_map_page_in     (uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
bool     paging_map_user_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, bool writable);
bool     paging_unmap_page_in   (uint64_t* pml4, uint64_t virt);
uint64_t paging_virt_to_phys_in (uint64_t* pml4, uint64_t virt);

// ----------------------------------------------------------------------------
// Backward-compat обёртки над *_in() — работают с boot_pml4. Использовались
// в этапах 37.2-37.4a, оставляем чтобы не ломать существующие вызовы.
// ----------------------------------------------------------------------------
bool     paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
bool     paging_map_user_page(uint64_t virt, uint64_t phys, bool writable);
bool     paging_unmap_page(uint64_t virt);
uint64_t paging_virt_to_phys(uint64_t virt);

// invlpg для конкретного адреса.
static inline void paging_invlpg(uint64_t virt) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

// Загрузка нового CR3 (для context switch в 37.4).
static inline void paging_load_cr3(uint64_t pml4_phys) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static inline uint64_t paging_read_cr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

#endif
