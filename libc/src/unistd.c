#include "../include/unistd.h"
#include "syscalls.h"

int open(const char *pathname, int flags) {
    return _syscall2(SYS_OPEN, (uint32_t)pathname, (uint32_t)flags);
}

int close(int fd) {
    return _syscall1(SYS_CLOSE, (uint32_t)fd);
}

int read(int fd, void *buf, size_t count) {
    return _syscall3(SYS_READ, (uint32_t)fd, (uint32_t)buf, (uint32_t)count);
}

int write(int fd, const void *buf, size_t count) {
    return _syscall3(SYS_WRITE, (uint32_t)fd, (uint32_t)buf, (uint32_t)count);
}

int getpid(void) {
    return _syscall0(SYS_GETPID);
}

void sleep(unsigned int milliseconds) { // MyOS sys_sleep uses ms, standard sleep uses seconds, but for test we pass ms or treat as ms
    _syscall1(SYS_SLEEP, milliseconds);
}
