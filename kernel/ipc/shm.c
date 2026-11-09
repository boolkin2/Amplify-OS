// ============================================================================
// shm.c — реализация POSIX System V shared memory для MyOS
// ============================================================================
// Сегмент описывается в shm_segment_t и содержит массив физических страниц,
// которые мы держим "живыми" через pmm_inc_ref. При shmat мы маппим эти
// страницы в user-часть текущего vm (PTE_USER|PTE_WRITABLE) и фиксируем
// присоединение в attachments[] (чтобы shmdt мог найти их по vaddr+vm).
// При detach или удалении сегмента (IPC_RMID) страницы возвращаются в PMM
// через обычный pmm_free_page (refcount--).
// ============================================================================

#include "shm.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../memory/paging.h"
#include "../memory/pmm.h"
#include "../memory/vm.h"
#include "../task/task.h"

#define SHM_MAX_SEGMENTS    16
#define SHM_MAX_ATTACHES    64
#define SHM_MAX_PAGES       256    // 1 MiB на сегмент — разумный лимит

typedef struct {
    int      in_use;
    int      key;                  // Ключ (SHM_IPC_PRIVATE = 0)
    int      id;                   // Индекс = id (>=1; 0 зарезервирован)
    uint32_t size;                 // Размер в байтах (кратен PAGE_SIZE)
    uint32_t num_pages;
    uint32_t phys_pages[SHM_MAX_PAGES];  // Физические адреса страниц
    int      marked_for_deletion;  // IPC_RMID установил 1
    int      attach_count;         // Сколько процессов сейчас присоединены
} shm_segment_t;

typedef struct {
    int          in_use;
    int          seg_id;
    vm_space_t*  vm;               // В каком vm сегмент замаплен
    uint32_t     vaddr;            // Начальный user-VA
} shm_attach_t;

static shm_segment_t shm_segs[SHM_MAX_SEGMENTS];
static shm_attach_t  shm_atts[SHM_MAX_ATTACHES];

void shm_init(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) shm_segs[i].in_use = 0;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) shm_atts[i].in_use = 0;
}

// Поиск сегмента по id (id = индекс + 1, чтобы 0 оставался "invalid").
static shm_segment_t* find_by_id(int id) {
    if (id <= 0 || id > SHM_MAX_SEGMENTS) return 0;
    shm_segment_t* s = &shm_segs[id - 1];
    return s->in_use ? s : 0;
}

// Поиск по key (только для не-PRIVATE ключей).
static shm_segment_t* find_by_key(int key) {
    if (key == SHM_IPC_PRIVATE) return 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segs[i].in_use && shm_segs[i].key == key) return &shm_segs[i];
    }
    return 0;
}

// Выделить все страницы для сегмента; при ошибке — откат.
static int alloc_pages_for(shm_segment_t* s) {
    for (uint32_t i = 0; i < s->num_pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) {
            // Откат: освобождаем выделенное до этого.
            for (uint32_t j = 0; j < i; j++) pmm_free_page(s->phys_pages[j]);
            return -1;
        }
        // Обнуляем страницу (POSIX: shm содержит нули).
        uint32_t* p = (uint32_t*)phys;
        for (int k = 0; k < 1024; k++) p[k] = 0;
        s->phys_pages[i] = phys;
    }
    return 0;
}

// Освободить физические страницы сегмента.
static void free_pages_of(shm_segment_t* s) {
    for (uint32_t i = 0; i < s->num_pages; i++) {
        if (s->phys_pages[i]) {
            pmm_free_page(s->phys_pages[i]);
            s->phys_pages[i] = 0;
        }
    }
}

int shm_get(int key, uint32_t size, int flags) {
    if (size == 0) return -1;

    uint32_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages > SHM_MAX_PAGES) return -1;

    // Для не-приватных ключей сначала ищем существующий сегмент.
    if (key != SHM_IPC_PRIVATE) {
        shm_segment_t* existing = find_by_key(key);
        if (existing) {
            if (flags & SHM_IPC_EXCL) return -1;  // уже есть и запросили EXCL
            return existing->id;
        }
        if (!(flags & SHM_IPC_CREAT)) return -1;   // не существует и CREAT не задан
    }

    // Ищем свободный слот.
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_segs[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    shm_segment_t* s = &shm_segs[slot];
    s->in_use = 1;
    s->key = key;
    s->id = slot + 1;
    s->size = num_pages * PAGE_SIZE;
    s->num_pages = num_pages;
    s->marked_for_deletion = 0;
    s->attach_count = 0;

    if (alloc_pages_for(s) != 0) {
        s->in_use = 0;
        return -1;
    }

    return s->id;
}

uint32_t shm_attach(int id) {
    shm_segment_t* s = find_by_id(id);
    if (!s) return (uint32_t)-1;

    task_t* t = task_get_current();
    if (!t) return (uint32_t)-1;
    vm_space_t* vm = t->vm;
    if (!vm) return (uint32_t)-1;

    // Находим свободный слот в таблице присоединений.
    int aslot = -1;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) {
        if (!shm_atts[i].in_use) { aslot = i; break; }
    }
    if (aslot < 0) return (uint32_t)-1;

    // Выделяем VA-диапазон из mmap-арены процесса.
    uint32_t vaddr = vm->mmap_top;
    vm->mmap_top += s->num_pages * PAGE_SIZE;

    // Маппим физические страницы в user-PD.
    // Каждая страница уже «занята» со стороны PMM — при detach мы их освободим
    // через pmm_free_page. Для поддержки нескольких процессов, одновременно
    // присоединённых к одной shm, дополнительно инкрементируем refcount
    // (pmm_inc_ref) — тогда первый detach «отсоединит» нас, но не вернёт
    // страницы в пул, пока не уйдёт последний участник.
    page_directory_t* pd = paging_get_current_directory();
    for (uint32_t i = 0; i < s->num_pages; i++) {
        pmm_inc_ref(s->phys_pages[i]);
        paging_map_page(pd, vaddr + i * PAGE_SIZE, s->phys_pages[i],
                        PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    shm_atts[aslot].in_use = 1;
    shm_atts[aslot].seg_id = s->id;
    shm_atts[aslot].vm     = vm;
    shm_atts[aslot].vaddr  = vaddr;
    s->attach_count++;

    return vaddr;
}

int shm_detach(uint32_t vaddr) {
    task_t* t = task_get_current();
    if (!t) return -1;
    vm_space_t* vm = t->vm;

    // Ищем соответствующую запись в attachments.
    int aslot = -1;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) {
        if (shm_atts[i].in_use && shm_atts[i].vm == vm && shm_atts[i].vaddr == vaddr) {
            aslot = i; break;
        }
    }
    if (aslot < 0) return -1;

    shm_attach_t* a = &shm_atts[aslot];
    shm_segment_t* s = find_by_id(a->seg_id);
    if (!s) { a->in_use = 0; return -1; }

    // Отмаппим страницы и декрементируем PMM-refcount (pmm_free_page).
    page_directory_t* pd = paging_get_current_directory();
    for (uint32_t i = 0; i < s->num_pages; i++) {
        paging_unmap_page(pd, a->vaddr + i * PAGE_SIZE);
        pmm_free_page(s->phys_pages[i]);
    }

    a->in_use = 0;
    s->attach_count--;

    // Если сегмент помечен к удалению и это был последний участник —
    // реально освобождаем структуру и сами страницы (refcount PMM уже
    // обнулится pmm_free_page-ами выше).
    if (s->marked_for_deletion && s->attach_count == 0) {
        // Сами страницы уже отпущены pmm_free_page выше, здесь только
        // обнуляем запись.
        for (uint32_t i = 0; i < s->num_pages; i++) s->phys_pages[i] = 0;
        s->in_use = 0;
    }

    return 0;
}

int shm_ctl(int id, int cmd) {
    shm_segment_t* s = find_by_id(id);
    if (!s) return -1;

    if (cmd == SHM_IPC_RMID) {
        s->marked_for_deletion = 1;
        // Если никто не присоединён — можно сразу освободить.
        if (s->attach_count == 0) {
            free_pages_of(s);
            s->in_use = 0;
        }
        return 0;
    }

    return -1;
}
