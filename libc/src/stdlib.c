#include "../include/stdlib.h"
#include "syscalls.h"

void* malloc(size_t size) {
    return (void*)_syscall1(SYS_SBRK, (uint32_t)size);
}

void free(void* ptr) {
    // very dummy free, does nothing
    (void)ptr;
}

void exit(int status) {
    _syscall1(SYS_EXIT, (uint32_t)status);
    while (1);
}
