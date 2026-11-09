// ============================================================================
// aos_arch.h — внутренний syscall-арх для musl-aos x86_64 (этап 39).
// ----------------------------------------------------------------------------
// SysV ABI x86_64 + AMPLIFY-OS SYSCALL/SYSRET (см. kernel/arch/x86_64/syscall.h).
//   RAX = syscall_num
//   RDI, RSI, RDX, R10, R8, R9 = arg1..6  (R10 вместо RCX!)
//   RAX = return
//   Сохраняются: RBX, RBP, R12..R15. Уничтожаются: RCX, R11 (RIP/RFLAGS).
//
// Номера ОБЯЗАНЫ совпадать с kernel/arch/x86_64/syscall.h. Не менять без
// синхронной правки ядра.
// ============================================================================
#ifndef _AOS_ARCH_H
#define _AOS_ARCH_H

#include <stdint.h>

#define AOS_SYS_EXIT     0x100
#define AOS_SYS_WRITE    0x101
#define AOS_SYS_READ     0x102
#define AOS_SYS_OPEN     0x103
#define AOS_SYS_CLOSE    0x104
#define AOS_SYS_SEEK     0x105
#define AOS_SYS_PID      0x106
#define AOS_SYS_UID      0x107
#define AOS_SYS_NOW      0x108
#define AOS_SYS_YIELD    0x109
#define AOS_SYS_SLEEP    0x10A
#define AOS_SYS_BRK      0x10B
#define AOS_SYS_SBRK     0x10C

// ---- Этап 40 группа A: файловые операции ---------------------------------
#define AOS_SYS_STAT     0x110
#define AOS_SYS_FSTAT    0x111
#define AOS_SYS_MKDIR    0x112
#define AOS_SYS_DUP      0x113
#define AOS_SYS_DUP2     0x114
#define AOS_SYS_FCNTL    0x115

// ---- Этап 40 группа B (сигналы) ------------------------------------------
#define AOS_SYS_SIGACTION 0x116
#define AOS_SYS_KILL      0x117
#define AOS_SYS_SIGRETURN 0x118

// ---- Этап 40 группа D (threads) ------------------------------------------
#define AOS_SYS_CLONE      0x120
#define AOS_SYS_FUTEX_WAIT 0x121
#define AOS_SYS_FUTEX_WAKE 0x122
#define AOS_SYS_SET_TID    0x123
#define AOS_SYS_JOIN       0x124

// ---- Этап 40 группа C (sockets) ------------------------------------------
#define AOS_SYS_SOCKET     0x130
#define AOS_SYS_BIND       0x131
#define AOS_SYS_LISTEN     0x132
#define AOS_SYS_ACCEPT     0x133
#define AOS_SYS_CONNECT    0x134
#define AOS_SYS_SEND       0x135
#define AOS_SYS_RECV       0x136
#define AOS_SYS_SENDTO     0x137
#define AOS_SYS_RECVFROM   0x138
#define AOS_SYS_SHUTDOWN   0x139
#define AOS_SYS_SETSOCKOPT 0x13A
#define AOS_SYS_GETSOCKOPT 0x13B

// Внешние ассемблерные трамплины (libc-x86_64/src/syscall/syscall.S).
// Возвращают сырое RAX из ядра. Set-errno делает caller через __aos_set_errno.
extern long __aos_syscall0(long num);
extern long __aos_syscall1(long num, long a1);
extern long __aos_syscall2(long num, long a1, long a2);
extern long __aos_syscall3(long num, long a1, long a2, long a3);
extern long __aos_syscall4(long num, long a1, long a2, long a3, long a4);
extern long __aos_syscall5(long num, long a1, long a2, long a3, long a4, long a5);
extern long __aos_syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6);

// errno-helper: если ret < 0 и |ret| < 4096 — трактуем как -errno.
// Возвращает ret (для удобства chaining).
long __aos_set_errno(long ret);

#endif
