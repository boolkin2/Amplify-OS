// ============================================================================
// fcntl.c — open/creat/fcntl через AOS_SYS_OPEN / AOS_SYS_FCNTL.
// ----------------------------------------------------------------------------
// Этап 40 группа A:
//   * open() теперь ПРОБРАСЫВАЕТ flags в ядро (до этапа 40 ядро игнорило
//     все кроме RDONLY). Ядро интерпретирует O_CREAT/O_TRUNC/O_APPEND/O_EXCL.
//   * fcntl() поддерживает F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD.
// ============================================================================
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include "../syscall/aos_arch.h"

int open(const char* path, int flags, ...) {
    // mode из va_arg игнорим: ядро его принимает, но ACL-слой не реализован.
    long r = __aos_syscall2(AOS_SYS_OPEN, (long)path, (long)flags);
    return (int)__aos_set_errno(r);
}

int creat(const char* path, mode_t mode) {
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    long r = __aos_syscall3(AOS_SYS_FCNTL, (long)fd, (long)cmd, arg);
    return (int)__aos_set_errno(r);
}
