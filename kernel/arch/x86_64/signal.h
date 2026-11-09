// ============================================================================
// signal.h — POSIX-like сигналы для AMPLIFY-OS x86_64.
// Порт kernel/ipc/signal.h.  Минимум: SIG_INT, SIG_KILL, флаги pending.
// ============================================================================
#ifndef AOS_X86_64_SIGNAL_H
#define AOS_X86_64_SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SIGNAL_MAX_TASKS     16
#define SIGNAL_TASK_NAME_LEN 32

#define SIG_INT  2
#define SIG_KILL 9

#define SIG_MASK_INT   (1u << SIG_INT)
#define SIG_MASK_KILL  (1u << SIG_KILL)

typedef struct {
    bool     used;
    int      pid;
    char     name[SIGNAL_TASK_NAME_LEN];
    uint32_t signals;
} signal_task_t;

void signal_init(void);

int  signal_register_task(const char* name);
void signal_unregister(int pid);
bool signal_send(int pid, int sig);
bool signal_pending(int pid, int sig);
void signal_clear(int pid, int sig);
int  signal_list(signal_task_t* out, int max);
const char* signal_task_name(int pid);

#endif
