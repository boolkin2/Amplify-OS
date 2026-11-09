// ============================================================================
// vm.c — реализация vm_space с подсчётом ссылок
// ============================================================================

#include "vm.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "memory.h"
#include "../task/task.h"   // USER_MMAP_BASE
#include "../vga.h"

// Освобождение пользовательской части PD (реализовано в paging.c).
// Выделено в отдельную функцию, чтобы не тащить детали PDE/PTE сюда.
extern void paging_destroy_user_directory(page_directory_t* dir);

// Статическое ядерное vm_space. Живёт всё время работы ядра.
// pd проставляется в vm_init(), чтобы не зависеть от порядка инициализации
// глобалов в paging.c (kernel_page_directory — static там).
static vm_space_t kernel_vm;
static int kernel_vm_inited = 0;

// Текущее активное vm_space. Обновляется vm_switch().
static vm_space_t* current_vm = NULL;

void vm_init(void) {
    // paging_get_current_directory() на этом этапе возвращает указатель на
    // kernel_page_directory, уже заряженный в CR3.
    kernel_vm.pd        = paging_get_current_directory();
    kernel_vm.refcount  = 1;     // никогда не уйдёт в 0
    kernel_vm.mmap_top  = 0;
    kernel_vm.is_kernel = 1;
    kernel_vm_inited    = 1;
    current_vm          = &kernel_vm;
}

vm_space_t* vm_kernel(void) {
    return kernel_vm_inited ? &kernel_vm : NULL;
}

vm_space_t* vm_current(void) {
    return current_vm;
}

vm_space_t* vm_create_user(void) {
    vm_space_t* vm = (vm_space_t*)kmalloc(sizeof(vm_space_t));
    if (!vm) return NULL;

    // Новый PD с копией ядерных PDE (identity-map + LAPIC).
    page_directory_t* pd = paging_create_directory();
    if (!pd) {
        kfree(vm);
        return NULL;
    }

    vm->pd        = pd;
    vm->refcount  = 1;
    vm->mmap_top  = USER_MMAP_BASE;
    vm->is_kernel = 0;
    return vm;
}

vm_space_t* vm_clone_cow(vm_space_t* parent) {
    if (!parent || parent->is_kernel) return NULL;  // kernel-vm не клонируется

    vm_space_t* vm = (vm_space_t*)kmalloc(sizeof(vm_space_t));
    if (!vm) return NULL;

    page_directory_t* pd = paging_clone_user_cow(parent->pd);
    if (!pd) {
        kfree(vm);
        return NULL;
    }

    vm->pd        = pd;
    vm->refcount  = 1;
    vm->mmap_top  = parent->mmap_top;   // наследуем позицию mmap-арены
    vm->is_kernel = 0;
    return vm;
}

vm_space_t* vm_retain(vm_space_t* vm) {
    if (!vm) return NULL;
    vm->refcount++;
    return vm;
}

void vm_release(vm_space_t* vm) {
    if (!vm) return;
    if (vm->refcount <= 0) {
        // Неожиданный refcount — не роняем ядро, но сигналим.
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[VM] release called on already-zero refcount");
        return;
    }

    vm->refcount--;
    if (vm->refcount > 0) return;

    // Последний пользователь ушёл.
    if (vm->is_kernel) {
        // Ядерное vm никогда не освобождается; восстанавливаем refcount.
        vm->refcount = 1;
        return;
    }

    // Если освобождаемое пространство сейчас активно — уходим на ядро,
    // иначе CR3 указывает на таблицы, которые сейчас исчезнут.
    if (current_vm == vm) {
        vm_switch(&kernel_vm);
    }

    // Освободить пользовательские страницы и сам PD.
    paging_destroy_user_directory(vm->pd);

    kfree(vm);
}

void vm_switch(vm_space_t* vm) {
    if (!vm) vm = &kernel_vm;
    if (vm == current_vm) return;        // уже в этом пространстве — CR3 не трогаем

    paging_switch_directory(vm->pd);
    current_vm = vm;
}
