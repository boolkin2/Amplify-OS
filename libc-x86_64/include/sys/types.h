// ============================================================================
// sys/types.h — типы POSIX. LP64 x86_64.
// ============================================================================
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long           off_t;
typedef long           off64_t;
typedef long           blkcnt_t;
typedef long           blksize_t;
typedef int            pid_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned int   mode_t;
typedef unsigned long  ino_t;
typedef unsigned long  ino64_t;
typedef unsigned long  dev_t;
typedef unsigned long  nlink_t;
typedef long           time_t;
typedef long           suseconds_t;
typedef int            clockid_t;
typedef int            timer_t;
typedef int            clock_t;
typedef int            id_t;
typedef int            key_t;

// pthread types — определены и в pthread.h, защищены своими guard'ами.
#ifndef _PTHREAD_T_DEFINED
#define _PTHREAD_T_DEFINED
typedef unsigned long  pthread_t;
#endif

#ifdef __cplusplus
}
#endif
#endif
