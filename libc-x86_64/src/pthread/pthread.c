// ============================================================================
// pthread.c — POSIX threads для AMPLIFY-OS x86_64 (этап 40 группа D).
// ----------------------------------------------------------------------------
// Архитектура:
//   pthread_create:
//     1) malloc(PTHREAD_STACK_SIZE + TLS_SIZE) — единый блок.
//     2) верхняя часть = TLS (адрес для FS.base), нижняя = stack.
//     3) syscall CLONE(entry_trampoline, stack_top, tls_block, arg, 0).
//     4) entry_trampoline читает arg из RDI, вызывает user entry(arg),
//        при возврате — syscall EXIT.
//
//   pthread_mutex_lock (fast path):
//     CAS lock 0->1. Если 0 — success. Если занято — CAS 1->2 и FUTEX_WAIT.
//
//   pthread_mutex_unlock:
//     xchg lock, 0. Если prev == 2 — FUTEX_WAKE.
//
//   pthread_join:
//     AOS_SYS_JOIN(tid, &retval) — блокирует до завершения.
//
// Ограничения:
//   * pthread_cond_t реализован как тривиальный stub поверх мьютекса.
//   * sigaltstack не используется.
//   * Detached threads — реализованы как join-igoring (ядро не знает detach).
// ============================================================================
#include <pthread.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../syscall/aos_arch.h"

#define PTHREAD_STACK_SIZE  (64 * 1024)     /* 64 KB */
#define TLS_SIZE            (4 * 1024)      /*  4 KB */

// Внутренний thread-control-block, размещается в TLS-блоке потока.
// Первые байты TLS (FS:[0] в userspace) — обычно self-pointer на TCB.
typedef struct thread_tcb {
    struct thread_tcb* self;        // FS:[0] = self-pointer (стандарт musl)
    int                tid;         // kernel tid
    void*              retval;
    void*              stack_base;  // malloc'нутый блок (для free в конце)
    int                errno_val;   // per-thread errno (если будет __thread)
} thread_tcb_t;

// Trampoline для новых потоков:
//   RDI = arg (положен ядром при first-run через user_rdi).
//   RSI = entry (pthread_entry_t).
//   TCB доступен через FS:[0].
//
// Это функция на C, но она должна СРАЗУ читать entry — положим его в
// TCB перед clone'ом.
typedef void* (*pthread_entry_t)(void*);

static void thread_entry_trampoline(void* arg) {
    // TCB адрес в FS:[0]. Прочитаем через inline asm.
    thread_tcb_t* tcb;
    __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tcb));
    if (!tcb) {
        // TCB не установлен — аварийный exit.
        _exit(139);
    }
    pthread_entry_t entry = (pthread_entry_t)tcb->retval;   // мы кладём туда entry
    void* result = entry ? entry(arg) : NULL;
    tcb->retval = result;
    _exit(0);       // завершает эту задачу через AOS_SYS_EXIT
}

// ----------------------------------------------------------------------------
int pthread_create(pthread_t* th, const pthread_attr_t* attr,
                   void* (*entry)(void*), void* arg) {
    (void)attr;
    if (!th || !entry) return EINVAL;

    char* block = (char*)malloc(PTHREAD_STACK_SIZE + TLS_SIZE);
    if (!block) return EAGAIN;
    memset(block, 0, PTHREAD_STACK_SIZE + TLS_SIZE);

    uint64_t stack_bottom = (uint64_t)(uintptr_t)block;
    uint64_t stack_top    = stack_bottom + PTHREAD_STACK_SIZE;
    // Выравниваем stack_top на 16 (SysV ABI).
    stack_top &= ~15ULL;

    // TLS блок — после stack.
    thread_tcb_t* tcb = (thread_tcb_t*)(block + PTHREAD_STACK_SIZE);
    tcb->self       = tcb;
    tcb->tid        = 0;
    tcb->retval     = (void*)entry;        /* во время старта хранит entry */
    tcb->stack_base = block;
    tcb->errno_val  = 0;

    long tid = __aos_syscall5(AOS_SYS_CLONE,
                              (long)&thread_entry_trampoline,
                              (long)stack_top,
                              (long)tcb,
                              (long)arg,
                              0 /* flags */);
    if (tid < 0) {
        free(block);
        errno = (int)-tid;
        return EAGAIN;
    }
    tcb->tid = (int)tid;
    *th = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t th, void** retval) {
    int rc = 0;
    long r = __aos_syscall2(AOS_SYS_JOIN, (long)th, 0);
    if (r < 0) rc = (int)-r;
    if (retval) *retval = NULL;      /* retval поток хранит в своём TCB; на 40-м
                                         не извлекаем из закрытой задачи */
    return rc;
}

int pthread_detach(pthread_t th) { (void)th; return 0; }

pthread_t pthread_self(void) {
    thread_tcb_t* tcb;
    __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tcb));
    if (tcb) return (pthread_t)tcb->tid;
    return (pthread_t)getpid();
}

__attribute__((noreturn))
void pthread_exit(void* rv) {
    thread_tcb_t* tcb;
    __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tcb));
    if (tcb) tcb->retval = rv;
    _exit(0);
    for (;;);
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

// ----------------------------------------------------------------------------
// pthread_attr_*
// ----------------------------------------------------------------------------
int pthread_attr_init   (pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t* a, size_t s) { (void)a; (void)s; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* s) {
    (void)a; if (s) *s = PTHREAD_STACK_SIZE; return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t* a, int st) { (void)a; (void)st; return 0; }

// ----------------------------------------------------------------------------
// pthread_mutex_* — futex fast/slow path.
// ----------------------------------------------------------------------------
static inline int cas32(volatile int* p, int oldv, int newv) {
    return __atomic_compare_exchange_n(p, &oldv, newv, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    (void)a;
    if (!m) return EINVAL;
    m->lock      = 0;
    m->owner_tid = 0;
    m->waiters   = 0;
    m->reserved  = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    // Fast: CAS 0->1.
    if (cas32(&m->lock, 0, 1)) {
        m->owner_tid = (int)(long)pthread_self();
        return 0;
    }
    // Contended path.
    for (;;) {
        // CAS 1->2 (пометить contended), затем FUTEX_WAIT пока lock == 2.
        int cur = __atomic_load_n(&m->lock, __ATOMIC_ACQUIRE);
        if (cur == 0) {
            if (cas32(&m->lock, 0, 1)) {
                m->owner_tid = (int)(long)pthread_self();
                return 0;
            }
            continue;
        }
        if (cur == 1) {
            // Попытаемся перевести в contended.
            cas32(&m->lock, 1, 2);
        }
        // Ждём пока lock != 2.
        __aos_syscall3(AOS_SYS_FUTEX_WAIT, (long)&m->lock, 2, 0);
    }
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    if (cas32(&m->lock, 0, 1)) {
        m->owner_tid = (int)(long)pthread_self();
        return 0;
    }
    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    int prev = __atomic_exchange_n(&m->lock, 0, __ATOMIC_RELEASE);
    m->owner_tid = 0;
    if (prev == 2) {
        __aos_syscall2(AOS_SYS_FUTEX_WAKE, (long)&m->lock, 1);
    }
    return 0;
}

int pthread_mutexattr_init   (pthread_mutexattr_t* a) { (void)a; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t) { (void)a; (void)t; return 0; }

// ----------------------------------------------------------------------------
// pthread_cond_* — stub поверх мьютекса (на 40-м полный condvar не реализуем).
// ----------------------------------------------------------------------------
int pthread_cond_init    (pthread_cond_t* c, const pthread_condattr_t* a) { (void)c; (void)a; return 0; }
int pthread_cond_destroy (pthread_cond_t* c) { (void)c; return 0; }
int pthread_cond_wait    (pthread_cond_t* c, pthread_mutex_t* m) {
    (void)c;
    pthread_mutex_unlock(m);
    usleep(1);
    pthread_mutex_lock(m);
    return 0;
}
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* a) {
    (void)c; (void)a;
    pthread_mutex_unlock(m);
    usleep(1);
    pthread_mutex_lock(m);
    return 0;
}
int pthread_cond_signal  (pthread_cond_t* c) { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t* c) { (void)c; return 0; }

int pthread_once(pthread_once_t* once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    // Atomic CAS 0 -> 1: первый поток выполняет init.
    int zero = 0;
    if (__atomic_compare_exchange_n((int*)once, &zero, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        init();
        __atomic_store_n((int*)once, 2, __ATOMIC_RELEASE);
        return 0;
    }
    // Ждём пока первый поток закончит.
    while (__atomic_load_n((int*)once, __ATOMIC_ACQUIRE) != 2) {
        usleep(1);
    }
    return 0;
}

// TLS keys — те же что в 39-м, работают и с новыми потоками (глобально).
#define KEY_MAX 64
static void* g_tls[KEY_MAX];
static int   g_next_key = 0;

int   pthread_key_create(pthread_key_t* k, void (*dtor)(void*)) {
    (void)dtor;
    if (g_next_key >= KEY_MAX) return EAGAIN;
    if (k) *k = g_next_key++;
    return 0;
}
int   pthread_key_delete(pthread_key_t k) { (void)k; return 0; }
void* pthread_getspecific(pthread_key_t k) {
    return (k >= 0 && k < KEY_MAX) ? g_tls[k] : NULL;
}
int   pthread_setspecific(pthread_key_t k, const void* v) {
    if (k < 0 || k >= KEY_MAX) return EINVAL;
    g_tls[k] = (void*)v;
    return 0;
}
