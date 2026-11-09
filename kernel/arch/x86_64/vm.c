// ============================================================================
// vm.c — реализация per-process vm_space.
// ============================================================================
#include "vm.h"
#include "paging.h"
#include "pmm.h"
#include "memory.h"
#include "serial.h"

// Глобальное "kernel vm", обёртка вокруг boot_pml4.
static vm_space_t kernel_vm;
static int        kernel_vm_inited = 0;
static vm_space_t* current_vm = 0;

// ----------------------------------------------------------------------------
void vm_init(void) {
    kernel_vm.pml4       = paging_kernel_pml4();
    kernel_vm.pml4_phys  = paging_kernel_pml4_phys();
    kernel_vm.refcount   = 1;
    kernel_vm.brk        = 0;
    kernel_vm_inited     = 1;
    current_vm           = &kernel_vm;

    serial_puts("[vm] kernel vm initialized, PML4=0x");
    serial_print_hex(kernel_vm.pml4_phys, 16);
    serial_puts("\n");
}

vm_space_t* vm_kernel(void) {
    return kernel_vm_inited ? &kernel_vm : 0;
}

vm_space_t* vm_current(void) {
    return current_vm;
}

// ----------------------------------------------------------------------------
vm_space_t* vm_create_user(void) {
    // 1) Новая PML4-страница.
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return 0;

    uint64_t* pml4 = (uint64_t*)(uintptr_t)pml4_phys;     // identity-mapped

    // 2) Копируем ВСЕ записи из boot PML4 (kernel-half).
    //    В boot_pml4 заполнен только PML4[0]. Остальные = 0.
    //    После копирования PML4[1] = 0 → user space будет аллоцирован
    //    лениво при первом vm_map_user_page().
    uint64_t* boot = paging_kernel_pml4();
    for (int i = 0; i < 512; i++) pml4[i] = boot[i];

    // 3) Создаём структуру vm_space. Аллоцируем её НЕ из heap (kmalloc),
    //    а из своей же PMM-страницы — иначе при vm_destroy придётся гонять
    //    heap-зависимости. На самом деле чтобы не мудрить, используем
    //    статический pool позже; пока выделяем отдельную страницу под struct.
    uint64_t struct_phys = pmm_alloc_page();
    if (!struct_phys) {
        pmm_free_page(pml4_phys);
        return 0;
    }
    vm_space_t* vm = (vm_space_t*)(uintptr_t)struct_phys;
    vm->pml4       = pml4;
    vm->pml4_phys  = pml4_phys;
    vm->refcount   = 1;
    vm->brk        = USER_BASE;

    return vm;
}

// ----------------------------------------------------------------------------
// Free user PT/PD/PDPT chain под PML4[1].
// PML4[0..0] (kernel) — не трогаем (shared).
// ----------------------------------------------------------------------------
static void free_user_pdpt(uint64_t pdpt_phys) {
    if (!pdpt_phys) return;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pdpt_phys & ~0xFFFULL);
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & 1)) continue;
        uint64_t pd_phys = pdpt[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t* pd = (uint64_t*)(uintptr_t)pd_phys;
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & 1)) continue;
            if (pd[j] & 0x80) continue;  // 2-MB страница — это shared/boot, не фриим
            uint64_t pt_phys = pd[j] & 0x000FFFFFFFFFF000ULL;
            uint64_t* pt = (uint64_t*)(uintptr_t)pt_phys;
            // Освобождаем все физ. страницы, на которые указывают PTE.
            for (int k = 0; k < 512; k++) {
                if (pt[k] & 1) pmm_free_page(pt[k] & 0x000FFFFFFFFFF000ULL);
            }
            pmm_free_page(pt_phys);
        }
        pmm_free_page(pd_phys);
    }
    pmm_free_page(pdpt_phys);
}

void vm_destroy(vm_space_t* vm) {
    if (!vm || vm == &kernel_vm) return;

    // Если активный — переключиться на kernel_vm перед освобождением.
    if (current_vm == vm) vm_switch(&kernel_vm);

    // Только PML4[1] (user space) — free.  PML4[0] делится с kernel_vm.
    uint64_t pdpt = vm->pml4[1] & 0x000FFFFFFFFFF000ULL;
    if (pdpt) free_user_pdpt(pdpt);

    pmm_free_page(vm->pml4_phys);
    pmm_free_page((uint64_t)(uintptr_t)vm);
}

vm_space_t* vm_retain(vm_space_t* vm) {
    if (vm) vm->refcount++;
    return vm;
}

void vm_release(vm_space_t* vm) {
    if (!vm || vm == &kernel_vm) return;
    if (--vm->refcount <= 0) vm_destroy(vm);
}

// ----------------------------------------------------------------------------
void vm_switch(vm_space_t* vm) {
    if (!vm) vm = &kernel_vm;
    if (vm == current_vm) return;
    paging_load_cr3(vm->pml4_phys);
    current_vm = vm;
}

bool vm_map_user_page(vm_space_t* vm, uint64_t virt, uint64_t phys, bool writable) {
    if (!vm) vm = &kernel_vm;
    return paging_map_user_page_in(vm->pml4, virt, phys, writable);
}

uint64_t vm_virt_to_phys(vm_space_t* vm, uint64_t virt) {
    if (!vm) vm = &kernel_vm;
    return paging_virt_to_phys_in(vm->pml4, virt);
}

// ----------------------------------------------------------------------------
// vm_brk_grow — рост кучи процесса в окне [USER_HEAP_BASE, USER_HEAP_LIMIT).
// Используется AOS_SYS_SBRK через kernel/arch/x86_64/syscall.c.
//
// Идея: vm->brk — указатель на «следующий байт за концом валидной кучи».
// При первом вызове brk(0) или sbrk(0) ленивая инициализация поднимает
// его до USER_HEAP_BASE (vm_create_user ставит туда USER_BASE — fix here,
// чтобы не пересекать .text/.data/stack ELF'а).
//
// Аллокация — постранично; если delta не кратно PAGE_SIZE, округляем вверх.
// Каждая страница получается через pmm_alloc_page() и маппится user-writable.
// ----------------------------------------------------------------------------
#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000ULL
#endif

uint64_t vm_brk_grow(vm_space_t* vm, int64_t delta) {
    if (!vm) vm = current_vm;
    if (!vm) return (uint64_t)-1;

    // Ленивая инициализация — поднимаем нулевой/«ELF base»-brk на USER_HEAP_BASE.
    if (vm->brk < USER_HEAP_BASE) vm->brk = USER_HEAP_BASE;

    uint64_t old_brk = vm->brk;

    if (delta == 0) return old_brk;

    if (delta < 0) {
        // Сжатие — TLB-флаш и unmap страниц мы пока не делаем (libc не требует
        // обратной отдачи). Просто двигаем указатель — следующий рост
        // переиспользует уже выделенные страницы (vm_map_user_page игнорит
        // повторный mapping).
        uint64_t new_brk = old_brk - (uint64_t)(-delta);
        if (new_brk < USER_HEAP_BASE) new_brk = USER_HEAP_BASE;
        vm->brk = new_brk;
        return old_brk;
    }

    // delta > 0 — выделяем недостающие страницы.
    uint64_t new_brk = old_brk + (uint64_t)delta;
    if (new_brk > USER_HEAP_LIMIT) return (uint64_t)-1;   // OOM range

    uint64_t page_start = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t page_end   = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
        // Если страница уже маплена (после неудачной попытки или прошлого
        // sbrk + shrink) — пропускаем.
        if (vm_virt_to_phys(vm, va)) continue;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return (uint64_t)-1;

        // Зануляем — POSIX гарантирует, что свежий sbrk-регион нулевой.
        uint8_t* p = (uint8_t*)(uintptr_t)phys;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) p[i] = 0;

        if (!vm_map_user_page(vm, va, phys, true)) {
            pmm_free_page(phys);
            return (uint64_t)-1;
        }
    }

    vm->brk = new_brk;
    return old_brk;
}
