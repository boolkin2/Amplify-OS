// ============================================================================
// mman.c — анонимный mmap поверх sbrk (этап 39). Файловый — TODO.
// ----------------------------------------------------------------------------
// На 45-м этапе (sandboxes / mmap-as-file) добавим прямой syscall в ядро.
// ============================================================================
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)prot; (void)off;
    if (fd != -1) { errno = ENOSYS; return MAP_FAILED; }
    if (!(flags & MAP_ANON)) { errno = EINVAL; return MAP_FAILED; }
    void* p = sbrk((intptr_t)len);
    if (p == (void*)-1) return MAP_FAILED;
    return p;
}

int munmap(void* addr, size_t len)              { (void)addr; (void)len; return 0; }
int mprotect(void* addr, size_t len, int prot)  { (void)addr; (void)len; (void)prot; return 0; }
int msync(void* addr, size_t len, int flags)    { (void)addr; (void)len; (void)flags; return 0; }
