#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKS_PER_SEC  1000000L

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long __tm_gmtoff;
    const char* __tm_zone;
};

time_t      time   (time_t* tloc);
clock_t     clock  (void);
int         clock_gettime(clockid_t id, struct timespec* ts);
int         clock_nanosleep(clockid_t id, int flags, const struct timespec* rqtp,
                            struct timespec* rmtp);
int         nanosleep(const struct timespec* rqtp, struct timespec* rmtp);
int         gettimeofday(struct timeval* tv, void* tz);

double      difftime(time_t a, time_t b);
struct tm*  localtime(const time_t* t);
struct tm*  gmtime   (const time_t* t);
struct tm*  localtime_r(const time_t* t, struct tm* out);
time_t      mktime  (struct tm* t);
size_t      strftime(char* __restrict__ buf, size_t n,
                     const char* __restrict__ fmt,
                     const struct tm* __restrict__ tm);
char*       asctime (const struct tm* t);
char*       ctime   (const time_t* t);

#ifdef __cplusplus
}
#endif
#endif
