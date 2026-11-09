// ============================================================================
// socket.c — POSIX sockets (этап 40 группа C).
// ----------------------------------------------------------------------------
// Реальные обёртки над AOS_SYS_SOCKET/BIND/CONNECT/SEND/RECV/SENDTO/RECVFROM/
// SHUTDOWN/LISTEN/ACCEPT/SETSOCKOPT/GETSOCKOPT.
//
// Байтовый порядок: htons/ntohs/htonl/ntohl + inet_addr/inet_ntoa/inet_pton/
// inet_ntop остаются чисто userspace-функциями (были в этапе 39 и работают).
// ============================================================================
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include "../syscall/aos_arch.h"

int socket(int domain, int type, int protocol) {
    long r = __aos_syscall3(AOS_SYS_SOCKET, domain, type, protocol);
    return (int)__aos_set_errno(r);
}

int bind(int fd, const struct sockaddr* addr, socklen_t len) {
    long r = __aos_syscall3(AOS_SYS_BIND, fd, (long)addr, (long)len);
    return (int)__aos_set_errno(r);
}

int listen(int fd, int backlog) {
    long r = __aos_syscall2(AOS_SYS_LISTEN, fd, backlog);
    return (int)__aos_set_errno(r);
}

int accept(int fd, struct sockaddr* addr, socklen_t* len) {
    long r = __aos_syscall3(AOS_SYS_ACCEPT, fd, (long)addr, (long)len);
    return (int)__aos_set_errno(r);
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    long r = __aos_syscall3(AOS_SYS_CONNECT, fd, (long)addr, (long)len);
    return (int)__aos_set_errno(r);
}

ssize_t send(int fd, const void* buf, size_t len, int flags) {
    long r = __aos_syscall4(AOS_SYS_SEND, fd, (long)buf, (long)len, flags);
    return (ssize_t)__aos_set_errno(r);
}

ssize_t recv(int fd, void* buf, size_t len, int flags) {
    long r = __aos_syscall4(AOS_SYS_RECV, fd, (long)buf, (long)len, flags);
    return (ssize_t)__aos_set_errno(r);
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tlen) {
    long r = __aos_syscall6(AOS_SYS_SENDTO, fd, (long)buf, (long)len,
                            flags, (long)to, (long)tlen);
    return (ssize_t)__aos_set_errno(r);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* flen) {
    long r = __aos_syscall6(AOS_SYS_RECVFROM, fd, (long)buf, (long)len,
                            flags, (long)from, (long)flen);
    return (ssize_t)__aos_set_errno(r);
}

int shutdown(int fd, int how) {
    long r = __aos_syscall2(AOS_SYS_SHUTDOWN, fd, how);
    return (int)__aos_set_errno(r);
}

int setsockopt(int fd, int level, int opt, const void* val, socklen_t len) {
    long r = __aos_syscall5(AOS_SYS_SETSOCKOPT, fd, level, opt,
                            (long)val, (long)len);
    return (int)__aos_set_errno(r);
}

int getsockopt(int fd, int level, int opt, void* val, socklen_t* len) {
    long r = __aos_syscall5(AOS_SYS_GETSOCKOPT, fd, level, opt,
                            (long)val, (long)len);
    return (int)__aos_set_errno(r);
}

// ---- Byte-order и address-conversion (userspace, без syscall) --------------
// Те же что и в stage 39 — переиспользуем.
uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
uint16_t ntohs(uint16_t x) { return htons(x); }
uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24)
         | ((x & 0x0000FF00u) <<  8)
         | ((x & 0x00FF0000u) >>  8)
         | ((x & 0xFF000000u) >> 24);
}
uint32_t ntohl(uint32_t x) { return htonl(x); }

in_addr_t inet_addr(const char* cp) {
    if (!cp) return INADDR_NONE;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int n = 0;
    const char* s = cp;
    unsigned* tab[4] = { &a, &b, &c, &d };
    while (n < 4) {
        unsigned int v = 0;
        if (*s < '0' || *s > '9') return INADDR_NONE;
        while (*s >= '0' && *s <= '9') { v = v*10 + (unsigned)(*s - '0'); s++; }
        if (v > 255) return INADDR_NONE;
        *tab[n++] = v;
        if (*s == 0) break;
        if (*s != '.') return INADDR_NONE;
        s++;
    }
    if (n != 4 || *s != 0) return INADDR_NONE;
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

static char ntoa_buf[16];
char* inet_ntoa(struct in_addr in) {
    uint32_t h = ntohl(in.s_addr);
    unsigned a = (h >> 24) & 0xFF, b = (h >> 16) & 0xFF,
             c = (h >> 8) & 0xFF,  d = h & 0xFF;
    char* p = ntoa_buf;
    #define EMIT(v) do { unsigned vv = (v); \
        if (vv >= 100) { *p++ = '0' + (vv/100); vv %= 100; *p++ = '0' + (vv/10); *p++ = '0' + vv%10; } \
        else if (vv >= 10) { *p++ = '0' + vv/10; *p++ = '0' + vv%10; } \
        else { *p++ = '0' + vv; } } while (0)
    EMIT(a); *p++ = '.';
    EMIT(b); *p++ = '.';
    EMIT(c); *p++ = '.';
    EMIT(d); *p = 0;
    #undef EMIT
    return ntoa_buf;
}

int inet_pton(int af, const char* src, void* dst) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    in_addr_t a = inet_addr(src);
    if (a == INADDR_NONE) return 0;
    *(in_addr_t*)dst = a;
    return 1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t len) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    if (len < 16) { errno = ENOSPC; return NULL; }
    struct in_addr ia = { *(const in_addr_t*)src };
    char* s = inet_ntoa(ia);
    int i = 0;
    while (s[i] && (socklen_t)i + 1 < len) { dst[i] = s[i]; i++; }
    dst[i] = 0;
    return dst;
}
