#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG    1
#define WUNTRACED  2

#define WEXITSTATUS(s) (((s) & 0xFF00) >> 8)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((signed char)(((s) & 0x7F) + 1) >> 1) > 0)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)

pid_t wait (int* status);
pid_t waitpid(pid_t pid, int* status, int options);

#ifdef __cplusplus
}
#endif
#endif
