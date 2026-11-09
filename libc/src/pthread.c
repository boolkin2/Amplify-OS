#include "../include/pthread.h"
#include "../include/stdlib.h"
#include "syscalls.h"

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {
    (void)attr;
    void* stack = malloc(4096);
    if (!stack) return -1;
    
    int tid = _syscall3(SYS_CLONE, (uint32_t)start_routine, (uint32_t)stack + 4096, (uint32_t)arg);
    if (tid < 0) return -1;
    
    if (thread) *thread = tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    return _syscall2(SYS_JOIN, (uint32_t)thread, (uint32_t)retval);
}
