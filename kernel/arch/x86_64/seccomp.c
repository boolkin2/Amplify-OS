// ============================================================================
// seccomp.c — реализация фильтров системных вызовов (этап 46).
// ----------------------------------------------------------------------------
// Полное соответствие seccomp.h. Поведение:
//   * seccomp_check вызывается из syscall_dispatch ДО разбора args->num.
//   * sigreturn (0x118) и kill самого себя при KILL-action всегда
//     разрешены — иначе мы не сможем доставить SIGKILL до user-side.
//   * STRICT-режим = жёсткий whitelist: 0x100 (exit), 0x101 (write),
//     0x102 (read), 0x118 (sigreturn).  Эта эмуляция Linux seccomp(STRICT).
// ============================================================================
#include "seccomp.h"
#include "syscall.h"
#include "task.h"
#include "heap.h"
#include "serial.h"

// Чтобы избежать «hard lock» если кто-то применит seccomp на саму kernel-
// задачу — внутренний массив фильтров. NULL = нет (fast-path).

void seccomp_init(void) {
    serial_puts("[seccomp] subsystem initialized\n");
}

seccomp_filter_t* seccomp_create(seccomp_default_t def) {
    seccomp_filter_t* f = (seccomp_filter_t*)kmalloc(sizeof(seccomp_filter_t));
    if (!f) return 0;
    f->refcount      = 1;
    f->def_action    = def;
    f->strict        = false;
    f->n_allow       = 0;
    f->total_checks  = 0;
    f->total_blocked = 0;
    f->total_allowed = 0;
    for (int i = 0; i < SECCOMP_MAX_RULES; i++) f->allow[i] = 0;
    return f;
}

void seccomp_destroy(seccomp_filter_t* f) {
    if (!f) return;
    kfree(f);
}

seccomp_filter_t* seccomp_retain(seccomp_filter_t* f) {
    if (!f) return 0;
    f->refcount++;
    return f;
}

void seccomp_release(seccomp_filter_t* f) {
    if (!f) return;
    f->refcount--;
    if (f->refcount <= 0) seccomp_destroy(f);
}

int seccomp_allow(seccomp_filter_t* f, uint32_t sysnum) {
    if (!f) return -1;
    if (f->n_allow >= SECCOMP_MAX_RULES) return -1;
    // Дубликаты не добавляем.
    for (int i = 0; i < f->n_allow; i++) {
        if (f->allow[i] == sysnum) return 0;
    }
    f->allow[f->n_allow++] = sysnum;
    return 0;
}

void seccomp_set_strict(seccomp_filter_t* f) {
    if (!f) return;
    f->strict = true;
    f->def_action = SECCOMP_DEF_KILL;
}

int seccomp_check(seccomp_filter_t* f, uint32_t sysnum) {
    if (!f) return 0;
    f->total_checks++;

    // sigreturn — никогда не блокируется (иначе не доставим сигнал).
    if (sysnum == AOS_SYS_SIGRETURN) {
        f->total_allowed++;
        return 0;
    }

    if (f->strict) {
        // STRICT: только read/write/exit/sigreturn.
        if (sysnum == AOS_SYS_READ ||
            sysnum == AOS_SYS_WRITE ||
            sysnum == AOS_SYS_EXIT) {
            f->total_allowed++;
            return 0;
        }
        f->total_blocked++;
        return -2;       // KILL по STRICT
    }

    for (int i = 0; i < f->n_allow; i++) {
        if (f->allow[i] == sysnum) {
            f->total_allowed++;
            return 0;
        }
    }
    f->total_blocked++;
    switch (f->def_action) {
    case SECCOMP_DEF_ALLOW:  return 0;
    case SECCOMP_DEF_ERRNO:  return -1;     // EPERM-equivalent
    case SECCOMP_DEF_KILL:   return -2;
    }
    return -1;
}

int seccomp_apply_to(struct task* t, seccomp_filter_t* f) {
    if (!t) return -1;
    if (t->seccomp) seccomp_release(t->seccomp);
    t->seccomp = seccomp_retain(f);
    return 0;
}

// ---- Профили --------------------------------------------------------------
//   default: всё, что нужно обычному ELF: I/O+process+timer+brk.
//   strict:  только read/write/exit/sigreturn.
//   net:     default + sockets.
//   compute: только compute (no fs/net): exit, yield, sleep, brk, now.
static seccomp_filter_t* profile_default(void) {
    seccomp_filter_t* f = seccomp_create(SECCOMP_DEF_ERRNO);
    if (!f) return 0;
    seccomp_allow(f, AOS_SYS_EXIT);
    seccomp_allow(f, AOS_SYS_WRITE);
    seccomp_allow(f, AOS_SYS_READ);
    seccomp_allow(f, AOS_SYS_OPEN);
    seccomp_allow(f, AOS_SYS_CLOSE);
    seccomp_allow(f, AOS_SYS_SEEK);
    seccomp_allow(f, AOS_SYS_PID);
    seccomp_allow(f, AOS_SYS_UID);
    seccomp_allow(f, AOS_SYS_NOW);
    seccomp_allow(f, AOS_SYS_YIELD);
    seccomp_allow(f, AOS_SYS_SLEEP);
    seccomp_allow(f, AOS_SYS_BRK);
    seccomp_allow(f, AOS_SYS_SBRK);
    seccomp_allow(f, AOS_SYS_STAT);
    seccomp_allow(f, AOS_SYS_FSTAT);
    seccomp_allow(f, AOS_SYS_MKDIR);
    seccomp_allow(f, AOS_SYS_DUP);
    seccomp_allow(f, AOS_SYS_DUP2);
    seccomp_allow(f, AOS_SYS_FCNTL);
    seccomp_allow(f, AOS_SYS_KILL);
    seccomp_allow(f, AOS_SYS_SIGACTION);
    return f;
}

static seccomp_filter_t* profile_strict(void) {
    seccomp_filter_t* f = seccomp_create(SECCOMP_DEF_KILL);
    if (!f) return 0;
    seccomp_set_strict(f);
    return f;
}

static seccomp_filter_t* profile_net(void) {
    seccomp_filter_t* f = profile_default();
    if (!f) return 0;
    seccomp_allow(f, AOS_SYS_SOCKET);
    seccomp_allow(f, AOS_SYS_BIND);
    seccomp_allow(f, AOS_SYS_LISTEN);
    seccomp_allow(f, AOS_SYS_ACCEPT);
    seccomp_allow(f, AOS_SYS_CONNECT);
    seccomp_allow(f, AOS_SYS_SEND);
    seccomp_allow(f, AOS_SYS_RECV);
    seccomp_allow(f, AOS_SYS_SENDTO);
    seccomp_allow(f, AOS_SYS_RECVFROM);
    seccomp_allow(f, AOS_SYS_SHUTDOWN);
    return f;
}

static seccomp_filter_t* profile_compute(void) {
    seccomp_filter_t* f = seccomp_create(SECCOMP_DEF_ERRNO);
    if (!f) return 0;
    seccomp_allow(f, AOS_SYS_EXIT);
    seccomp_allow(f, AOS_SYS_YIELD);
    seccomp_allow(f, AOS_SYS_SLEEP);
    seccomp_allow(f, AOS_SYS_BRK);
    seccomp_allow(f, AOS_SYS_SBRK);
    seccomp_allow(f, AOS_SYS_NOW);
    seccomp_allow(f, AOS_SYS_PID);
    seccomp_allow(f, AOS_SYS_WRITE);   // stdout остаётся
    return f;
}

static int prof_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

seccomp_filter_t* seccomp_profile(const char* name) {
    if (!name) return profile_default();
    if (prof_strcmp(name, "default") == 0) return profile_default();
    if (prof_strcmp(name, "strict")  == 0) return profile_strict();
    if (prof_strcmp(name, "net")     == 0) return profile_net();
    if (prof_strcmp(name, "compute") == 0) return profile_compute();
    return profile_default();
}
