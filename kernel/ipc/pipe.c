// ============================================================================
// pipe.c — Реализация pipe
// ============================================================================

#include "pipe.h"
#include "../memory/heap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

pipe_t* pipe_create(uint32_t capacity) {
    if (capacity == 0) capacity = PIPE_DEFAULT_CAPACITY;

    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return NULL;

    p->buffer = (uint8_t*)kmalloc(capacity);
    if (!p->buffer) { kfree(p); return NULL; }

    p->capacity     = capacity;
    p->head         = 0;
    p->tail         = 0;
    p->count        = 0;
    p->write_closed = false;
    return p;
}

void pipe_destroy(pipe_t* p) {
    if (!p) return;
    if (p->buffer) kfree(p->buffer);
    kfree(p);
}

size_t pipe_write(pipe_t* p, const void* buf, size_t len) {
    if (!p || !buf || p->write_closed) return 0;
    const uint8_t* src = (const uint8_t*)buf;

    // Сколько свободного места
    uint32_t free_space = p->capacity - p->count;
    size_t to_write = (len < free_space) ? len : free_space;

    for (size_t i = 0; i < to_write; i++) {
        p->buffer[p->tail] = src[i];
        p->tail = (p->tail + 1) % p->capacity;
    }
    p->count += (uint32_t)to_write;
    return to_write;
}

size_t pipe_read(pipe_t* p, void* buf, size_t cap) {
    if (!p || !buf) return 0;
    uint8_t* dst = (uint8_t*)buf;

    size_t to_read = (cap < p->count) ? cap : p->count;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buffer[p->head];
        p->head = (p->head + 1) % p->capacity;
    }
    p->count -= (uint32_t)to_read;
    return to_read;
}

size_t pipe_available(const pipe_t* p) {
    return p ? (size_t)p->count : 0;
}

bool pipe_is_full(const pipe_t* p) {
    return p && (p->count == p->capacity);
}

void pipe_close_write(pipe_t* p) {
    if (p) p->write_closed = true;
}

bool pipe_eof(const pipe_t* p) {
    return p && p->write_closed && p->count == 0;
}
