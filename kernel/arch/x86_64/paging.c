// ============================================================================
// paging.c — реализация map/unmap/walk поверх произвольного PML4.
// ----------------------------------------------------------------------------
// Особенность boot.asm: PD-entries 2-MB больших страниц (PS=1) для всех 4 GB.
// Если кто-то вызовет paging_map_*(virt, ...) для адреса внутри такой большой
// страницы — нам надо "сломать" её: выделить новую PT (4 KB), заполнить её
// 512 PTE-шками, описывающими тот же 2-MB регион 4 KB страницами с теми же
// флагами, и заменить PDE.
//
// Этап 37.4b: добавлены *_in(pml4, ...) версии, которые работают с любым PML4
// (для per-process vm_space). Старые wrappers без _in() продолжают использовать
// boot_pml4 — обратная совместимость с 37.2/37.3/37.4a.
// ============================================================================
#include "paging.h"
#include "pmm.h"
#include "serial.h"

// Из boot.asm.
extern uint64_t boot_pml4[512];

// [38.1.1] Сдвиг образа ядра в физической памяти (memory.h). Пишет входная
// заглушка boot.asm ещё до kernel_main64; при старой раскладке остаётся 0.
uint64_t boot_kphys_delta = 0;

// Доступ к таблицам по физическому адресу.
// Поскольку нижние 4 GB identity-маплены, физ. адрес == виртуальный.
static inline uint64_t* phys_to_table(uint64_t phys) {
    return (uint64_t*)(uintptr_t)(phys & PG_PHYS_MASK);
}

// ----------------------------------------------------------------------------
// Хелперы построения цепочки таблиц
// ----------------------------------------------------------------------------
static bool ensure_pdpt(uint64_t* pml4, uint64_t v) {
    uint64_t i = pml4_index(v);
    if (pml4[i] & PG_PRESENT) return true;
    uint64_t p = pmm_alloc_page();
    if (!p) return false;
    uint64_t* t = phys_to_table(p);
    for (int k = 0; k < 512; k++) t[k] = 0;
    pml4[i] = p | PG_PRESENT | PG_WRITE | PG_USER;
    return true;
}

static bool ensure_pd(uint64_t* pdpt, uint64_t v) {
    uint64_t i = pdpt_index(v);
    if (pdpt[i] & PG_PRESENT) return true;
    uint64_t p = pmm_alloc_page();
    if (!p) return false;
    uint64_t* t = phys_to_table(p);
    for (int k = 0; k < 512; k++) t[k] = 0;
    pdpt[i] = p | PG_PRESENT | PG_WRITE | PG_USER;
    return true;
}

// Если PD-entry уже описывает 2-MB страницу (PS=1) — разбиваем её на 4-KB PT.
static bool break_2m_to_4k(uint64_t* pd, uint64_t v) {
    uint64_t i = pd_index(v);
    uint64_t entry = pd[i];

    if ((entry & PG_PRESENT) == 0) {
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return false;
        uint64_t* pt = phys_to_table(pt_phys);
        for (int k = 0; k < 512; k++) pt[k] = 0;
        pd[i] = pt_phys | PG_PRESENT | PG_WRITE | PG_USER;
        return true;
    }

    if ((entry & PG_PS) == 0) return true;          // уже 4-KB PT

    // Разбиваем 2-MB на 512 × 4-KB.
    uint64_t big_base  = entry & 0xFFFFFFFFFFE00000ULL;
    uint64_t big_flags = entry & ~(PG_PHYS_MASK | PG_PS);
    big_flags &= ~PG_PS;

    uint64_t pt_phys = pmm_alloc_page();
    if (!pt_phys) return false;
    uint64_t* pt = phys_to_table(pt_phys);
    for (int k = 0; k < 512; k++)
        pt[k] = (big_base + (uint64_t)k * PAGE_SIZE) | big_flags;
    pd[i] = pt_phys | PG_PRESENT | PG_WRITE | PG_USER;

    for (uint64_t k = 0; k < 512; k++)
        paging_invlpg(big_base + k * PAGE_SIZE);
    return true;
}

// ----------------------------------------------------------------------------
void paging_init(void) {
    serial_puts("[paging] using boot PML4 @ 0x");
    serial_print_hex((uint64_t)(uintptr_t)boot_pml4, 16);
    serial_puts(" (phys 0x");
    serial_print_hex(virt_to_phys(boot_pml4), 16);
    serial_puts(")\n");
    // По этой строке тесты 38.1 узнают, куда загрузчик положил ядро.
    serial_puts("[boot] kernel image: phys 0x");
    serial_print_hex(kernel_phys_start(), 16);
    serial_puts("..0x");
    serial_print_hex(kernel_phys_end(), 16);
    serial_puts(", virt base 0x");
    serial_print_hex(KERNEL_VMA, 16);
    serial_puts(", delta 0x");
    serial_print_hex(boot_kphys_delta, 16);
    serial_puts("\n");
}

// ----------------------------------------------------------------------------
// [49.1] RAM выше 4 ГБ. Раньше ядро видело только нижние 4 ГБ — а на q35 и
// многих ПК ниже 4 ГБ всего 2-3 ГБ RAM (остальное адресное пространство
// занимают PCI-окна), и веса Qwen3-1.7B (1.8 ГБ одним блоком) туда не
// помещались. Таблица PD на каждый гигабайт берётся из нижней памяти.
// Добавляются только ранее отсутствовавшие записи PDPT: «не присутствующие»
// трансляции процессор не кэширует, но CR3 всё равно перезагружаем.
// ----------------------------------------------------------------------------
extern uint64_t boot_pdpt[512];

uint64_t paging_map_high_identity(uint64_t top) {
    const uint64_t GB1 = 1ULL << 30;
    uint64_t gb = (top + GB1 - 1) / GB1;
    if (gb > 512) gb = 512;
    uint64_t mapped = 4 * GB1;
    for (uint64_t g = 4; g < gb; g++) {
        if (!(boot_pdpt[g] & PG_PRESENT)) {
            uint64_t pd = pmm_alloc_page();
            if (!pd) break;
            uint64_t* t = phys_to_table(pd);
            for (uint64_t j = 0; j < 512; j++)
                t[j] = (g * GB1 + j * (2ULL << 20)) | PG_PRESENT | PG_WRITE | PG_PS;
            boot_pdpt[g] = pd | PG_PRESENT | PG_WRITE;
        }
        mapped = (g + 1) * GB1;
    }
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
    return mapped;
}

uint64_t  paging_kernel_pml4_phys(void) { return virt_to_phys(boot_pml4); }
uint64_t* paging_kernel_pml4(void)      { return boot_pml4; }

// ----------------------------------------------------------------------------
// _in() версии — работают с указанным PML4
// ----------------------------------------------------------------------------
bool paging_map_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    virt &= ~PAGE_MASK;
    phys &= ~PAGE_MASK;

    if (!ensure_pdpt(pml4, virt)) return false;
    uint64_t* pdpt = phys_to_table(pml4[pml4_index(virt)]);
    if (!ensure_pd(pdpt, virt)) return false;
    uint64_t* pd = phys_to_table(pdpt[pdpt_index(virt)]);
    if (!break_2m_to_4k(pd, virt)) return false;
    uint64_t* pt = phys_to_table(pd[pd_index(virt)]);

    pt[pt_index(virt)] = phys | (flags & ~PG_PHYS_MASK) | PG_PRESENT;
    paging_invlpg(virt);
    return true;
}

bool paging_map_user_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, bool writable) {
    virt &= ~PAGE_MASK;
    phys &= ~PAGE_MASK;

    if (!ensure_pdpt(pml4, virt)) return false;
    pml4[pml4_index(virt)] |= PG_USER;

    uint64_t* pdpt = phys_to_table(pml4[pml4_index(virt)]);
    if (!ensure_pd(pdpt, virt)) return false;
    pdpt[pdpt_index(virt)] |= PG_USER;

    uint64_t* pd = phys_to_table(pdpt[pdpt_index(virt)]);
    if (!break_2m_to_4k(pd, virt)) return false;
    pd[pd_index(virt)] |= PG_USER;

    uint64_t* pt = phys_to_table(pd[pd_index(virt)]);
    uint64_t flags = PG_PRESENT | PG_USER;
    if (writable) flags |= PG_WRITE;
    pt[pt_index(virt)] = phys | flags;
    paging_invlpg(virt);
    return true;
}

bool paging_unmap_page_in(uint64_t* pml4, uint64_t virt) {
    virt &= ~PAGE_MASK;
    if (!(pml4[pml4_index(virt)] & PG_PRESENT)) return false;
    uint64_t* pdpt = phys_to_table(pml4[pml4_index(virt)]);
    if (!(pdpt[pdpt_index(virt)] & PG_PRESENT)) return false;
    uint64_t* pd = phys_to_table(pdpt[pdpt_index(virt)]);
    uint64_t pde = pd[pd_index(virt)];
    if (!(pde & PG_PRESENT)) return false;
    if (pde & PG_PS) {
        if (!break_2m_to_4k(pd, virt)) return false;
        pde = pd[pd_index(virt)];
    }
    uint64_t* pt = phys_to_table(pde);
    pt[pt_index(virt)] = 0;
    paging_invlpg(virt);
    return true;
}

uint64_t paging_virt_to_phys_in(uint64_t* pml4, uint64_t virt) {
    uint64_t off = virt & PAGE_MASK;
    uint64_t e = pml4[pml4_index(virt)];
    if (!(e & PG_PRESENT)) return 0;
    uint64_t* pdpt = phys_to_table(e);
    e = pdpt[pdpt_index(virt)];
    if (!(e & PG_PRESENT)) return 0;
    if (e & PG_PS) return (e & 0xFFFFFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);
    uint64_t* pd = phys_to_table(e);
    e = pd[pd_index(virt)];
    if (!(e & PG_PRESENT)) return 0;
    if (e & PG_PS) return (e & 0xFFFFFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);
    uint64_t* pt = phys_to_table(e);
    e = pt[pt_index(virt)];
    if (!(e & PG_PRESENT)) return 0;
    return (e & PG_PHYS_MASK) | off;
}

// ----------------------------------------------------------------------------
// Backward-compat: над boot_pml4
// ----------------------------------------------------------------------------
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_page_in(boot_pml4, virt, phys, flags);
}
bool paging_map_user_page(uint64_t virt, uint64_t phys, bool writable) {
    return paging_map_user_page_in(boot_pml4, virt, phys, writable);
}
bool paging_unmap_page(uint64_t virt) {
    return paging_unmap_page_in(boot_pml4, virt);
}
uint64_t paging_virt_to_phys(uint64_t virt) {
    return paging_virt_to_phys_in(boot_pml4, virt);
}
