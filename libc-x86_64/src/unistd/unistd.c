// ============================================================================
// unistd.c — POSIX обёртки над AOS syscall'ами.
// ----------------------------------------------------------------------------
// Каждая функция = единственный вызов __aos_syscallN + __aos_set_errno.
// ============================================================================
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include "../syscall/aos_arch.h"

ssize_t write(int fd, const void* buf, size_t n) {
    return __aos_set_errno(__aos_syscall3(AOS_SYS_WRITE, fd, (long)buf, (long)n));
}

ssize_t read(int fd, void* buf, size_t n) {
    return __aos_set_errno(__aos_syscall3(AOS_SYS_READ, fd, (long)buf, (long)n));
}

int close(int fd) {
    return (int)__aos_set_errno(__aos_syscall1(AOS_SYS_CLOSE, fd));
}

off_t lseek(int fd, off_t off, int whence) {
    return (off_t)__aos_set_errno(__aos_syscall3(AOS_SYS_SEEK, fd, off, whence));
}

pid_t getpid(void) {
    return (pid_t)__aos_syscall0(AOS_SYS_PID);
}

pid_t getppid(void) { return 1; }       // На 39-м у нас нет дерева процессов
uid_t getuid (void) { return (uid_t)__aos_syscall0(AOS_SYS_UID); }
uid_t geteuid(void) { return getuid(); }
gid_t getgid (void) { return 0; }
gid_t getegid(void) { return 0; }

int isatty(int fd) {
    // Простая эвристика: 0/1/2 — терминал.
    if (fd >= 0 && fd <= 2) return 1;
    errno = ENOTTY;
    return 0;
}

__attribute__((noreturn))
void _exit(int status) {
    __aos_syscall1(AOS_SYS_EXIT, status);
    for (;;) __asm__ volatile("hlt");
}

void* sbrk(intptr_t inc) {
    long r = __aos_syscall1(AOS_SYS_SBRK, (long)inc);
    if (r == -1) {                  // OOM / out-of-range
        errno = ENOMEM;
        return (void*)-1;
    }
    return (void*)(uintptr_t)r;
}

int brk(void* end) {
    long r = __aos_syscall1(AOS_SYS_BRK, (long)end);
    if (r == (long)end || r > (long)end) return 0;
    errno = ENOMEM;
    return -1;
}

unsigned int sleep(unsigned int sec) {
    long ns = (long)sec * 1000000000L;
    __aos_syscall2(AOS_SYS_SLEEP,
                   (long)(ns & 0xFFFFFFFF),
                   (long)((unsigned long)ns >> 32));
    return 0;
}

int usleep(unsigned int usec) {
    long ns = (long)usec * 1000L;
    __aos_syscall2(AOS_SYS_SLEEP,
                   (long)(ns & 0xFFFFFFFF),
                   (long)((unsigned long)ns >> 32));
    return 0;
}

// Stubs: пока ядро не умеет fork/exec/dup/pipe/access/unlink/chdir — стопорим.
pid_t fork(void)                       { errno = ENOSYS; return -1; }
int   execv(const char* p, char* const a[])
                                       { (void)p; (void)a; errno = ENOSYS; return -1; }
int   execvp(const char* p, char* const a[])
                                       { (void)p; (void)a; errno = ENOSYS; return -1; }
int   execve(const char* p, char* const a[], char* const e[])
                                       { (void)p; (void)a; (void)e; errno = ENOSYS; return -1; }
int   dup(int fd) {
    long r = __aos_syscall1(AOS_SYS_DUP, (long)fd);
    return (int)__aos_set_errno(r);
}
int   dup2(int o, int n) {
    long r = __aos_syscall2(AOS_SYS_DUP2, (long)o, (long)n);
    return (int)__aos_set_errno(r);
}
int   pipe(int fd[2])                  { (void)fd; errno = ENOSYS; return -1; }
int   fsync(int fd)                    { (void)fd; return 0; }
int   ftruncate(int fd, off_t l)       { (void)fd; (void)l; errno = ENOSYS; return -1; }
int   access(const char* p, int m)     { (void)p; (void)m; errno = ENOSYS; return -1; }
int   unlink(const char* p)            { (void)p; errno = ENOSYS; return -1; }
int   chdir(const char* p)             { (void)p; errno = ENOSYS; return -1; }
int   fchdir(int fd)                   { (void)fd; errno = ENOSYS; return -1; }
char* getcwd(char* buf, size_t sz) {
    if (!buf || sz < 2) { errno = ERANGE; return NULL; }
    buf[0] = '/';                                  /* фиксированный «корень» */
    buf[1] = 0;
    return buf;
}
int   rmdir(const char* p)             { (void)p; errno = ENOSYS; return -1; }
