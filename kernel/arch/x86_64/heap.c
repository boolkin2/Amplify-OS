// ============================================================================
// heap.c — реализация kmalloc/kfree для 64-битного AMPLIFY-OS.
// ============================================================================
#include "heap.h"
#include "serial.h"

static heap_block_t* first_block;
static heap_stats_t  stats;

static inline void* block_payload(heap_block_t* b) {
    return (void*)((uint8_t*)b + sizeof(heap_block_t));
}
static inline heap_block_t* payload_block(void* p) {
    return (heap_block_t*)((uint8_t*)p - sizeof(heap_block_t));
}

static void split_block(heap_block_t* b, uint64_t size) {
    if (b->size < size + sizeof(heap_block_t) + HEAP_MIN_BLOCK) return;

    heap_block_t* nb = (heap_block_t*)((uint8_t*)b + sizeof(heap_block_t) + size);
    nb->magic   = HEAP_MAGIC;
    nb->size    = b->size - size - sizeof(heap_block_t);
    nb->is_free = true;
    nb->next    = b->next;
    nb->prev    = b;

    if (b->next) b->next->prev = nb;
    b->next = nb;
    b->size = size;

    stats.block_count++;
    stats.free_blocks++;
}

static void merge_forward(heap_block_t* b) {
    while (b->next && b->next->is_free && b->next->magic == HEAP_MAGIC) {
        heap_block_t* n = b->next;
        b->size += sizeof(heap_block_t) + n->size;
        b->next  = n->next;
        if (n->next) n->next->prev = b;
        stats.block_count--;
        stats.free_blocks--;
    }
}

static heap_block_t* find_free(uint64_t size) {
    for (heap_block_t* c = first_block; c; c = c->next) {
        if (c->is_free && c->size >= size) return c;
    }
    return 0;
}

void heap_init(void) {
    first_block = (heap_block_t*)KHEAP_START;
    first_block->magic   = HEAP_MAGIC;
    first_block->size    = KHEAP_SIZE - sizeof(heap_block_t);
    first_block->is_free = true;
    first_block->next    = 0;
    first_block->prev    = 0;

    stats.total_size  = KHEAP_SIZE;
    stats.used_size   = 0;
    stats.free_size   = KHEAP_SIZE - sizeof(heap_block_t);
    stats.block_count = 1;
    stats.free_blocks = 1;
    stats.used_blocks = 0;

    serial_puts("[heap] initialized: ");
    {
        char buf[16]; int n=0; uint64_t v = KHEAP_SIZE/MB;
        if (v==0) buf[n++]='0';
        else { char tmp[16]; int t=0; while(v){tmp[t++]='0'+v%10;v/=10;} while(t--) buf[n++]=tmp[t]; }
        buf[n]=0; serial_puts(buf);
    }
    serial_puts(" MB @ 0x"); serial_print_hex(KHEAP_START, 8); serial_puts("\n");
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    // Выравниваем размер по 16 байт (стандарт SysV).
    size = (size + 15) & ~(size_t)15;

    heap_block_t* b = find_free(size);
    if (!b) {
        serial_puts("[heap] OOM: kmalloc("); serial_print_dec(size); serial_puts(")\n");
        return 0;
    }
    split_block(b, size);
    b->is_free = false;
    stats.used_size += b->size;
    stats.free_size -= b->size + sizeof(heap_block_t);
    stats.free_blocks--;
    stats.used_blocks++;
    return block_payload(b);
}

// Сентинель для kmalloc_aligned: записываем перед результатом 16 байт:
//   [-16] : ALIGN_SENTINEL
//   [-8]  : raw-указатель из kmalloc
// kfree, видя сентинель, восстанавливает raw-указатель и освобождает его.
#define ALIGN_SENTINEL  0xA1A1A1A1A1A1A1A1ULL

void* kmalloc_aligned(size_t size, size_t align) {
    if (align <= 16) return kmalloc(size);
    // Выделяем с запасом и выравниваем payload вверх. 16 байт перед итоговым
    // указателем зарезервированы под сентинель + raw.
    size_t pad = align - 1 + 16;
    void*  raw = kmalloc(size + pad);
    if (!raw) return 0;
    uintptr_t addr = ((uintptr_t)raw + 16 + (align - 1)) & ~((uintptr_t)align - 1);
    ((uint64_t*)addr)[-2] = ALIGN_SENTINEL;
    ((uint64_t*)addr)[-1] = (uint64_t)(uintptr_t)raw;
    return (void*)addr;
}

void* kcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void* p = kmalloc(total);
    if (!p) return 0;
    uint8_t* q = (uint8_t*)p;
    for (size_t i = 0; i < total; i++) q[i] = 0;
    return p;
}

void kfree(void* ptr) {
    if (!ptr) return;

    // Сначала проверяем, не aligned-allocation ли это — у него за 16 байт
    // до payload лежит ALIGN_SENTINEL, потом raw-указатель.
    if (((uint64_t*)ptr)[-2] == ALIGN_SENTINEL) {
        ptr = (void*)(uintptr_t)((uint64_t*)ptr)[-1];
    }

    heap_block_t* b = payload_block(ptr);
    if (b->magic != HEAP_MAGIC) {
        serial_puts("[heap] kfree: bad magic, double-free?\n");
        return;
    }
    if (b->is_free) {
        serial_puts("[heap] kfree: double-free\n");
        return;
    }
    b->is_free = true;
    stats.used_size -= b->size;
    stats.free_size += b->size + sizeof(heap_block_t);
    stats.used_blocks--;
    stats.free_blocks++;

    merge_forward(b);
    if (b->prev && b->prev->is_free) merge_forward(b->prev);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return 0; }
    heap_block_t* b = payload_block(ptr);
    if (b->magic != HEAP_MAGIC) return 0;
    if (b->size >= new_size) return ptr;

    void* np = kmalloc(new_size);
    if (!np) return 0;
    uint8_t* dst = (uint8_t*)np;
    uint8_t* src = (uint8_t*)ptr;
    for (uint64_t i = 0; i < b->size; i++) dst[i] = src[i];
    kfree(ptr);
    return np;
}

heap_stats_t heap_get_stats(void) { return stats; }

bool heap_check_integrity(void) {
    for (heap_block_t* c = first_block; c; c = c->next) {
        if (c->magic != HEAP_MAGIC) return false;
        if (c->next && c->next->prev != c) return false;
    }
    return true;
}
