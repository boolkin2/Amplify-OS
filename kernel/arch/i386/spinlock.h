#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    lock->lock = 0;
}

static inline void spinlock_acquire(spinlock_t* lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        while (lock->lock) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void spinlock_release(spinlock_t* lock) {
    __sync_lock_release(&lock->lock);
}

#endif // SPINLOCK_H
