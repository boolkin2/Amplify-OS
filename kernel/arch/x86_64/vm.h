// ============================================================================
// vm.h — per-process address space (vm_space) для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Архитектурный приём (без higher-half kernel'а — это для 37.6):
//
//   PML4[0]   — kernel space (общий, shallow-копия из boot_pml4): покрывает
//               первые 512 GB вирт. памяти. Включает identity-mapped 4 GB,
//               где живут BIOS/kernel/heap/MMIO. Все процессы видят одно
//               и то же ядро.
//   PML4[1]   — per-process user space (covers 512 GB..1 TB). Каждый
//               vm_space владеет СВОИМ PDPT/PD/PT под этой записью.
//   PML4[2..] — пока не используются.
//
// Преимущество: чтобы переключить процесс, достаточно загрузить новый CR3.
// Кернельный код продолжает работать (PML4[0] тот же), а user-страницы
// одного процесса невидимы другому.
//
// USER_BASE = 0x80_0000_0000 = 512 GB — ELF-loader линкует туда entry _start.
// ============================================================================
#ifndef AOS_X86_64_VM_H
#define AOS_X86_64_VM_H

#include <stdint.h>
#include <stdbool.h>

// База пользовательского адресного пространства = старт PML4[1].
#define USER_BASE        0x0000008000000000ULL    // 512 GB
#define USER_LIMIT       0x000000FFFFFFFFFFULL    // конец PML4[1] - 1

typedef struct vm_space {
    uint64_t* pml4;            // virtual = phys (identity-mapped < 4 GB)
    uint64_t  pml4_phys;
    int       refcount;
    uint64_t  brk;             // sbrk top (0 = не используется)
} vm_space_t;

void         vm_init(void);                       // инициализирует kernel_vm
vm_space_t*  vm_kernel(void);                     // shared kernel vm (boot_pml4)
vm_space_t*  vm_create_user(void);                // alloc PML4 + копия PML4[0] из boot
void         vm_destroy(vm_space_t* vm);          // освобождает user-страницы и PML4
vm_space_t*  vm_retain(vm_space_t* vm);           // ++refcount
void         vm_release(vm_space_t* vm);          // --refcount; при 0 → vm_destroy

void         vm_switch(vm_space_t* vm);           // CR3 = vm->pml4_phys
vm_space_t*  vm_current(void);

// Маппинг user-страницы в адресное пространство процесса.
bool         vm_map_user_page(vm_space_t* vm, uint64_t virt, uint64_t phys, bool writable);

// Walk PML4 vm: virt → phys. 0 если не маплено.
uint64_t     vm_virt_to_phys (vm_space_t* vm, uint64_t virt);

// ----------------------------------------------------------------------------
// Этап 39 (musl-aos libc): heap-управление пользовательского процесса.
// База кучи живёт в vm->brk, начальное значение USER_HEAP_BASE — отдельное
// окно высоко над .text/.data/stack пользователя, чтобы grow никогда не
// конфликтовал ни с ELF-сегментами (USER_BASE+0..filesz), ни со стеком
// (USER_BASE+0x100000..). Конец окна — USER_HEAP_LIMIT.
//
// vm_brk_grow:
//   delta == 0  → вернуть текущий brk (как POSIX sbrk(0)).
//   delta  > 0  → выделить страницы под [old_brk, old_brk+delta), смапить
//                 как user-writable, обновить brk, вернуть old_brk.
//   delta  < 0  → отдать высвободившиеся страницы (опционально на 39-й —
//                 пока вернём old_brk и просто двигаем указатель назад).
// Возвращает (uint64_t)-1 при OOM/out-of-range.
// ----------------------------------------------------------------------------
#define USER_HEAP_BASE   0x0000008010000000ULL    // USER_BASE + 256 MB
#define USER_HEAP_LIMIT  0x0000008080000000ULL    // USER_BASE + 2 GB

uint64_t     vm_brk_grow(vm_space_t* vm, int64_t delta);

#endif
