#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

#define SYS_EXIT                0
#define SYS_WRITE               4
#define SYS_READ                5
#define SYS_OPEN                6
#define SYS_CLOSE               7
#define SYS_GETPID              8
#define SYS_SLEEP               9
#define SYS_SBRK                10
#define SYS_CLONE               12
#define SYS_JOIN                13
#define SYS_MMAP                14
#define SYS_MUNMAP              15
#define SYS_SWAP_TEST           16

static inline uint32_t _syscall0(uint32_t num) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (num));
    return ret;
}

static inline uint32_t _syscall1(uint32_t num, uint32_t a1) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (num), "b" (a1));
    return ret;
}

static inline uint32_t _syscall2(uint32_t num, uint32_t a1, uint32_t a2) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (num), "b" (a1), "c" (a2));
    return ret;
}

static inline uint32_t _syscall3(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (num), "b" (a1), "c" (a2), "d" (a3));
    return ret;
}

static inline uint32_t _syscall5(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (num), "b" (a1), "c" (a2), "d" (a3), "S" (a4), "D" (a5));
    return ret;
}

#endif
