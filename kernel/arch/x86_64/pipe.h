// ============================================================================
// pipe.h — Unix-like pipe (FIFO) для AMPLIFY-OS x86_64.
// Порт kernel/ipc/pipe.h.  API идентичен — байтовый кольцевой буфер.
// ============================================================================
#ifndef AOS_X86_64_PIPE_H
#define AOS_X86_64_PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PIPE_DEFAULT_CAPACITY  (8 * 1024)

typedef struct pipe {
    uint8_t* buffer;
    uint64_t capacity;
    uint64_t head;
    uint64_t tail;
    uint64_t count;
    bool     write_closed;
} pipe_t;

pipe_t* pipe_create(uint64_t capacity);
void    pipe_destroy(pipe_t* p);
size_t  pipe_write(pipe_t* p, const void* buf, size_t len);
size_t  pipe_read(pipe_t* p, void* buf, size_t cap);
size_t  pipe_available(const pipe_t* p);
bool    pipe_is_full(const pipe_t* p);
void    pipe_close_write(pipe_t* p);
bool    pipe_eof(const pipe_t* p);

#endif
