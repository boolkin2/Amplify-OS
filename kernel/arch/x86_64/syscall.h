// ============================================================================
// syscall.h — SYSCALL/SYSRET fast syscall path для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Кардинальное отличие от i386 (int 0x80 + EBX/ECX/EDX/ESI/EDI):
//   * Используется SYSCALL (вход) и SYSRETQ (возврат) — это FAST path Intel/AMD,
//     минует IDT и сегментные дескрипторы. Тратит ~50 ns против ~150 ns int.
//   * Аргументы по System V x86_64 ABI:
//       RAX = syscall_num
//       RDI, RSI, RDX, R10, R8, R9 = arg1..arg6  (R10 вместо RCX, потому что
//                                                 RCX занят user RIP при syscall)
//       RAX = возвращаемое значение
//   * MSR'ы инициализируются в syscall_init():
//       IA32_EFER  bit 0 (SCE) — включает SYSCALL/SYSRET
//       IA32_STAR (0xC0000081) — селекторы CS/SS для kernel и user
//       IA32_LSTAR (0xC0000082) — точка входа в long mode
//       IA32_FMASK (0xC0000084) — биты RFLAGS, которые CPU очищает при syscall
//
// Номера syscall'ов 1-в-1 копия kernel/arch/i386/syscall.h, чтобы userspace
// мог использовать тот же aos_syscall.h после перекомпиляции.
// ============================================================================
#ifndef AOS_X86_64_SYSCALL_H
#define AOS_X86_64_SYSCALL_H

#include <stdint.h>

// AMPLIFY-OS native syscalls (зеркало kernel/arch/i386/syscall.h)
#define AOS_SYS_EXIT            0x100
#define AOS_SYS_WRITE           0x101
#define AOS_SYS_READ            0x102
#define AOS_SYS_OPEN            0x103
#define AOS_SYS_CLOSE           0x104
#define AOS_SYS_SEEK            0x105
#define AOS_SYS_PID             0x106
#define AOS_SYS_UID             0x107
#define AOS_SYS_NOW             0x108
#define AOS_SYS_YIELD           0x109
#define AOS_SYS_SLEEP           0x10A
#define AOS_SYS_BRK             0x10B
#define AOS_SYS_SBRK            0x10C

// ---- Этап 40 группа A: файловые операции, stat, dup, fcntl ----------------
// Номера выбраны с гарантированным зазором от 0x100..0x10C (заморожены ABI).
#define AOS_SYS_STAT            0x110   // stat(path, *aos_stat)
#define AOS_SYS_FSTAT           0x111   // fstat(fd, *aos_stat)
#define AOS_SYS_MKDIR           0x112   // mkdir(path, mode)
#define AOS_SYS_DUP             0x113   // dup(fd) -> newfd
#define AOS_SYS_DUP2            0x114   // dup2(old, new) -> new
#define AOS_SYS_FCNTL           0x115   // fcntl(fd, cmd, arg) -> value

// ---- Этап 40 группа B (сигналы) — номера зарезервированы, реализация позже --
#define AOS_SYS_SIGACTION       0x116
#define AOS_SYS_KILL            0x117
#define AOS_SYS_SIGRETURN       0x118

// ---- Этап 40 группа D (threads) — номера зарезервированы ------------------
#define AOS_SYS_CLONE           0x120
#define AOS_SYS_FUTEX_WAIT      0x121
#define AOS_SYS_FUTEX_WAKE      0x122
#define AOS_SYS_SET_TID         0x123
#define AOS_SYS_JOIN            0x124

// ---- Этап 40 группа C (sockets) — номера зарезервированы ------------------
#define AOS_SYS_SOCKET          0x130
#define AOS_SYS_BIND            0x131
#define AOS_SYS_LISTEN          0x132
#define AOS_SYS_ACCEPT          0x133
#define AOS_SYS_CONNECT         0x134
#define AOS_SYS_SEND            0x135
#define AOS_SYS_RECV            0x136
#define AOS_SYS_SENDTO          0x137
#define AOS_SYS_RECVFROM        0x138
#define AOS_SYS_SHUTDOWN        0x139
#define AOS_SYS_SETSOCKOPT      0x13A
#define AOS_SYS_GETSOCKOPT      0x13B

// ---- Этап 41 (GPU / DRM): новые syscalls 0x140..0x148 ---------------------
// Не пересекаются с group C (0x130..0x13B). Зарезервированы как стабильное
// ABI; userspace (libdrm) будет использовать их через syscall.h.
#define AOS_SYS_DRM_OPEN        0x140   // open /dev/dri/card0 -> 0/1 (active)
#define AOS_SYS_DRM_GET_INFO    0x141   // copy aos_drm_info_t to user buf
#define AOS_SYS_DRM_PRESENT     0x142   // принудительный flush primary FB
#define AOS_SYS_DRM_CREATE_DUMB 0x143   // зарезервировано (этап 44)
#define AOS_SYS_DRM_MAP         0x144   // зарезервировано (этап 44)
#define AOS_SYS_DRM_ADDFB       0x145   // зарезервировано (этап 44)
#define AOS_SYS_DRM_SETCRTC     0x146   // зарезервировано (этап 44)
#define AOS_SYS_DRM_PAGEFLIP    0x147   // зарезервировано (этап 44)
#define AOS_SYS_DRM_DESTROY     0x148   // зарезервировано (этап 44)

// ---- Этап 46 (Sandboxes): namespaces / cgroups / seccomp ------------------
// 0x150..0x15F зарезервировано под весь sandbox-stack.
#define AOS_SYS_UNSHARE         0x150   // unshare(flags) — копия namespace'ов
#define AOS_SYS_SETHOSTNAME     0x151   // sethostname(name, len)
#define AOS_SYS_GETHOSTNAME     0x152   // gethostname(buf, len)
#define AOS_SYS_SECCOMP_LOAD    0x153   // seccomp_load(profile_name) — set strict whitelist
#define AOS_SYS_SECCOMP_ALLOW   0x154   // seccomp_allow(sysnum) — добавить в whitelist
#define AOS_SYS_CGROUP_CREATE   0x155   // cgroup_create(name) -> id
#define AOS_SYS_CGROUP_LIMIT    0x156   // cgroup_limit(id, ctrl, value)
#define AOS_SYS_CGROUP_ATTACH   0x157   // cgroup_attach(tid, id)
#define AOS_SYS_CGROUP_STAT     0x158   // cgroup_stat(id, &out)
#define AOS_SYS_NS_INFO         0x159   // ns_info(&out_array, max)
#define AOS_SYS_BOX_RUN         0x15A   // box_run(spec*, log*, log_max) -> tid

// ---- Флаги для AOS_SYS_UNSHARE (зеркало ns.h CLONE_NEW*) ------------------
// Эти константы переэкспортируются под публичными именами для libc.
#define AOS_UNSHARE_NEWPID      0x20000000
#define AOS_UNSHARE_NEWNS       0x00020000
#define AOS_UNSHARE_NEWUTS      0x04000000
#define AOS_UNSHARE_NEWNET      0x40000000
#define AOS_UNSHARE_NEWUSER     0x10000000

// Контроллеры для AOS_SYS_CGROUP_LIMIT.
#define AOS_CGCTRL_CPU_NS       0
#define AOS_CGCTRL_CPU_PERIOD   1
#define AOS_CGCTRL_MEM          2
#define AOS_CGCTRL_IO           3
#define AOS_CGCTRL_PIDS         4

// Профили seccomp (передаются как строка в AOS_SYS_SECCOMP_LOAD).
//   "default", "strict", "net", "compute"

// ---- Флаги open() (этап 40, совпадают с libc-x86_64/include/fcntl.h) -------
#define AOS_O_RDONLY            0x0000
#define AOS_O_WRONLY            0x0001
#define AOS_O_RDWR              0x0002
#define AOS_O_ACCMODE           0x0003
#define AOS_O_CREAT             0x0040
#define AOS_O_EXCL              0x0080
#define AOS_O_TRUNC             0x0200
#define AOS_O_APPEND            0x0400
#define AOS_O_NONBLOCK          0x0800

// ---- fcntl команды ---------------------------------------------------------
#define AOS_F_DUPFD             0
#define AOS_F_GETFD             1
#define AOS_F_SETFD             2
#define AOS_F_GETFL             3
#define AOS_F_SETFL             4

// ---- struct aos_stat — зеркало libc struct stat ---------------------------
// Размер 144 байта; согласован с libc-x86_64/include/sys/stat.h (см. Group A).
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
} aos_stat_t;

// Биты st_mode (как в POSIX и libc).
#define AOS_S_IFMT    0170000
#define AOS_S_IFREG   0100000
#define AOS_S_IFDIR   0040000

// ---- Коды ошибок, возвращаемые ядром (зеркало libc errno.h) ---------------
// Отрицательное значение на RAX, диапазон 1..4095, libc конвертирует в errno.
#define AOS_EPERM            1
#define AOS_ENOENT           2
#define AOS_EBADF            9
#define AOS_EAGAIN          11
#define AOS_ENOMEM          12
#define AOS_EACCES          13
#define AOS_EFAULT          14
#define AOS_EBUSY           16
#define AOS_EEXIST          17
#define AOS_ENODEV          19
#define AOS_ENOTDIR         20
#define AOS_EISDIR          21
#define AOS_EINVAL          22
#define AOS_EMFILE          24
#define AOS_ENOSPC          28
#define AOS_ENOSYS          38
#define AOS_EAFNOSUPPORT    97
#define AOS_EADDRINUSE      98
#define AOS_ECONNREFUSED    111
#define AOS_EOPNOTSUPP      95
#define AOS_EIO             5
#define AOS_ENOTCONN        107
#define AOS_EDESTADDRREQ    89
#define AOS_ETIMEDOUT       110

// Структура аргументов, которую строит syscall_entry.asm на стеке и
// передаёт в C-диспатчер по указателю.
typedef struct {
    uint64_t num;        // RAX
    uint64_t a1;         // RDI
    uint64_t a2;         // RSI
    uint64_t a3;         // RDX
    uint64_t a4;         // R10
    uint64_t a5;         // R8
    uint64_t a6;         // R9
} syscall_args_t;

void     syscall_init(uint64_t kernel_stack_top);
uint64_t syscall_dispatch(syscall_args_t* args);

// Глобалы из syscall_entry.asm — нужны и из C (для отладки, и для SMP-апгрейда
// в 37.4b, когда заменим на per-CPU GS).
extern uint64_t kernel_rsp_save;
extern uint64_t user_rsp_save;

#endif
