// ============================================================================
// pthread.h — POSIX threads. Этап 39: stubs. Реальная реализация в 45/46.
// ============================================================================
#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _PTHREAD_T_DEFINED
#define _PTHREAD_T_DEFINED
typedef unsigned long pthread_t;
#endif

typedef struct { int dummy; } pthread_attr_t;
// Этап 40 группа D: реальный pthread_mutex_t layout.
//   lock: 0=free, 1=locked-uncontended, 2=locked-contended (futex slow-path)
typedef struct {
    volatile int lock;
    int          owner_tid;
    int          waiters;
    int          reserved;
} pthread_mutex_t;
typedef struct { int dummy; }   pthread_mutexattr_t;
typedef struct { int dummy; }   pthread_cond_t;
typedef struct { int dummy; }   pthread_condattr_t;
typedef int                     pthread_key_t;
typedef int                     pthread_once_t;
typedef struct { int dummy; }   pthread_rwlock_t;
typedef struct { int dummy; }   pthread_rwlockattr_t;
typedef struct { int dummy; }   pthread_spinlock_t;
typedef struct { int dummy; }   pthread_barrier_t;
typedef struct { int dummy; }   pthread_barrierattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }
#define PTHREAD_ONCE_INIT         0
#define PTHREAD_CREATE_JOINABLE   0
#define PTHREAD_CREATE_DETACHED   1
#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2
#define PTHREAD_MUTEX_DEFAULT     PTHREAD_MUTEX_NORMAL
#define PTHREAD_PROCESS_PRIVATE   0
#define PTHREAD_PROCESS_SHARED    1

int pthread_create (pthread_t* th, const pthread_attr_t* attr,
                    void* (*entry)(void*), void* arg);
int pthread_join   (pthread_t th, void** retval);
int pthread_detach (pthread_t th);
pthread_t pthread_self(void);
void pthread_exit  (void* retval) __attribute__((noreturn));
int pthread_equal  (pthread_t a, pthread_t b);

int pthread_attr_init    (pthread_attr_t* a);
int pthread_attr_destroy (pthread_attr_t* a);
int pthread_attr_setstacksize(pthread_attr_t* a, size_t sz);
int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* sz);
int pthread_attr_setdetachstate(pthread_attr_t* a, int state);

int pthread_mutex_init   (pthread_mutex_t* m, const pthread_mutexattr_t* a);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock   (pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock (pthread_mutex_t* m);
int pthread_mutexattr_init   (pthread_mutexattr_t* a);
int pthread_mutexattr_destroy(pthread_mutexattr_t* a);
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t);

int pthread_cond_init   (pthread_cond_t* c, const pthread_condattr_t* a);
int pthread_cond_destroy(pthread_cond_t* c);
int pthread_cond_wait   (pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                          const struct timespec* abs);
int pthread_cond_signal (pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);

int pthread_once(pthread_once_t* once, void (*init)(void));

int pthread_key_create(pthread_key_t* k, void (*dtor)(void*));
int pthread_key_delete(pthread_key_t k);
void* pthread_getspecific(pthread_key_t k);
int pthread_setspecific (pthread_key_t k, const void* v);

#ifdef __cplusplus
}
#endif
#endif
