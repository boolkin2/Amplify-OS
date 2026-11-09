// ============================================================================
// stat.c — stat/fstat/mkdir через AOS_SYS_STAT / AOS_SYS_FSTAT / AOS_SYS_MKDIR.
// ----------------------------------------------------------------------------
// Этап 40 группа A. Ядро возвращает aos_stat_t, libc struct stat имеет
// тот же layout (см. libc-x86_64/include/sys/stat.h), поэтому буфер
// передаётся напрямую без промежуточного копирования.
// ============================================================================
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include "../syscall/aos_arch.h"

int stat(const char* path, struct stat* st) {
    long r = __aos_syscall2(AOS_SYS_STAT, (long)path, (long)st);
    return (int)__aos_set_errno(r);
}

int fstat(int fd, struct stat* st) {
    long r = __aos_syscall2(AOS_SYS_FSTAT, (long)fd, (long)st);
    return (int)__aos_set_errno(r);
}

int lstat(const char* path, struct stat* st) {
    // Символьных ссылок на FAT32 нет — lstat = stat.
    return stat(path, st);
}

int mkdir(const char* path, mode_t mode) {
    long r = __aos_syscall2(AOS_SYS_MKDIR, (long)path, (long)mode);
    return (int)__aos_set_errno(r);
}

int chmod(const char* path, mode_t mode) {
    (void)path; (void)mode;
    return 0;                // no-op (stage 40)
}

int fchmod(int fd, mode_t mode) {
    (void)fd; (void)mode;
    return 0;
}

mode_t umask(mode_t mask) {
    (void)mask;
    return 0;
}
