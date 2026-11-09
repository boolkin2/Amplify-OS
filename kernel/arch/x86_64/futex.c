// ============================================================================
// futex.c — futex-like примитивы для AMPLIFY-OS x86_64 (этап 40 группа D).
// ----------------------------------------------------------------------------
// Минимальная реализация для pthread_mutex slow-path:
//   * AOS_SYS_FUTEX_WAIT(uaddr, expected, timeout_ns):
//       Проверяет *uaddr == expected атомарно. Если совпадает — блокирует
//       текущую задачу. Если нет — возвращает -AOS_EAGAIN.
//   * AOS_SYS_FUTEX_WAKE(uaddr, n):
//       Пробуждает n задач, ожидающих по uaddr. Возвращает количество
//       пробуждённых.
//
// Реализация:
//   * Хеш-таблица из 64 bucket'ов. Ключ = uaddr (xor с vm_space-ptr для
//     изоляции — разные процессы не мешают друг другу).
//   * Waiter = структура { task_t*, uaddr, next }. Выделяется в kernel-heap.
//   * Пробуждение = taskwait->state = TASK_READY + перемещение в round-robin
//     списке.
//
// Ограничения 40-го этапа:
//   * Нет priority inheritance.
//   * Нет FUTEX_REQUEUE.
//   * Таймауты не реализованы — futex_wait всегда бесконечен.
//   * Thundering herd: wake-all работает, но wake-N обслуживает только
//     первых N waiter'ов без учёта приоритета.
// ============================================================================
#include <stdint.h>
#include "task.h"
#include "heap.h"
#include "serial.h"
#include "syscall.h"

#define FUTEX_BUCKETS  64

typedef struct futex_waiter {
    task_t*               task;
    uint64_t              uaddr;
    struct futex_waiter*  next;
} futex_waiter_t;

static futex_waiter_t* g_buckets[FUTEX_BUCKETS];

static uint64_t futex_hash(uint64_t uaddr, void* vm) {
    uint64_t k = uaddr ^ ((uint64_t)vm >> 4);
    k ^= k >> 32;
    k *= 0x9E3779B97F4A7C15ULL;
    return k % FUTEX_BUCKETS;
}

// Простая проверка CPL=3 адреса — для безопасности.
static int user_addr_ok(uint64_t addr) {
    return addr >= 0x8000000000ULL;
}

// Блокирующее состояние (добавлено для futex); используется только здесь.
// task.c не знает о BLOCKED — мы эмулируем через удаление из ready-списка
// и прямой task_yield.  Упрощение: вместо настоящего BLOCKED просто делаем
// spin-yield с проверкой флага в цикле. Это не идеально (трата CPU), но
// работоспособно на 40-м с одним CPU и кооперативным шедулером.
// ----------------------------------------------------------------------------

int64_t futex_sys_wait(uint64_t uaddr, int expected, uint64_t timeout_ns) {
    (void)timeout_ns;
    if (!user_addr_ok(uaddr)) return -AOS_EFAULT;

    // Проверяем *uaddr == expected. Если нет — сразу возврат.
    int cur = *(volatile int*)(uintptr_t)uaddr;
    if (cur != expected) return -AOS_EAGAIN;

    task_t* t = task_current();
    if (!t) return -AOS_EPERM;

    // Регистрируем waiter.
    futex_waiter_t* w = (futex_waiter_t*)kmalloc(sizeof(*w));
    if (!w) return -AOS_ENOMEM;
    w->task  = t;
    w->uaddr = uaddr;
    uint64_t h = futex_hash(uaddr, (void*)t->vm);
    w->next      = g_buckets[h];
    g_buckets[h] = w;

    // Ждём: yield'имся в цикле, пока нас не удалят из bucket'а (сигнализация
    // из futex_sys_wake). На 40-м используем spin-yield — неэффективно, но
    // проще чем настоящее блокирующее состояние.
    for (;;) {
        // Проверяем, что задача всё ещё в bucket'е.
        futex_waiter_t* p;
        int still_in = 0;
        for (p = g_buckets[h]; p; p = p->next) {
            if (p == w) { still_in = 1; break; }
        }
        if (!still_in) break;
        task_yield();
    }
    kfree(w);
    return 0;
}

int64_t futex_sys_wake(uint64_t uaddr, int n) {
    task_t* t = task_current();
    if (!t) return -AOS_EPERM;
    uint64_t h = futex_hash(uaddr, (void*)t->vm);
    int woken = 0;
    futex_waiter_t** prev = &g_buckets[h];
    while (*prev && woken < n) {
        futex_waiter_t* w = *prev;
        if (w->uaddr == uaddr) {
            *prev = w->next;
            // Не освобождаем w здесь — оно освобождается в futex_sys_wait
            // после пробуждения. Просто удаляем из bucket'а.
            woken++;
        } else {
            prev = &(*prev)->next;
        }
    }
    return woken;
}

// ============================================================================
// clone (создание потока) — новый task_t с shared vm_space и pre-set TLS.
// args:
//   a1 = entry_pc
//   a2 = user_stack_top
//   a3 = tls_block
//   a4 = arg
//   a5 = flags (зарезервировано — пока все флаги подразумеваются)
// ============================================================================
int64_t clone_sys_create(uint64_t entry, uint64_t stack_top, uint64_t tls,
                         uint64_t arg, uint64_t flags) {
    (void)flags;
    task_t* parent = task_current();
    if (!parent || !parent->vm) return -AOS_EPERM;
    if (!user_addr_ok(entry) || !user_addr_ok(stack_top)) return -AOS_EFAULT;

    task_t* th = task_create_thread(parent->vm, entry, stack_top, tls, arg);
    if (!th) return -AOS_ENOMEM;
    return (int64_t)th->tid;
}

// ============================================================================
// join — ждать завершения задачи по tid.
// args: a1 = tid, a2 = &retval (или 0)
// ============================================================================
int64_t clone_sys_join(int tid, int* retval) {
    if (tid <= 0) return -AOS_EINVAL;
    for (;;) {
        task_t* t = task_find_by_tid(tid);
        if (!t) {
            // Задачи нет в списке — уже завершилась (удалена) либо никогда
            // не существовала. Вернём 0 для pthread-совместимости.
            if (retval && (uint64_t)retval >= 0x8000000000ULL) *retval = 0;
            return 0;
        }
        if (t->state == TASK_DEAD) {
            if (retval && (uint64_t)retval >= 0x8000000000ULL) *retval = t->exit_code;
            return 0;
        }
        task_yield();
    }
}

// ============================================================================
// set_tid — установить TLS-pointer текущей задачи (для updates после clone).
// ============================================================================
int64_t set_tid_set(uint64_t tls_block) {
    task_t* t = task_current();
    if (!t) return -AOS_EPERM;
    t->fs_base = tls_block;
    // Сразу применяем через wrmsr.
    #define MSR_FS_BASE_IN 0xC0000100
    uint32_t lo = (uint32_t)tls_block;
    uint32_t hi = (uint32_t)(tls_block >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(MSR_FS_BASE_IN), "a"(lo), "d"(hi));
    return 0;
}
