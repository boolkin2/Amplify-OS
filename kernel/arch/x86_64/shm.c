// ============================================================================
// shm.c — реализация POSIX shared memory.
// ============================================================================
#include "shm.h"
#include "memory.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"

#define SHM_MAX_SEGMENTS    16
#define SHM_MAX_ATTACHES    32
#define SHM_MAX_PAGES       256        // 1 MB на сегмент

// Старт VA-arena для shm-attach. Лежит в PML4[1] (per-process user space),
// после диапазона ELF (0x80_0000_0000) и пользовательского стека/heap.
#define SHM_VA_BASE         0x0000009000000000ULL

typedef struct {
    int      in_use;
    int      key;
    int      id;                   // = index + 1
    uint64_t size;
    uint64_t num_pages;
    uint64_t phys_pages[SHM_MAX_PAGES];
    int      marked_for_deletion;
    int      attach_count;
} shm_segment_t;

typedef struct {
    int          in_use;
    int          seg_id;
    vm_space_t*  vm;
    uint64_t     vaddr;
} shm_attach_t;

static shm_segment_t shm_segs[SHM_MAX_SEGMENTS];
static shm_attach_t  shm_atts[SHM_MAX_ATTACHES];

// Просто счётчик-арена для следующего attach. Каждый attach занимает
// num_pages × 4 KB. После detach слот не реализован для повторного
// использования — на учебной ОС не критично, MMAP-арены 100s of GB.
static uint64_t shm_va_next = SHM_VA_BASE;

void shm_init(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) shm_segs[i].in_use = 0;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) shm_atts[i].in_use = 0;
    shm_va_next = SHM_VA_BASE;
    serial_puts("[shm] initialized\n");
}

static shm_segment_t* find_by_id(int id) {
    if (id <= 0 || id > SHM_MAX_SEGMENTS) return 0;
    shm_segment_t* s = &shm_segs[id - 1];
    return s->in_use ? s : 0;
}

static shm_segment_t* find_by_key(int key) {
    if (key == SHM_IPC_PRIVATE) return 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (shm_segs[i].in_use && shm_segs[i].key == key) return &shm_segs[i];
    return 0;
}

static int alloc_pages_for(shm_segment_t* s) {
    for (uint64_t i = 0; i < s->num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            for (uint64_t j = 0; j < i; j++) pmm_free_page(s->phys_pages[j]);
            return -1;
        }
        // Зануляем (POSIX: shm возвращает нули).
        uint64_t* p = (uint64_t*)(uintptr_t)phys;
        for (int k = 0; k < 512; k++) p[k] = 0;
        s->phys_pages[i] = phys;
    }
    return 0;
}

static void free_pages_of(shm_segment_t* s) {
    for (uint64_t i = 0; i < s->num_pages; i++) {
        if (s->phys_pages[i]) {
            pmm_free_page(s->phys_pages[i]);
            s->phys_pages[i] = 0;
        }
    }
}

int shm_get(int key, uint64_t size, int flags) {
    if (size == 0) return -1;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages > SHM_MAX_PAGES) return -1;

    if (key != SHM_IPC_PRIVATE) {
        shm_segment_t* existing = find_by_key(key);
        if (existing) {
            if (flags & SHM_IPC_EXCL) return -1;
            return existing->id;
        }
        if (!(flags & SHM_IPC_CREAT)) return -1;
    }

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

    if (alloc_pages_for(s) != 0) { s->in_use = 0; return -1; }
    return s->id;
}

uint64_t shm_attach(vm_space_t* vm, int id) {
    shm_segment_t* s = find_by_id(id);
    if (!s || !vm) return (uint64_t)-1;

    int aslot = -1;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) if (!shm_atts[i].in_use) { aslot = i; break; }
    if (aslot < 0) return (uint64_t)-1;

    uint64_t vaddr = shm_va_next;
    shm_va_next += s->num_pages * PAGE_SIZE;

    // Маппим все страницы как user-RW.
    for (uint64_t i = 0; i < s->num_pages; i++) {
        if (!vm_map_user_page(vm, vaddr + i * PAGE_SIZE, s->phys_pages[i], true)) {
            // Откат
            for (uint64_t j = 0; j < i; j++) {
                paging_unmap_page_in(vm->pml4, vaddr + j * PAGE_SIZE);
            }
            return (uint64_t)-1;
        }
        // Refcount: каждая активная страница shm должна оставаться живой
        // даже если pmm_free_page будет вызван по другим путям.
        pmm_inc_ref(s->phys_pages[i]);
    }

    shm_atts[aslot].in_use = 1;
    shm_atts[aslot].seg_id = id;
    shm_atts[aslot].vm     = vm;
    shm_atts[aslot].vaddr  = vaddr;
    s->attach_count++;
    return vaddr;
}

int shm_detach(vm_space_t* vm, uint64_t vaddr) {
    if (!vm) return -1;
    for (int i = 0; i < SHM_MAX_ATTACHES; i++) {
        if (!shm_atts[i].in_use) continue;
        if (shm_atts[i].vm != vm) continue;
        if (shm_atts[i].vaddr != vaddr) continue;

        shm_segment_t* s = find_by_id(shm_atts[i].seg_id);
        if (!s) { shm_atts[i].in_use = 0; return 0; }

        for (uint64_t k = 0; k < s->num_pages; k++) {
            paging_unmap_page_in(vm->pml4, vaddr + k * PAGE_SIZE);
            pmm_free_page(s->phys_pages[k]);  // refcount--
        }

        shm_atts[i].in_use = 0;
        s->attach_count--;
        if (s->marked_for_deletion && s->attach_count == 0) {
            free_pages_of(s);
            s->in_use = 0;
        }
        return 0;
    }
    return -1;
}

int shm_ctl(int id, int cmd) {
    shm_segment_t* s = find_by_id(id);
    if (!s) return -1;
    if (cmd == SHM_IPC_RMID) {
        s->marked_for_deletion = 1;
        if (s->attach_count == 0) {
            free_pages_of(s);
            s->in_use = 0;
        }
        return 0;
    }
    return -1;
}
