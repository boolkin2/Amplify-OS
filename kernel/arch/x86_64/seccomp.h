// ============================================================================
// seccomp.h — белый список (whitelist) системных вызовов для AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Этап 46. Облегчённая версия Linux seccomp-bpf:
//
//  * у каждого фильтра — массив до 128 разрешённых syscall-номеров;
//  * по умолчанию все syscall'ы РАЗРЕШЕНЫ (ALLOW_ALL);
//  * если фильтр активен — поведение по запрещённому syscall задаётся
//    действием (action): ALLOW, ERRNO(-EPERM), KILL_PROCESS;
//  * опционально — режим STRICT (только: read, write, exit, sigreturn);
//
//  * каждая task_t имеет seccomp_filter_t* — может быть NULL (нет фильтра).
//
// Применяется в syscall_dispatch перед switch'ем по args->num. Если
// действие — ERRNO, syscall сразу возвращает -EPERM. Если KILL — task_exit.
// ============================================================================
#ifndef AOS_X86_64_SECCOMP_H
#define AOS_X86_64_SECCOMP_H

#include <stdint.h>
#include <stdbool.h>

// Forward-decl. См. ns.h — без явного `struct task;` C создаёт прототип-
// локальную копию tag'а и получаем conflicting types при компиляции seccomp.c.
struct task;

#define SECCOMP_MAX_RULES   128

typedef enum {
    SECCOMP_DEF_ALLOW = 0,   // не в whitelist → ALLOW (только log)
    SECCOMP_DEF_ERRNO = 1,   // не в whitelist → ERRNO -EPERM
    SECCOMP_DEF_KILL  = 2    // не в whitelist → kill task
} seccomp_default_t;

typedef struct seccomp_filter {
    int                refcount;
    seccomp_default_t  def_action;
    bool               strict;            // STRICT: только read/write/exit/sigreturn
    int                n_allow;
    uint32_t           allow[SECCOMP_MAX_RULES];

    // Статистика.
    uint64_t           total_checks;
    uint64_t           total_blocked;
    uint64_t           total_allowed;
} seccomp_filter_t;

void               seccomp_init(void);

seccomp_filter_t*  seccomp_create(seccomp_default_t def);
void               seccomp_destroy(seccomp_filter_t* f);
seccomp_filter_t*  seccomp_retain(seccomp_filter_t* f);
void               seccomp_release(seccomp_filter_t* f);

// Добавить syscall-номер в whitelist. Возвращает 0/-1.
int                seccomp_allow(seccomp_filter_t* f, uint32_t sysnum);

// Включить STRICT-режим (read/write/exit/sigreturn only).
void               seccomp_set_strict(seccomp_filter_t* f);

// Главная проверка: возвращает 0 = ALLOW, отрицательное errno = блокировано
// (через ERRNO action). -2 = должна быть KILL (вызывающий обязан task_exit).
int                seccomp_check(seccomp_filter_t* f, uint32_t sysnum);

// Применить готовую конфигурацию к текущей задаче (вызывается из syscall).
// Используется box-runtime, чтобы установить фильтр перед exec.
struct task;
int                seccomp_apply_to(struct task* t, seccomp_filter_t* f);

// Парсинг profile-имени → готовый фильтр (для box).
//   "default"   — разрешает только то, что нужно типичному ELF (read/write/
//                 open/close/seek/exit/yield/sleep/now/brk/sbrk/getpid/kill).
//   "strict"    — только read/write/exit/sigreturn.
//   "net"       — default + sockets.
//   "compute"   — только compute (без I/O).
seccomp_filter_t*  seccomp_profile(const char* name);

#endif
