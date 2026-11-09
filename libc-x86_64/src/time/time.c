// ============================================================================
// time.c — time/clock/gettimeofday/nanosleep/localtime поверх AOS_SYS_NOW.
// ============================================================================
#include <time.h>
#include <sys/time.h>
#include <stddef.h>
#include <errno.h>
#include "../syscall/aos_arch.h"

// Точка отсчёта эпохи Unix — на 39-м этапе у нас нет RTC-моста в user, поэтому
// time() возвращает фиктивную «секунду с момента старта системы». Когда
// ядро будет отдавать realtime через AOS_SYS_NOW2 или ACPI/RTC drv, заменим.
static const time_t EPOCH_BASE = 1747e6;   // 2025-05-13 round-figure

static uint64_t now_ms_kernel(void) {
    uint64_t ms = 0;
    __aos_syscall1(AOS_SYS_NOW, (long)&ms);
    return ms;
}

time_t time(time_t* tloc) {
    time_t t = (time_t)(EPOCH_BASE + (long)(now_ms_kernel() / 1000));
    if (tloc) *tloc = t;
    return t;
}

clock_t clock(void) {
    return (clock_t)(now_ms_kernel() * 1000);    /* us */
}

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (!tv) { errno = EFAULT; return -1; }
    uint64_t ms = now_ms_kernel();
    tv->tv_sec  = (time_t)(EPOCH_BASE + (long)(ms / 1000));
    tv->tv_usec = (suseconds_t)((ms % 1000) * 1000);
    return 0;
}

int settimeofday(const struct timeval* tv, const struct timezone* tz) {
    (void)tv; (void)tz;
    errno = EPERM;
    return -1;
}

int clock_gettime(clockid_t id, struct timespec* ts) {
    (void)id;
    if (!ts) { errno = EFAULT; return -1; }
    uint64_t ms = now_ms_kernel();
    if (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW) {
        ts->tv_sec  = (time_t)(ms / 1000);
        ts->tv_nsec = (long)((ms % 1000) * 1000000L);
    } else {
        ts->tv_sec  = (time_t)(EPOCH_BASE + (long)(ms / 1000));
        ts->tv_nsec = (long)((ms % 1000) * 1000000L);
    }
    return 0;
}

int nanosleep(const struct timespec* rq, struct timespec* rem) {
    (void)rem;
    if (!rq) { errno = EFAULT; return -1; }
    uint64_t ns = (uint64_t)rq->tv_sec * 1000000000ULL + (uint64_t)rq->tv_nsec;
    __aos_syscall2(AOS_SYS_SLEEP, (long)(ns & 0xFFFFFFFF), (long)(ns >> 32));
    return 0;
}

int clock_nanosleep(clockid_t id, int flags, const struct timespec* rq,
                    struct timespec* rem) {
    (void)id; (void)flags;
    return nanosleep(rq, rem);
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

// Упрощённые tm-функции (UTC, без TZ-базы). Этап 52 расширит локалями.
static const int days_per_month_norm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
static const int days_per_month_leap[12] = {31,29,31,30,31,30,31,31,30,31,30,31};
static int is_leap(int y) { return (y%4==0 && y%100!=0) || (y%400==0); }

static struct tm tm_buf;

struct tm* gmtime(const time_t* t) {
    if (!t) return NULL;
    long s = (long)*t;
    long days = s / 86400; long sec = s % 86400;
    tm_buf.tm_hour = sec / 3600;
    tm_buf.tm_min  = (sec % 3600) / 60;
    tm_buf.tm_sec  = sec % 60;
    tm_buf.tm_wday = (int)((days + 4) % 7);     // 1970-01-01 = Thursday(4)
    int year = 1970;
    while (1) {
        int dy = is_leap(year) ? 366 : 365;
        if (days < dy) break;
        days -= dy; year++;
    }
    tm_buf.tm_year = year - 1900;
    tm_buf.tm_yday = (int)days;
    const int* dpm = is_leap(year) ? days_per_month_leap : days_per_month_norm;
    int m = 0;
    while (days >= dpm[m]) { days -= dpm[m]; m++; }
    tm_buf.tm_mon  = m;
    tm_buf.tm_mday = (int)days + 1;
    tm_buf.tm_isdst = 0;
    tm_buf.__tm_gmtoff = 0;
    tm_buf.__tm_zone = "UTC";
    return &tm_buf;
}

struct tm* localtime(const time_t* t) { return gmtime(t); }
struct tm* localtime_r(const time_t* t, struct tm* out) {
    struct tm* r = gmtime(t);
    if (out && r) *out = *r;
    return out;
}

time_t mktime(struct tm* t) {
    if (!t) return (time_t)-1;
    int y = t->tm_year + 1900;
    long days = 0;
    for (int yy = 1970; yy < y; yy++) days += is_leap(yy) ? 366 : 365;
    const int* dpm = is_leap(y) ? days_per_month_leap : days_per_month_norm;
    for (int m = 0; m < t->tm_mon; m++) days += dpm[m];
    days += t->tm_mday - 1;
    return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}

size_t strftime(char* __restrict__ buf, size_t n,
                const char* __restrict__ fmt,
                const struct tm* __restrict__ t) {
    if (!buf || !n || !fmt || !t) return 0;
    size_t pos = 0;
    while (*fmt && pos + 1 < n) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        char tmp[16]; int tn = 0; int v = 0;
        switch (*fmt) {
        case 'Y': v = t->tm_year + 1900;
                  tmp[tn++] = '0' + (v/1000)%10;
                  tmp[tn++] = '0' + (v/100)%10;
                  tmp[tn++] = '0' + (v/10)%10;
                  tmp[tn++] = '0' + v%10; break;
        case 'm': v = t->tm_mon + 1;  tmp[tn++] = '0' + v/10; tmp[tn++] = '0' + v%10; break;
        case 'd': v = t->tm_mday;     tmp[tn++] = '0' + v/10; tmp[tn++] = '0' + v%10; break;
        case 'H': v = t->tm_hour;     tmp[tn++] = '0' + v/10; tmp[tn++] = '0' + v%10; break;
        case 'M': v = t->tm_min;      tmp[tn++] = '0' + v/10; tmp[tn++] = '0' + v%10; break;
        case 'S': v = t->tm_sec;      tmp[tn++] = '0' + v/10; tmp[tn++] = '0' + v%10; break;
        case '%': tmp[tn++] = '%'; break;
        default:  tmp[tn++] = '%'; tmp[tn++] = *fmt; break;
        }
        for (int i = 0; i < tn && pos + 1 < n; i++) buf[pos++] = tmp[i];
        if (*fmt) fmt++;
    }
    buf[pos] = 0;
    return pos;
}

static char asctime_buf[32];
char* asctime(const struct tm* t) {
    static const char* dn[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mn[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int wd = (t->tm_wday >= 0 && t->tm_wday < 7) ? t->tm_wday : 0;
    int mo = (t->tm_mon  >= 0 && t->tm_mon  < 12) ? t->tm_mon : 0;
    int y = t->tm_year + 1900;
    asctime_buf[0]='\0';
    /* day mon dd hh:mm:ss yyyy\n */
    char* p = asctime_buf;
    const char* s = dn[wd]; while (*s) *p++ = *s++; *p++ = ' ';
    s = mn[mo]; while (*s) *p++ = *s++; *p++ = ' ';
    *p++ = '0' + (t->tm_mday/10); *p++ = '0' + (t->tm_mday%10); *p++ = ' ';
    *p++ = '0' + (t->tm_hour/10); *p++ = '0' + (t->tm_hour%10); *p++ = ':';
    *p++ = '0' + (t->tm_min/10);  *p++ = '0' + (t->tm_min%10);  *p++ = ':';
    *p++ = '0' + (t->tm_sec/10);  *p++ = '0' + (t->tm_sec%10);  *p++ = ' ';
    *p++ = '0' + (y/1000)%10; *p++ = '0' + (y/100)%10;
    *p++ = '0' + (y/10)%10;   *p++ = '0' + y%10;
    *p++ = '\n'; *p = 0;
    return asctime_buf;
}

char* ctime(const time_t* t) { return asctime(localtime(t)); }
