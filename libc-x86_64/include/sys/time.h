#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval* tv, void* tz);
int settimeofday(const struct timeval* tv, const struct timezone* tz);

#ifdef __cplusplus
}
#endif
#endif
