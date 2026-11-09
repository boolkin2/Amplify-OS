#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long off_t;
#endif

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *) -1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

#endif
