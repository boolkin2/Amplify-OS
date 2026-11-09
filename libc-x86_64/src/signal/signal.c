// ============================================================================
// signal.c — POSIX signals для AMPLIFY-OS x86_64 (этап 40 группа B).
// ----------------------------------------------------------------------------
// signal() — тонкая обёртка над sigaction().
// sigaction() — реальный вызов в ядро через AOS_SYS_SIGACTION.
// Передаём adress trampoline'а (__aos_sigreturn_tramp из sigreturn.S),
// чтобы kernel знал, куда перенаправить handler-return.
// kill() — AOS_SYS_KILL.
// raise() — self-kill через getpid().
// sigset_t операции — локальные битовые.
// ============================================================================
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include "../syscall/aos_arch.h"

// Трамполин из sigreturn.S
extern void __aos_sigreturn_tramp(void);

// Userspace-копия handlers для sigreturn-trampoline'а.
// Обращается __aos_sigreturn_tramp через `lea rax, [rip+__aos_user_handlers]`.
__attribute__((visibility("default")))
void (*__aos_user_handlers[32])(int) = { 0 };

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction act  = { .sa_handler = handler };
    struct sigaction oact = { 0 };
    if (sigaction(signum, &act, &oact) < 0) return SIG_ERR;
    return oact.sa_handler;
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* old) {
    if (signum <= 0 || signum >= 32) { errno = EINVAL; return -1; }

    // Локальная структура — зеркало user_sigaction_t ядра.
    struct {
        unsigned long handler;
        unsigned long mask;
        int           flags;
        int           pad;
        unsigned long restorer;
    } k_act, k_old;

    const void* act_ptr = NULL;
    void* old_ptr = NULL;
    if (act) {
        k_act.handler  = (unsigned long)act->sa_handler;
        k_act.mask     = act->sa_mask;
        k_act.flags    = act->sa_flags;
        k_act.pad      = 0;
        k_act.restorer = (unsigned long)&__aos_sigreturn_tramp;
        act_ptr = &k_act;
    }
    if (old) old_ptr = &k_old;

    long r = __aos_syscall3(AOS_SYS_SIGACTION, signum,
                            (long)act_ptr, (long)old_ptr);
    long rc = __aos_set_errno(r);
    if (rc < 0) return -1;

    // Успех — обновляем userspace-таблицу для sigreturn-trampoline'а.
    if (act) __aos_user_handlers[signum] = act->sa_handler;

    if (old) {
        old->sa_handler  = (void (*)(int))k_old.handler;
        old->sa_mask     = k_old.mask;
        old->sa_flags    = k_old.flags;
        old->sa_restorer = (void (*)(void))k_old.restorer;
    }
    return 0;
}

int sigemptyset(sigset_t* s) { if (s) *s = 0; return 0; }
int sigfillset (sigset_t* s) { if (s) *s = (sigset_t)-1; return 0; }
int sigaddset  (sigset_t* s, int n) { if (s) *s |= (1UL << n); return 0; }
int sigdelset  (sigset_t* s, int n) { if (s) *s &= ~(1UL << n); return 0; }
int sigismember(const sigset_t* s, int n) { return s ? ((*s >> n) & 1) : 0; }

int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
    (void)how; (void)set; (void)old;
    return 0;    // stub — sigmask не влияет на доставку в ядре этапа 40
}

int raise(int sig) { return kill(getpid(), sig); }

int kill(pid_t pid, int sig) {
    long r = __aos_syscall2(AOS_SYS_KILL, (long)pid, (long)sig);
    return (int)__aos_set_errno(r);
}
