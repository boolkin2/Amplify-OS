// ============================================================================
// heap.h — kernel heap (kmalloc/kfree) для 64-битного AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Алгоритм: First-Fit с двусвязным списком блоков и magic-канарейками.
// Это та же логика, что в kernel/memory/heap.c, но size_t/указатели — 64-bit,
// и сама область heap живёт в KHEAP_START..KHEAP_END (см. memory.h).
// PMM знает про эту область как "reserved" — она целиком наша.
// ============================================================================
#ifndef AOS_X86_64_HEAP_H
#define AOS_X86_64_HEAP_H

#include "memory.h"

#define HEAP_MAGIC          0xAAFEBEEFDEADF00DULL
#define HEAP_MIN_BLOCK      32

typedef struct heap_block {
    uint64_t          magic;
    uint64_t          size;       // размер payload (без заголовка)
    bool              is_free;
    struct heap_block* next;
    struct heap_block* prev;
} heap_block_t;

typedef struct {
    uint64_t total_size;
    uint64_t used_size;
    uint64_t free_size;
    uint64_t block_count;
    uint64_t free_blocks;
    uint64_t used_blocks;
} heap_stats_t;

void  heap_init(void);
void* kmalloc(size_t size);
void* kmalloc_aligned(size_t size, size_t align);
void* kcalloc(size_t n, size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);
heap_stats_t heap_get_stats(void);
bool  heap_check_integrity(void);

#endif
