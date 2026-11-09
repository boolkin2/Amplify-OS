// ============================================================================
// task.c — реализация round-robin планировщика.
// ============================================================================
#include "task.h"
#include "vm.h"
#include "pmm.h"
#include "heap.h"
#include "memory.h"
#include "user_mode.h"
#include "syscall.h"
#include "serial.h"
#include "io.h"
// [этап 46] Sandboxing
#include "ns.h"
#include "cgroup.h"
#include "seccomp.h"
#include "timer.h"

#define KSTACK_PAGES   4                       // 16 KB на kernel-стек
#define KSTACK_SIZE    (KSTACK_PAGES * PAGE_SIZE)

extern void context_switch(uint64_t* old_rsp_save, uint64_t new_rsp);

// ============================================================================
// [этап 44] FPU/SSE state per-task.
// ----------------------------------------------------------------------------
// Эталонный FXSAVE-блок: «чистый» FPU + SSE state, такой какой даёт FNINIT
// + LDMXCSR(0x1F80). Создаётся однократно в task_fpu_template_init() при
// первом обращении (lazy).  Используется при task_create_* как образец
// инициализации.
//
// Сохранение/восстановление обёрнуто в маленькие inline-помощники, чтобы
// task_yield/task_exit оставались читаемыми.
// ============================================================================
static uint8_t  g_fpu_template[512] __attribute__((aligned(16)));
static bool     g_fpu_template_inited = false;

// Сохранить текущий FPU/SSE state в 16-byte aligned buffer.
static inline void fpu_save(uint8_t* dst) {
    if (!dst) return;
    __asm__ __volatile__("fxsaveq (%0)" : : "r"(dst) : "memory");
}

// Восстановить FPU/SSE state из 16-byte aligned buffer.
static inline void fpu_restore(const uint8_t* src) {
    if (!src) return;
    __asm__ __volatile__("fxrstorq (%0)" : : "r"(src) : "memory");
}

// Инициализация эталонного блока. Делается лениво — мы можем дёргать
// task_create раньше, чем FPU полностью прогрет, поэтому ждём первого
// task_create.  FNINIT + LDMXCSR + FXSAVE дают «чистый» снимок.
static void task_fpu_template_init(void) {
    if (g_fpu_template_inited) return;
    uint32_t mxcsr = 0x1F80;
    __asm__ __volatile__(
        "fninit\n\t"
        "ldmxcsr %0\n\t"
        "fxsaveq (%1)\n\t"
        : : "m"(mxcsr), "r"(g_fpu_template) : "memory");
    g_fpu_template_inited = true;
}

// Скопировать эталон в свежевыделенный per-task блок.
static void fpu_state_init(uint8_t* dst) {
    if (!dst) return;
    task_fpu_template_init();
    for (int i = 0; i < 512; i++) dst[i] = g_fpu_template[i];
}

static task_t* current = 0;
static task_t* task_list = 0;                  // циркулярный
static int     next_tid = 1;

// ----------------------------------------------------------------------------
task_t* task_current(void) { return current; }

// Этап 40 группа B: поиск задачи по tid для sys_kill/join.
task_t* task_find_by_tid(int tid) {
    if (!task_list) return 0;
    task_t* t = task_list;
    do {
        if (t->tid == tid) return t;
        t = t->next;
    } while (t != task_list);
    return 0;
}

// Циркулярный обход списка задач. prev == NULL → возвращает первую задачу;
// затем каждый следующий вызов возвращает next, пока не упрёмся обратно в
// task_list (тогда — NULL).
task_t* task_iterate(task_t* prev) {
    if (!task_list) return 0;
    if (!prev) return task_list;
    task_t* n = prev->next;
    if (n == task_list) return 0;
    return n;
}

int task_count(void) {
    if (!task_list) return 0;
    int n = 0;
    task_t* t = task_list;
    do { n++; t = t->next; } while (t != task_list && n < 1024);
    return n;
}

const char* task_state_name(task_state_t s) {
    switch (s) {
        case TASK_NEW:     return "NEW";
        case TASK_READY:   return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_DEAD:    return "DEAD";
        default:           return "?";
    }
}

// ----------------------------------------------------------------------------
// Инициализация kernel-стека новой задачи под формат, который ожидает
// context_switch: 6 callee-saved регистров (любые значения), затем RIP =
// task_first_run. После switch'а context_switch попадёт в task_first_run,
// который выполнит vm_switch + enter_user_mode и впервые отправит задачу
// в Ring 3.
// ----------------------------------------------------------------------------
static uint64_t prepare_initial_stack(uint64_t kstack_top) {
    uint64_t* sp = (uint64_t*)(uintptr_t)kstack_top;
    *--sp = 0;                                  // padding (16-byte align)
    *--sp = (uint64_t)(uintptr_t)task_first_run;// ret target
    *--sp = 0;                                  // rbp
    *--sp = 0;                                  // rbx
    *--sp = 0;                                  // r12
    *--sp = 0;                                  // r13
    *--sp = 0;                                  // r14
    *--sp = 0;                                  // r15
    return (uint64_t)(uintptr_t)sp;
}

// ----------------------------------------------------------------------------
task_t* task_create_user(vm_space_t* vm, uint64_t entry, uint64_t user_rsp) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return 0;

    uint64_t kstack_phys = pmm_alloc_pages(KSTACK_PAGES);
    if (!kstack_phys) { kfree(t); return 0; }

    // [этап 44] FPU/SSE save area — 512 байт, 16-byte aligned.
    uint8_t* fpu = (uint8_t*)kmalloc_aligned(512, 16);
    if (!fpu) {
        pmm_free_pages(kstack_phys, KSTACK_PAGES);
        kfree(t);
        return 0;
    }
    fpu_state_init(fpu);

    uint64_t kstack_top = kstack_phys + KSTACK_SIZE;

    t->tid               = next_tid++;
    t->state             = TASK_NEW;
    t->vm                = vm;
    t->saved_rsp         = prepare_initial_stack(kstack_top);
    t->kernel_stack_phys = kstack_phys;
    t->kernel_stack_top  = kstack_top;
    t->user_entry        = entry;
    t->user_rsp          = user_rsp;
    t->user_rdi          = 0;
    t->fs_base           = 0;
    t->exit_code         = 0;
    // Этап 40 группа B: нулевая signal-таблица.
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;   /* SIG_DFL */
    t->sig_pending       = 0;
    t->sig_mask          = 0;
    t->sig_restorer      = 0;
    t->fpu_state         = fpu;
    // [этап 46] По умолчанию наследуем namespaces/cgroup/seccomp у текущей
    // задачи (если есть); если нет — root.  box_run после создания может
    // переустановить эти поля.
    {
        task_t* cur = current;
        ns_set_t* base_ns = cur && cur->ns ? cur->ns : ns_root();
        t->ns = ns_retain(base_ns);
        t->cgroup = cur && cur->cgroup ? cgroup_retain(cur->cgroup)
                                       : cgroup_retain(cgroup_root());
        t->seccomp = cur && cur->seccomp ? seccomp_retain(cur->seccomp) : 0;
        t->ns_pid_visible = 0;
        t->cpu_tick_last_ms = timer_get_uptime_ms();
        if (t->cgroup) cgroup_charge_pids(t->cgroup, +1);
    }
    t->next              = 0;

    // Insert into circular list.
    if (!task_list) {
        task_list = t;
        t->next   = t;
    } else {
        t->next = task_list->next;
        task_list->next = t;
    }

    serial_puts("[task] created tid="); serial_print_dec(t->tid);
    serial_puts(" entry=0x"); serial_print_hex(entry, 16);
    serial_puts(" user_rsp=0x"); serial_print_hex(user_rsp, 16);
    serial_puts("\n");
    return t;
}

// ----------------------------------------------------------------------------
// Этап 40 группа D: task_create_thread — новый поток, делит vm_space с
// родителем. Родительский vm НЕ копируется — используется тот же PML4,
// что даёт общий view памяти (= что нужно для pthread).
// ----------------------------------------------------------------------------
task_t* task_create_thread(vm_space_t* shared_vm, uint64_t entry,
                           uint64_t user_rsp, uint64_t tls_block, uint64_t arg) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) return 0;
    uint64_t kstack_phys = pmm_alloc_pages(KSTACK_PAGES);
    if (!kstack_phys) { kfree(t); return 0; }
    uint8_t* fpu = (uint8_t*)kmalloc_aligned(512, 16);
    if (!fpu) {
        pmm_free_pages(kstack_phys, KSTACK_PAGES);
        kfree(t);
        return 0;
    }
    fpu_state_init(fpu);
    uint64_t kstack_top = kstack_phys + KSTACK_SIZE;

    t->tid               = next_tid++;
    t->state             = TASK_NEW;
    t->vm                = vm_retain(shared_vm);     /* ++refcount */
    t->saved_rsp         = prepare_initial_stack(kstack_top);
    t->kernel_stack_phys = kstack_phys;
    t->kernel_stack_top  = kstack_top;
    t->user_entry        = entry;
    t->user_rsp          = user_rsp;
    t->user_rdi          = arg;
    t->fs_base           = tls_block;
    t->exit_code         = 0;
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;
    t->sig_pending       = 0;
    t->sig_mask          = 0;
    t->sig_restorer      = 0;
    t->fpu_state         = fpu;
    // [этап 46] Поток — копия родительского ns/cgroup/seccomp (потоки
    // ВСЕГДА разделяют namespace/cgroup/seccomp; это требование Linux).
    {
        task_t* cur = current;
        ns_set_t* base_ns = cur && cur->ns ? cur->ns : ns_root();
        t->ns = ns_retain(base_ns);
        t->cgroup = cur && cur->cgroup ? cgroup_retain(cur->cgroup)
                                       : cgroup_retain(cgroup_root());
        t->seccomp = cur && cur->seccomp ? seccomp_retain(cur->seccomp) : 0;
        t->ns_pid_visible = 0;
        t->cpu_tick_last_ms = timer_get_uptime_ms();
        if (t->cgroup) cgroup_charge_pids(t->cgroup, +1);
    }
    t->next              = 0;

    if (!task_list) {
        task_list = t;
        t->next   = t;
    } else {
        t->next = task_list->next;
        task_list->next = t;
    }
    serial_puts("[task] created THREAD tid="); serial_print_dec(t->tid);
    serial_puts(" entry=0x"); serial_print_hex(entry, 16);
    serial_puts(" rsp=0x"); serial_print_hex(user_rsp, 16);
    serial_puts(" fs=0x"); serial_print_hex(tls_block, 16);
    serial_puts("\n");
    return t;
}

// ----------------------------------------------------------------------------
// task_first_run — первая «строчка» каждой задачи. Вызывается через
// context_switch's ret, когда мы впервые переключились на её stack.
// ----------------------------------------------------------------------------
#define MSR_FS_BASE   0xC0000100
static inline void wrmsr_local(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

// Входная точка thread-task'а: передаёт arg через RDI.
extern void enter_user_mode_thread(uint64_t rip, uint64_t rsp, uint64_t rdi);

void task_first_run(void) {
    task_t* t = current;
    serial_puts("[task] first run tid="); serial_print_dec(t->tid); serial_puts("\n");
    vm_switch(t->vm);
    if (t->fs_base) {
        wrmsr_local(MSR_FS_BASE, t->fs_base);
    }
    if (t->user_rdi) {
        enter_user_mode_thread(t->user_entry, t->user_rsp, t->user_rdi);
    } else {
        enter_user_mode(t->user_entry, t->user_rsp);
    }
    /* unreachable */
}

// ----------------------------------------------------------------------------
void task_start(void) {
    if (!task_list) {
        serial_puts("[task] no tasks to start\n");
        for (;;) hlt();
    }
    current = task_list;
    current->state = TASK_RUNNING;
    kernel_rsp_save = current->kernel_stack_top;

    serial_puts("[task] scheduler starting, first tid=");
    serial_print_dec(current->tid); serial_puts("\n");

    uint64_t dummy_save;
    context_switch(&dummy_save, current->saved_rsp);
    /* unreachable */
    for (;;) hlt();
}

// ----------------------------------------------------------------------------
// Чтение FS.base (для сохранения при context switch). Используется ниже.
#define MSR_FS_BASE_READ 0xC0000100
static inline uint64_t rdmsr_local(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void task_yield(void) {
    if (!current) return;
    task_t* old = current;
    task_t* new_t = old->next;
    if (new_t == old) return;                  // одна задача — некуда переключать

    // [этап 46] CPU accounting в cgroup. Считаем время с прошлого yield'а.
    // Это аппроксимация — у нас кооперативный планировщик, фактическое CPU
    // time ≈ wall-clock между tick'ами для активной задачи. Достаточно
    // для лимитов «Пакет N CPU-секунд за период».
    {
        uint64_t now_ms = timer_get_uptime_ms();
        if (old->cpu_tick_last_ms && now_ms > old->cpu_tick_last_ms) {
            uint64_t dt_ns = (now_ms - old->cpu_tick_last_ms) * 1000000ULL;
            if (old->cgroup) cgroup_charge_cpu(old->cgroup, dt_ns);
        }
        cgroup_tick();   // даёт каждому cgroup'у шанс сбросить period budget
        new_t->cpu_tick_last_ms = now_ms;
    }

    // Сохраняем текущий FS.base (TLS) в old->fs_base.
    old->fs_base = rdmsr_local(MSR_FS_BASE_READ);

    // [этап 44] Сохраняем FPU/SSE state текущей задачи.
    fpu_save(old->fpu_state);

    old->state = TASK_READY;
    new_t->state = TASK_RUNNING;
    current = new_t;
    kernel_rsp_save = new_t->kernel_stack_top;

    // Если новая задача имеет FS.base (== thread) — ставим его. Для главного
    // процесса fs_base остаётся 0 и MSR не трогается (оставляем текущий
    // — если главный процесс никогда не ставил TLS, это 0).
    if (new_t->fs_base) {
        wrmsr_local(MSR_FS_BASE, new_t->fs_base);
    }

    // [этап 44] Восстанавливаем FPU/SSE state новой задачи.
    fpu_restore(new_t->fpu_state);

    context_switch(&old->saved_rsp, new_t->saved_rsp);
    // Возвращаемся сюда, когда кто-то yield'нул обратно в `old`.
    // Восстанавливаем наш FS.base + FPU state.
    if (current && current->fs_base) {
        wrmsr_local(MSR_FS_BASE, current->fs_base);
    }
    if (current) {
        fpu_restore(current->fpu_state);
    }
}

// ----------------------------------------------------------------------------
void task_exit(int code) {
    if (!current) {
        serial_puts("[task] exit without scheduler\n");
        for (;;) hlt();
    }
    task_t* old = current;
    old->state = TASK_DEAD;
    old->exit_code = code;

    serial_puts("[task] tid="); serial_print_dec(old->tid);
    serial_puts(" exited with code "); serial_print_dec((uint64_t)(int64_t)code);
    serial_puts("\n");

    task_t* new_t = old->next;
    if (new_t == old) {
        serial_puts("[task] last task done — halting\n");
        for (;;) hlt();
    }

    // Удаляем old из циркулярного списка.
    task_t* p = new_t;
    while (p->next != old) p = p->next;
    p->next = new_t;
    if (task_list == old) task_list = new_t;

    new_t->state = TASK_RUNNING;
    current = new_t;
    kernel_rsp_save = new_t->kernel_stack_top;

    // На 37.4c НЕ освобождаем old->vm и kernel-стек — там сейчас "плавающие"
    // syscall_entry frame'ы. Проще: оставляем утечку (≈20 KB на задачу).
    // В 37.4d/5 заведём reaper и освободим.
    // Этап 40 группа D: для thread (fs_base != 0) vm разделяется — только
    // декрементируем refcount; если это был последний поток, vm_destroy
    // освободит PML4/страницы.
    if (old->fs_base && old->vm) {
        vm_release(old->vm);
    }
    // [этап 44] FPU state можно освободить сразу — мёртвая задача его
    // больше не использует, а текущий контекст уже сохранил/восстановил.
    if (old->fpu_state) {
        kfree(old->fpu_state);
        old->fpu_state = 0;
    }
    // [этап 46] Освобождаем sandbox-state. cgroup_charge_pids -1 чтобы
    // pids_current корректно убавился. Refcount'ы вернутся в пул.
    if (old->cgroup) {
        cgroup_charge_pids(old->cgroup, -1);
        cgroup_release(old->cgroup);
        old->cgroup = 0;
    }
    if (old->ns) {
        if (old->ns->pid) ns_pid_release(old->ns->pid);
        ns_release(old->ns);
        old->ns = 0;
    }
    if (old->seccomp) {
        seccomp_release(old->seccomp);
        old->seccomp = 0;
    }
    (void)old;

    uint64_t dummy_save;
    context_switch(&dummy_save, new_t->saved_rsp);
    /* unreachable */
    for (;;) hlt();
}
