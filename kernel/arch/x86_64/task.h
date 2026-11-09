// ============================================================================
// task.h — задачи + round-robin планировщик для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 37.4c: cooperative — задачи переключаются ТОЛЬКО при `aos_yield`
//             или `aos_exit`. PIT-preemption — этап 37.4d (нужен SMP-safe
//             switch из IRQ-контекста через swapgs+per-CPU данные).
//
// Каждая task_t владеет:
//   * vm_space — собственный PML4 (изоляция через PML4[1])
//   * kernel-стек 16 KB — на нём выполняется syscall handler этой задачи
//   * saved_rsp — RSP в её kernel-стеке между context_switch'ами
// ============================================================================
#ifndef AOS_X86_64_TASK_H
#define AOS_X86_64_TASK_H

#include <stdint.h>
#include "vm.h"

// Forward declarations: эти типы определены в ns.h/cgroup.h/seccomp.h, но
// task_t содержит только указатели на них — поэтому include'ы там не нужны
// (избегаем циркулярной зависимости).
struct ns_set;
struct cgroup;
struct seccomp_filter;

typedef enum {
    TASK_NEW = 0,        // ещё не запускалась
    TASK_READY,          // готова, ждёт CPU
    TASK_RUNNING,        // сейчас на CPU
    TASK_DEAD            // завершилась, ждёт reap
} task_state_t;

typedef struct task {
    int            tid;                  // 1, 2, 3, ...
    task_state_t   state;
    vm_space_t*    vm;

    uint64_t       saved_rsp;            // RSP в kernel-стеке (между switch'ами)
    uint64_t       kernel_stack_phys;    // bottom kernel-стека (для cleanup)
    uint64_t       kernel_stack_top;     // top — туда syscall_entry грузит RSP

    uint64_t       user_entry;           // только для first-run
    uint64_t       user_rsp;             // только для first-run
    uint64_t       user_rdi;             // этап 40 группа D: RDI при first-run (pthread arg)
    uint64_t       fs_base;              // этап 40 группа D: TLS-block адрес (FS.base MSR)

    int            exit_code;

    // ---- Этап 40 группа B: signal state per-task ------------------------
    // sig_handlers[0] не используется (сигналы нумеруются с 1).
    // Тип: void (*)(int); 0 = SIG_DFL, 1 = SIG_IGN.
    uint64_t       sig_handlers[32];
    uint32_t       sig_pending;          // битовая маска pending signals
    uint32_t       sig_mask;             // битовая маска заблокированных
    uint64_t       sig_restorer;         // адрес sigreturn-трамполина в userspace

    // ---- [этап 44] FPU/SSE state per-task --------------------------------
    // Указатель на 512-байтовый 16-byte aligned FXSAVE-блок, отдельно
    // выделенный через kmalloc_aligned. Сохраняется в task_yield() и
    // восстанавливается при возврате на эту задачу. При первом запуске
    // инициализируется шаблоном (FNINIT + LDMXCSR(0x1F80)). Это позволяет
    // ядру и user-mode коду свободно пользоваться x87/SSE/SSE2 регистрами,
    // не опасаясь corruption между задачами.
    uint8_t*       fpu_state;

    // ---- [этап 46] Sandboxing ------------------------------------------
    // Все три указателя могут быть NULL — тогда задача в root-namespace,
    // root-cgroup и без seccomp-фильтра.  Установлены через box_run или
    // через aos_unshare/aos_seccomp_set syscalls. ns_pid_visible — pid,
    // который задача видит "внутри" своего pid_ns (для AOS_SYS_PID).
    struct ns_set*           ns;
    struct cgroup*           cgroup;
    struct seccomp_filter*   seccomp;
    int                      ns_pid_visible;
    uint64_t                 cpu_tick_last_ms;  // для cgroup_charge_cpu (наклонная)

    struct task*   next;                 // циркулярный round-robin список
} task_t;

// Создаёт задачу, регистрирует в планировщике (state=NEW). НЕ запускает.
task_t*  task_create_user(vm_space_t* vm, uint64_t entry, uint64_t user_rsp);

// Этап 40 группа D: thread-таск.  В отличие от task_create_user, делит
// vm_space с родителем (vm_retain'ится) и получает TLS + user RDI = arg.
// tls_block — адрес TLS-блока userspace (станет FS.base). 0 если не нужен.
task_t*  task_create_thread(vm_space_t* shared_vm, uint64_t entry,
                            uint64_t user_rsp, uint64_t tls_block,
                            uint64_t arg);

// Запускает планировщик: переключаемся на task_list (первое созданное задание).
// НЕ ВОЗВРАЩАЕТСЯ — kernel_main64 после task_start() мёртв.
void     task_start(void) __attribute__((noreturn));

// Cooperative yield: вызывается через AOS_SYS_YIELD из user-mode либо из
// kernel-кода. Если другая задача готова — переключаемся.
void     task_yield(void);

// Завершает текущую задачу. Удаляет из round-robin списка, переключается
// на следующую. Если задача последняя — halt.  __noreturn__.
void     task_exit(int code) __attribute__((noreturn));

// Текущая задача (NULL если планировщик ещё не стартовал).
task_t*  task_current(void);

// Этап 40 группа B: поиск задачи по tid.
task_t*  task_find_by_tid(int tid);

// Внутренний first-run trampoline (вызывается через context_switch ret).
// Не вызывать руками.
void     task_first_run(void);

// ----------------------------------------------------------------------------
// Итераторы — для GUI Task Manager / отладки. Возвращают NULL когда список
// исчерпан (циркулярный список обходится строго один раз).
// ----------------------------------------------------------------------------
task_t*  task_iterate(task_t* prev);
int      task_count(void);
const char* task_state_name(task_state_t s);

#endif
