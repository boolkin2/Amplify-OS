// ============================================================================
// syscall.c — инициализация SYSCALL/SYSRET MSR и C-диспатчер.
// ============================================================================
#include "syscall.h"
#include "cpu.h"
#include "gdt.h"
#include "serial.h"
#include "fb.h"
#include "task.h"
#include "vfs.h"
#include "vm.h"
#include "timer.h"
#include "drm.h"     // этап 41: AOS_SYS_DRM_*
// [этап 46] sandboxing
#include "ns.h"
#include "cgroup.h"
#include "seccomp.h"
#include "box.h"
#include "heap.h"

// MSR-ы.
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_FMASK           0xC0000084

#define EFER_SCE            (1ULL << 0)

// FMASK: какие биты RFLAGS очищаются при SYSCALL.
//   bit 9  IF — выключаем прерывания во время syscall handler
//   bit 10 DF — System V ABI требует DF=0
#define FMASK_VALUE         ((1ULL << 9) | (1ULL << 10))

extern void syscall_entry(void);

// Признак "user программа вызвала exit" — kernel_main64 проверит это
// чтобы продолжить (на 37.4a у нас простейший callback вместо настоящего
// планировщика).
volatile int g_user_exited = 0;
volatile int g_user_exit_code = 0;

// Этап 40 группа B: номер последнего syscall — используется
// sig_check_and_deliver_user чтобы обнаружить SIGRETURN.
uint64_t g_last_syscall_num = 0;

// Глобалы, заданные в syscall_entry.asm (.bss).
// Объявление дублирует syscall.h, но без extern, потому что они тут не определены —
// определены в asm. Здесь мы только инициализируем kernel_rsp_save из C.

void syscall_init(uint64_t kernel_stack_top) {
    // 1. EFER.SCE = 1 — без этого SYSCALL вызовет #UD.
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // 2. STAR:
    //   bits [47:32] = kernel base (selector for kernel CS, kernel SS = +8)
    //   bits [63:48] = user base (sysret loads CS = +16, SS = +8 with RPL=3)
    uint64_t star = ((uint64_t)GDT_SEL_KERNEL_CODE << 32)
                  | ((uint64_t)GDT_SYSRET_BASE     << 48);
    wrmsr(MSR_STAR, star);

    // 3. LSTAR — точка входа syscall.
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);

    // 4. FMASK.
    wrmsr(MSR_FMASK, FMASK_VALUE);

    // 5. kernel_rsp_save для syscall_entry.asm.
    kernel_rsp_save = kernel_stack_top;

    serial_puts("[syscall] SYSCALL/SYSRET enabled. LSTAR=0x");
    serial_print_hex((uint64_t)(uintptr_t)&syscall_entry, 16);
    serial_puts(" STAR=0x");
    serial_print_hex(star, 16);
    serial_puts("\n");
}

// ============================================================================
// C-диспатчер syscall'ов
// ============================================================================
static uint64_t do_write(int fd, const char* buf, uint64_t n) {
    // На 37.4a поддерживаем только fd=1 (stdout) и fd=2 (stderr).
    // fb_console_putc сама дублирует в serial — вызываем только её,
    // иначе будет "HHeelllloo".
    if (fd != 1 && fd != 2) return (uint64_t)-1;
    for (uint64_t i = 0; i < n; i++) fb_console_putc(buf[i]);
    return n;
}

// На 37.4a do_exit halt'ил CPU — теперь у нас планировщик, делегируем task_exit
// (он переключится на следующую задачу или halt'нет если последняя).
static uint64_t do_exit(int code) {
    task_exit(code);    /* noreturn */
    return 0;           /* unreachable */
}

static uint64_t do_yield(void) {
    task_yield();
    return 0;
}

// [этап 46] seccomp checkpoint. Возвращает 0 если разрешено, иначе либо
// -EPERM, либо вызывает task_exit при KILL. Не должен вызываться для
// SYS_SIGRETURN (в seccomp_check этот номер хардкодом всегда ALLOW).
static uint64_t seccomp_gate(syscall_args_t* args) {
    task_t* t = task_current();
    if (!t || !t->seccomp) return 0;
    int r = seccomp_check(t->seccomp, (uint32_t)args->num);
    if (r == 0) return 0;
    if (r == -2) {
        serial_puts("[seccomp] KILL tid=");
        serial_print_dec((uint64_t)t->tid);
        serial_puts(" sysnum=0x");
        serial_print_hex(args->num, 4);
        serial_puts("\n");
        task_exit(137);   // SIGKILL exit-code, noreturn
    }
    return (uint64_t)(int64_t)(-AOS_EPERM);
}

uint64_t syscall_dispatch(syscall_args_t* args) {
    g_last_syscall_num = args->num;
    // [этап 46] seccomp проверяется ДО switch'а. Если профиль активен и
    // syscall не в whitelist — возвращаем -EPERM или kill task.
    {
        uint64_t blocked = seccomp_gate(args);
        if (blocked) return blocked;
    }
    switch (args->num) {
    case AOS_SYS_WRITE:  return do_write((int)args->a1, (const char*)args->a2, args->a3);
    case AOS_SYS_EXIT:   return do_exit((int)args->a1);
    case AOS_SYS_YIELD:  return do_yield();
    case AOS_SYS_PID: {
        task_t* t = task_current();
        // [этап 46] Возвращаем pid_ns-visible PID, если задача в child
        // pid_ns; иначе глобальный tid.
        if (!t) return 1;
        return (uint64_t)ns_visible_pid(t);
    }
    case AOS_SYS_UID: {
        task_t* t = task_current();
        // [этап 46] Возвращаем UID внутри user_ns (по умолчанию = 0/root).
        return (uint64_t)ns_visible_uid(t);
    }

    // ---- File-syscalls (этап 37.5a, через VFS) -----------------------------
    case AOS_SYS_OPEN: {
        // a1 = path (user-pointer), a2 = flags (этап 40: полный парсинг)
        const char* path = (const char*)args->a1;
        int flags = (int)args->a2;
        int fd = vfs_open_flags(path, flags);
        return (uint64_t)(int64_t)fd;
    }
    case AOS_SYS_READ: {
        // a1 = fd, a2 = buf (user-pointer), a3 = len
        int fd = (int)args->a1;
        void* buf = (void*)args->a2;
        uint64_t len = args->a3;
        int64_t got = vfs_read(fd, buf, len);
        return (uint64_t)got;
    }
    case AOS_SYS_CLOSE:
        return (uint64_t)(int64_t)vfs_close((int)args->a1);
    case AOS_SYS_SEEK: {
        // a1 = fd, a2 = offset, a3 = whence
        int fd = (int)args->a1;
        int64_t off = (int64_t)args->a2;
        int whence = (int)args->a3;
        return (uint64_t)vfs_seek(fd, off, whence);
    }

    // ---- Этап 40 группа A: stat, mkdir, dup, dup2, fcntl ------------------
    case AOS_SYS_STAT: {
        const char* path = (const char*)args->a1;
        vfs_stat_t* st   = (vfs_stat_t*)args->a2;
        return (uint64_t)(int64_t)vfs_stat(path, st);
    }
    case AOS_SYS_FSTAT: {
        int fd = (int)args->a1;
        vfs_stat_t* st = (vfs_stat_t*)args->a2;
        return (uint64_t)(int64_t)vfs_fstat(fd, st);
    }
    case AOS_SYS_MKDIR: {
        const char* path = (const char*)args->a1;
        uint32_t mode    = (uint32_t)args->a2;
        return (uint64_t)(int64_t)vfs_mkdir(path, mode);
    }
    case AOS_SYS_DUP:  return (uint64_t)(int64_t)vfs_dup((int)args->a1);
    case AOS_SYS_DUP2: return (uint64_t)(int64_t)vfs_dup2((int)args->a1, (int)args->a2);
    case AOS_SYS_FCNTL:
        return (uint64_t)(int64_t)vfs_fcntl((int)args->a1, (int)args->a2, (long)args->a3);

    // ---- Группы B/C/D (сигналы/сокеты/pthread) — номера зарезервированы,
    //      реализация в следующих итерациях этапа 40.
    case AOS_SYS_SIGACTION: {
        // a1 = signum, a2 = &act, a3 = &old
        extern int64_t sig_sys_sigaction(int, const void*, void*);
        return (uint64_t)sig_sys_sigaction(
            (int)args->a1, (const void*)args->a2, (void*)args->a3);
    }
    case AOS_SYS_KILL: {
        extern int64_t sig_sys_kill(int, int);
        return (uint64_t)sig_sys_kill((int)args->a1, (int)args->a2);
    }
    case AOS_SYS_SIGRETURN: {
        // sigreturn не возвращается нормально — он переопределяет return context.
        // Реализация внутри syscall_entry.asm (после вызова sig_check_and_deliver_user)
        // prenable: здесь мы возвращаем 0, и asm-entry уже пере-использует
        // user_rsp_save/return slots.
        extern int64_t sig_sys_sigreturn_from_dispatch(syscall_args_t* a);
        return (uint64_t)sig_sys_sigreturn_from_dispatch(args);
    }
    case AOS_SYS_SOCKET: {
        extern int64_t sock_sys_socket(int, int, int);
        return (uint64_t)sock_sys_socket((int)args->a1, (int)args->a2, (int)args->a3);
    }
    case AOS_SYS_BIND: {
        extern int64_t sock_sys_bind(int, const void*, int);
        return (uint64_t)sock_sys_bind((int)args->a1, (const void*)args->a2, (int)args->a3);
    }
    case AOS_SYS_LISTEN: {
        extern int64_t sock_sys_listen(int, int);
        return (uint64_t)sock_sys_listen((int)args->a1, (int)args->a2);
    }
    case AOS_SYS_ACCEPT: {
        extern int64_t sock_sys_accept(int, void*, int*);
        return (uint64_t)sock_sys_accept((int)args->a1, (void*)args->a2, (int*)args->a3);
    }
    case AOS_SYS_CONNECT: {
        extern int64_t sock_sys_connect(int, const void*, int);
        return (uint64_t)sock_sys_connect((int)args->a1, (const void*)args->a2, (int)args->a3);
    }
    case AOS_SYS_SEND: {
        extern int64_t sock_sys_send(int, const void*, uint64_t, int);
        return (uint64_t)sock_sys_send((int)args->a1, (const void*)args->a2,
                                       args->a3, (int)args->a4);
    }
    case AOS_SYS_RECV: {
        extern int64_t sock_sys_recv(int, void*, uint64_t, int);
        return (uint64_t)sock_sys_recv((int)args->a1, (void*)args->a2,
                                       args->a3, (int)args->a4);
    }
    case AOS_SYS_SENDTO: {
        extern int64_t sock_sys_sendto(int, const void*, uint64_t, int, const void*, int);
        return (uint64_t)sock_sys_sendto((int)args->a1, (const void*)args->a2,
                                         args->a3, (int)args->a4,
                                         (const void*)args->a5, (int)args->a6);
    }
    case AOS_SYS_RECVFROM: {
        extern int64_t sock_sys_recvfrom(int, void*, uint64_t, int, void*, int*);
        return (uint64_t)sock_sys_recvfrom((int)args->a1, (void*)args->a2,
                                           args->a3, (int)args->a4,
                                           (void*)args->a5, (int*)args->a6);
    }
    case AOS_SYS_SHUTDOWN: {
        extern int64_t sock_sys_shutdown(int, int);
        return (uint64_t)sock_sys_shutdown((int)args->a1, (int)args->a2);
    }
    case AOS_SYS_SETSOCKOPT: {
        extern int64_t sock_sys_setsockopt(int, int, int, const void*, int);
        return (uint64_t)sock_sys_setsockopt((int)args->a1, (int)args->a2,
                                             (int)args->a3, (const void*)args->a4,
                                             (int)args->a5);
    }
    case AOS_SYS_GETSOCKOPT: {
        extern int64_t sock_sys_getsockopt(int, int, int, void*, int*);
        return (uint64_t)sock_sys_getsockopt((int)args->a1, (int)args->a2,
                                             (int)args->a3, (void*)args->a4,
                                             (int*)args->a5);
    }

    case AOS_SYS_CLONE: {
        extern int64_t clone_sys_create(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        return (uint64_t)clone_sys_create(args->a1, args->a2, args->a3, args->a4, args->a5);
    }
    case AOS_SYS_FUTEX_WAIT: {
        extern int64_t futex_sys_wait(uint64_t, int, uint64_t);
        return (uint64_t)futex_sys_wait(args->a1, (int)args->a2, args->a3);
    }
    case AOS_SYS_FUTEX_WAKE: {
        extern int64_t futex_sys_wake(uint64_t, int);
        return (uint64_t)futex_sys_wake(args->a1, (int)args->a2);
    }
    case AOS_SYS_SET_TID: {
        extern int64_t set_tid_set(uint64_t);
        return (uint64_t)set_tid_set(args->a1);
    }
    case AOS_SYS_JOIN: {
        extern int64_t clone_sys_join(int, int*);
        return (uint64_t)clone_sys_join((int)args->a1, (int*)args->a2);
    }

    // ---- Этап 39 (musl-aos libc): brk/sbrk/now/sleep ----------------------
    // SBRK(delta): возвращает старый brk, расширяет кучу процесса. Эквивалент
    // POSIX sbrk. delta — знаковое 64-бит число (a1).
    case AOS_SYS_SBRK: {
        task_t* t = task_current();
        if (!t || !t->vm) return (uint64_t)-1;
        int64_t delta = (int64_t)args->a1;
        return vm_brk_grow(t->vm, delta);
    }
    // BRK(addr): устанавливает brk на addr.  Эквивалент Linux brk:
    //   возвращает новое значение brk (или старое, если addr невалиден).
    case AOS_SYS_BRK: {
        task_t* t = task_current();
        if (!t || !t->vm) return (uint64_t)-1;
        uint64_t addr = args->a1;
        if (addr == 0) {
            // brk(0) — Linux semantics: вернуть текущий brk.
            return vm_brk_grow(t->vm, 0);
        }
        uint64_t cur = vm_brk_grow(t->vm, 0);
        if (addr == cur) return cur;
        int64_t delta = (int64_t)(addr - cur);
        uint64_t r = vm_brk_grow(t->vm, delta);
        if (r == (uint64_t)-1) return cur;          // отказ → старый brk
        return vm_brk_grow(t->vm, 0);               // новый brk
    }

    // NOW(out_ms*): монотонное время в миллисекундах с момента старта.
    // Если a1 != 0 — пишем 64-bit значение по указателю и возвращаем 0;
    // если a1 == 0 — возвращаем младшие 32 бита в RAX.
    case AOS_SYS_NOW: {
        uint64_t now = timer_get_uptime_ms();
        if (args->a1) {
            *(uint64_t*)args->a1 = now;
            return 0;
        }
        return (uint64_t)(uint32_t)now;
    }

    // SLEEP(ns_lo, ns_hi): спать ns наносекунд (округление до 10 ms — PIT-tick).
    case AOS_SYS_SLEEP: {
        uint64_t ns = (args->a2 << 32) | (args->a1 & 0xFFFFFFFFULL);
        uint32_t ms = (uint32_t)((ns + 999999ULL) / 1000000ULL);
        if (ms == 0) ms = 1;
        timer_sleep(ms);
        return 0;
    }

    // ---- Этап 41 (GPU / DRM): syscalls 0x140..0x148 -----------------------
    // На этап 41 active 3 syscall'а: OPEN/GET_INFO/PRESENT — userspace может
    // только наблюдать состояние GPU. Остальные возвращают ENOSYS до этапа 44.
    case AOS_SYS_DRM_OPEN:
        return (uint64_t)drm_sys_open(args->a1, args->a2);
    case AOS_SYS_DRM_GET_INFO:
        return (uint64_t)drm_sys_get_info(args->a1);
    case AOS_SYS_DRM_PRESENT:
        return (uint64_t)drm_sys_present(args->a1, args->a2);
    case AOS_SYS_DRM_CREATE_DUMB:
        return (uint64_t)drm_sys_create_dumb(args->a1, args->a2);
    case AOS_SYS_DRM_MAP:
        return (uint64_t)drm_sys_map(args->a1);
    case AOS_SYS_DRM_ADDFB:
        return (uint64_t)drm_sys_addfb(args->a1, args->a2);
    case AOS_SYS_DRM_SETCRTC:
        return (uint64_t)drm_sys_setcrtc(args->a1, args->a2);
    case AOS_SYS_DRM_PAGEFLIP:
        return (uint64_t)drm_sys_pageflip(args->a1, args->a2);
    case AOS_SYS_DRM_DESTROY:
        return (uint64_t)drm_sys_destroy(args->a1);

    // ---- [этап 46] Sandboxes / namespaces / cgroups / seccomp -----------
    case AOS_SYS_UNSHARE: {
        task_t* t = task_current();
        if (!t) return (uint64_t)(int64_t)(-AOS_EINVAL);
        ns_set_t* base = t->ns ? t->ns : ns_root();
        ns_set_t* fresh = ns_unshare(base, args->a1);
        if (!fresh) return (uint64_t)(int64_t)(-AOS_ENOMEM);
        ns_release(t->ns);
        t->ns = fresh;
        ns_pid_assign(t);
        return 0;
    }
    case AOS_SYS_SETHOSTNAME: {
        const char* name = (const char*)args->a1;
        int len = (int)args->a2;
        task_t* t = task_current();
        if (!t || !name || len <= 0 || len >= 64) return (uint64_t)(int64_t)(-AOS_EINVAL);
        int r = ns_sethostname(t, name, len);
        return (uint64_t)(int64_t)(r == 0 ? 0 : -AOS_EPERM);
    }
    case AOS_SYS_GETHOSTNAME: {
        char* buf = (char*)args->a1;
        int len = (int)args->a2;
        if (!buf || len <= 0) return (uint64_t)(int64_t)(-AOS_EINVAL);
        const char* h = ns_hostname(task_current());
        int i = 0;
        for (; i < len - 1 && h[i]; i++) buf[i] = h[i];
        buf[i] = 0;
        return 0;
    }
    case AOS_SYS_SECCOMP_LOAD: {
        const char* prof = (const char*)args->a1;
        task_t* t = task_current();
        if (!t || !prof) return (uint64_t)(int64_t)(-AOS_EINVAL);
        seccomp_filter_t* f = seccomp_profile(prof);
        if (!f) return (uint64_t)(int64_t)(-AOS_ENOMEM);
        if (t->seccomp) seccomp_release(t->seccomp);
        t->seccomp = f;     // refcount = 1
        return 0;
    }
    case AOS_SYS_SECCOMP_ALLOW: {
        task_t* t = task_current();
        if (!t || !t->seccomp) return (uint64_t)(int64_t)(-AOS_EINVAL);
        return (uint64_t)(int64_t)seccomp_allow(t->seccomp, (uint32_t)args->a1);
    }
    case AOS_SYS_CGROUP_CREATE: {
        const char* name = (const char*)args->a1;
        if (!name) return (uint64_t)(int64_t)(-AOS_EINVAL);
        cgroup_t* c = cgroup_create(cgroup_root(), name);
        if (!c) return (uint64_t)(int64_t)(-AOS_ENOMEM);
        return (uint64_t)c->id;
    }
    case AOS_SYS_CGROUP_LIMIT: {
        // a1=id, a2=controller, a3=value, a4=period (для CPU)
        int id = (int)args->a1;
        int ctrl = (int)args->a2;
        uint64_t val = args->a3;
        uint64_t period = args->a4;
        cgroup_t* c = 0;
        for (int i = 0; i < cgroup_count(); i++) {
            cgroup_t* cc = cgroup_at(i);
            if (cc && cc->id == id) { c = cc; break; }
        }
        if (!c) return (uint64_t)(int64_t)(-AOS_ENOENT);
        switch (ctrl) {
        case AOS_CGCTRL_CPU_NS:    cgroup_set_limit_cpu(c, val, c->cpu_period_ns); break;
        case AOS_CGCTRL_CPU_PERIOD:cgroup_set_limit_cpu(c, c->cpu_max_ns, val);    break;
        case AOS_CGCTRL_MEM:       cgroup_set_limit_mem(c, val);                   break;
        case AOS_CGCTRL_IO:        cgroup_set_limit_io(c, val);                    break;
        case AOS_CGCTRL_PIDS:      cgroup_set_limit_pids(c, (int)val);             break;
        default: return (uint64_t)(int64_t)(-AOS_EINVAL);
        }
        (void)period;
        return 0;
    }
    case AOS_SYS_CGROUP_ATTACH: {
        int tid = (int)args->a1;
        int id  = (int)args->a2;
        task_t* t = task_find_by_tid(tid);
        if (!t) return (uint64_t)(int64_t)(-AOS_ENOENT);
        cgroup_t* c = 0;
        for (int i = 0; i < cgroup_count(); i++) {
            cgroup_t* cc = cgroup_at(i);
            if (cc && cc->id == id) { c = cc; break; }
        }
        if (!c) return (uint64_t)(int64_t)(-AOS_ENOENT);
        if (t->cgroup) {
            cgroup_charge_pids(t->cgroup, -1);
            cgroup_release(t->cgroup);
        }
        t->cgroup = cgroup_retain(c);
        cgroup_charge_pids(c, +1);
        return 0;
    }
    case AOS_SYS_CGROUP_STAT: {
        int id = (int)args->a1;
        // out: 6×uint64: cpu_total_ns, mem_current, mem_peak, io_current,
        //      pids_current, throttle_count
        uint64_t* out = (uint64_t*)args->a2;
        if (!out) return (uint64_t)(int64_t)(-AOS_EFAULT);
        cgroup_t* c = 0;
        for (int i = 0; i < cgroup_count(); i++) {
            cgroup_t* cc = cgroup_at(i);
            if (cc && cc->id == id) { c = cc; break; }
        }
        if (!c) return (uint64_t)(int64_t)(-AOS_ENOENT);
        out[0] = c->cpu_total_ns;
        out[1] = c->mem_current;
        out[2] = c->mem_peak;
        out[3] = c->io_current;
        out[4] = (uint64_t)c->pids_current;
        out[5] = c->cpu_throttles + c->mem_oom + c->io_throttles + c->pids_denied;
        return 0;
    }
    case AOS_SYS_NS_INFO: {
        ns_summary_t* out = (ns_summary_t*)args->a1;
        int max = (int)args->a2;
        if (!out || max <= 0) return (uint64_t)(int64_t)(-AOS_EINVAL);
        return (uint64_t)ns_get_summaries(out, max);
    }
    case AOS_SYS_BOX_RUN: {
        const box_spec_t* spec = (const box_spec_t*)args->a1;
        char* log = (char*)args->a2;
        int log_max = (int)args->a3;
        if (!spec) return (uint64_t)(int64_t)(-AOS_EINVAL);
        return (uint64_t)(int64_t)box_run(spec, log, log_max);
    }

    default:
        serial_puts("[syscall] unknown num=0x");
        serial_print_hex(args->num, 8);
        serial_puts("\n");
        return (uint64_t)-1;
    }
}
