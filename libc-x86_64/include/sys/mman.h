#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE    0
#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_EXEC    4
#define PROT_GROWSDOWN 0x01000000

#define MAP_SHARED       0x01
#define MAP_PRIVATE      0x02
#define MAP_FIXED        0x10
#define MAP_ANON         0x20
#define MAP_ANONYMOUS    MAP_ANON
#define MAP_FAILED       ((void*)-1)

void* mmap   (void* addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap (void* addr, size_t len);
int   mprotect(void* addr, size_t len, int prot);
int   msync  (void* addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif
#endif
