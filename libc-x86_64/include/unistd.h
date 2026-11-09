// ============================================================================
// unistd.h — POSIX операционная среда.
// ============================================================================
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define F_OK            0
#define R_OK            4
#define W_OK            2
#define X_OK            1

#define _POSIX_VERSION  200809L

extern char** environ;

// Файловые операции
ssize_t read   (int fd, void* buf, size_t n);
ssize_t write  (int fd, const void* buf, size_t n);
int     close  (int fd);
off_t   lseek  (int fd, off_t off, int whence);
int     unlink (const char* path);
int     access (const char* path, int mode);
int     dup    (int fd);
int     dup2   (int oldfd, int newfd);
int     pipe   (int fd[2]);
int     fsync  (int fd);
int     ftruncate(int fd, off_t length);
int     isatty (int fd);
int     chdir  (const char* path);
int     fchdir (int fd);
char*   getcwd (char* buf, size_t size);
int     rmdir  (const char* path);

// Процесс
pid_t   getpid (void);
pid_t   getppid(void);
uid_t   getuid (void);
uid_t   geteuid(void);
gid_t   getgid (void);
gid_t   getegid(void);
pid_t   fork   (void);
int     execv  (const char* path, char* const argv[]);
int     execvp (const char* file, char* const argv[]);
int     execve (const char* path, char* const argv[], char* const envp[]);
void    _exit  (int status) __attribute__((noreturn));

// Тайминги
unsigned int sleep  (unsigned int sec);
int          usleep (unsigned int usec);

// Память
void*   sbrk   (intptr_t inc);
int     brk    (void* end);

#ifdef __cplusplus
}
#endif
#endif
