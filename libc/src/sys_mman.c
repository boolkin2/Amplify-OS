#include "../include/sys/mman.h"
#include "syscalls.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    // Для базовой реализации offset игнорируется
    (void)offset;
    uint32_t ret = _syscall5(SYS_MMAP, (uint32_t)addr, length, prot, flags, fd);
    return (void*)ret;
}

int munmap(void *addr, size_t length) {
    return (int)_syscall2(SYS_MUNMAP, (uint32_t)addr, length);
}
