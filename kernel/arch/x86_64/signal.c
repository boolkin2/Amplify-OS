// ============================================================================
// signal.c — реализация POSIX-like сигналов.
// ============================================================================
#include "signal.h"

static signal_task_t g_tasks[SIGNAL_MAX_TASKS];
static int           g_next_pid = 1;

static void s_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static signal_task_t* find_by_pid(int pid) {
    for (int i = 0; i < SIGNAL_MAX_TASKS; i++) {
        if (g_tasks[i].used && g_tasks[i].pid == pid) return &g_tasks[i];
    }
    return 0;
}

void signal_init(void) {
    for (int i = 0; i < SIGNAL_MAX_TASKS; i++) {
        g_tasks[i].used = false;
        g_tasks[i].pid = 0;
        g_tasks[i].signals = 0;
        g_tasks[i].name[0] = '\0';
    }
    g_next_pid = 1;
}

int signal_register_task(const char* name) {
    for (int i = 0; i < SIGNAL_MAX_TASKS; i++) {
        if (!g_tasks[i].used) {
            g_tasks[i].used    = true;
            g_tasks[i].pid     = g_next_pid++;
            g_tasks[i].signals = 0;
            s_strncpy(g_tasks[i].name, name ? name : "task", SIGNAL_TASK_NAME_LEN);
            return g_tasks[i].pid;
        }
    }
    return -1;
}

void signal_unregister(int pid) {
    signal_task_t* t = find_by_pid(pid);
    if (!t) return;
    t->used = false;
    t->pid  = 0;
    t->signals = 0;
    t->name[0] = '\0';
}

bool signal_send(int pid, int sig) {
    signal_task_t* t = find_by_pid(pid);
    if (!t) return false;
    if (sig <= 0 || sig >= 32) return false;
    t->signals |= (1u << sig);
    return true;
}

bool signal_pending(int pid, int sig) {
    signal_task_t* t = find_by_pid(pid);
    if (!t) return false;
    return (t->signals & (1u << sig)) != 0;
}

void signal_clear(int pid, int sig) {
    signal_task_t* t = find_by_pid(pid);
    if (!t) return;
    if (sig == SIG_KILL) return;       // SIG_KILL нельзя сбросить
    t->signals &= ~(1u << sig);
}

int signal_list(signal_task_t* out, int max) {
    int n = 0;
    for (int i = 0; i < SIGNAL_MAX_TASKS && n < max; i++) {
        if (g_tasks[i].used) out[n++] = g_tasks[i];
    }
    return n;
}

const char* signal_task_name(int pid) {
    signal_task_t* t = find_by_pid(pid);
    return t ? t->name : 0;
}
