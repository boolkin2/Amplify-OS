// ============================================================================
// spinlock.h — простейший test-and-set spinlock для 64-битного AMPLIFY-OS.
// Идентичен i386-версии: __sync_lock_test_and_set + pause.
// ============================================================================
#ifndef AOS_X86_64_SPINLOCK_H
#define AOS_X86_64_SPINLOCK_H

#include <stdint.h>

typedef struct { volatile uint32_t lock; } spinlock_t;

static inline void spinlock_init(spinlock_t* l) { l->lock = 0; }

static inline void spinlock_acquire(spinlock_t* l) {
    while (__sync_lock_test_and_set(&l->lock, 1)) {
        while (l->lock) __asm__ __volatile__("pause");
    }
}

static inline void spinlock_release(spinlock_t* l) {
    __sync_lock_release(&l->lock);
}

#endif
