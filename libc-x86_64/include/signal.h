#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int sig_atomic_t;

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGSYS   31
#define NSIG     32

typedef unsigned long sigset_t;

typedef void (*sighandler_t)(int);

struct sigaction {
    void   (*sa_handler)(int);
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};

sighandler_t signal  (int signum, sighandler_t handler);
int          sigaction(int signum, const struct sigaction* act, struct sigaction* old);
int          sigemptyset(sigset_t* set);
int          sigfillset(sigset_t* set);
int          sigaddset (sigset_t* set, int sig);
int          sigdelset (sigset_t* set, int sig);
int          sigismember(const sigset_t* set, int sig);
int          sigprocmask(int how, const sigset_t* set, sigset_t* old);
int          raise(int sig);
int          kill (pid_t pid, int sig);

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#ifdef __cplusplus
}
#endif
#endif
