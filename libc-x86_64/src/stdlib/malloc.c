// ============================================================================
// malloc.c — first-fit аллокатор поверх sbrk() (этап 39).
// ----------------------------------------------------------------------------
// Дизайн:
//   * Все блоки выровнены на 16 байт (SysV ABI требует stack-align, и
//     malloc должен это держать же).
//   * Каждый блок имеет 16-байтовый header { size, next, free, magic }.
//   * Free-list — двусвязный по полю `next`, добавление в HEAD.
//   * Growing — пакеты по MIN_GROW=64 KB, через sbrk(). Если sbrk вернул
//     не contiguous участок — создаём новый блок, иначе расширяем последний.
//   * Алгоритм: first-fit + split (если оставшийся хвост ≥ MIN_SPLIT).
//   * free() — присоединение к соседям (coalesce) через дальнейший проход.
//
// Это НЕ dlmalloc/jemalloc, но достаточно для serial-программ. Замена на
// более серьёзный аллокатор — этап 44/46 (когда понадобится Doom/Quake).
// ============================================================================
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define ALIGN          16
#define MIN_GROW       (64 * 1024)
#define MIN_SPLIT      64
#define MAGIC_USED     0xABCDEF42CAFEBABEull
#define MAGIC_FREE     0xDEADBEEFCAFEF00Dull

typedef struct block {
    size_t          size;          // размер payload'а БЕЗ header'а
    struct block*   next;          // следующий блок в общем списке (по росту brk)
    int             free;
    uint64_t        magic;
} block_t;

#define BLOCK_HDR_SZ   ((sizeof(block_t) + ALIGN - 1) & ~(ALIGN - 1))

static block_t* g_head = NULL;
static block_t* g_tail = NULL;

static size_t align_up(size_t v) {
    return (v + ALIGN - 1) & ~(ALIGN - 1);
}

// Запрашиваем у ядра ещё `need` байт через sbrk.  Возвращаем указатель на
// начало нового сегмента (block_t*), либо NULL.
static block_t* grow_heap(size_t need) {
    size_t total = align_up(need + BLOCK_HDR_SZ);
    if (total < MIN_GROW) total = MIN_GROW;

    void* old = sbrk((intptr_t)total);
    if (old == (void*)-1) return NULL;

    block_t* b = (block_t*)old;
    b->size  = total - BLOCK_HDR_SZ;
    b->next  = NULL;
    b->free  = 1;
    b->magic = MAGIC_FREE;

    if (!g_head) {
        g_head = g_tail = b;
    } else {
        g_tail->next = b;
        g_tail = b;
    }
    return b;
}

// Разделить блок на (used | tail).  tail возвращается в free-list.
static void split_block(block_t* b, size_t used_size) {
    if (b->size < used_size + BLOCK_HDR_SZ + MIN_SPLIT) return;  // не делим
    block_t* tail = (block_t*)((char*)b + BLOCK_HDR_SZ + used_size);
    tail->size  = b->size - used_size - BLOCK_HDR_SZ;
    tail->next  = b->next;
    tail->free  = 1;
    tail->magic = MAGIC_FREE;
    b->size = used_size;
    b->next = tail;
    if (g_tail == b) g_tail = tail;
}

void* malloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size);
    if (size < ALIGN) size = ALIGN;

    // 1) first-fit по существующему списку.
    for (block_t* b = g_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free  = 0;
            b->magic = MAGIC_USED;
            return (char*)b + BLOCK_HDR_SZ;
        }
    }

    // 2) Расширяем кучу.
    block_t* nb = grow_heap(size);
    if (!nb) return NULL;
    split_block(nb, size);
    nb->free  = 0;
    nb->magic = MAGIC_USED;
    return (char*)nb + BLOCK_HDR_SZ;
}

void free(void* p) {
    if (!p) return;
    block_t* b = (block_t*)((char*)p - BLOCK_HDR_SZ);
    if (b->magic != MAGIC_USED) {
        // Double-free или corruption — игнор (на 45-м включим abort).
        return;
    }
    b->free  = 1;
    b->magic = MAGIC_FREE;

    // Coalesce с соседом справа (быстрый случай).
    if (b->next && b->next->free) {
        b->size += BLOCK_HDR_SZ + b->next->size;
        b->next = b->next->next;
        if (!b->next) g_tail = b;
    }
    // Coalesce с соседом слева — O(n), линейный walk.  Это нечастый путь,
    // не критично для производительности на 39-м этапе.
    if (b != g_head) {
        block_t* prev = g_head;
        while (prev && prev->next != b) prev = prev->next;
        if (prev && prev->free) {
            prev->size += BLOCK_HDR_SZ + b->size;
            prev->next = b->next;
            if (!prev->next) g_tail = prev;
        }
    }
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;       // overflow
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (!p) return NULL;
    memset(p, 0, total);
    return p;
}

void* realloc(void* p, size_t new_size) {
    if (!p) return malloc(new_size);
    if (new_size == 0) { free(p); return NULL; }

    block_t* b = (block_t*)((char*)p - BLOCK_HDR_SZ);
    if (b->magic != MAGIC_USED) return NULL;

    size_t aligned = align_up(new_size);
    if (b->size >= aligned) {
        split_block(b, aligned);
        return p;
    }

    void* np = malloc(new_size);
    if (!np) return NULL;
    memcpy(np, p, b->size < new_size ? b->size : new_size);
    free(p);
    return np;
}
