// ============================================================================
// sys/socket.h — POSIX сокеты. На 39-м этапе — заголовки + stubs (-ENOSYS).
// Полноценные сокеты придут с TLS-стеком на 46-м.
// ============================================================================
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC      0
#define AF_UNIX        1
#define AF_LOCAL       AF_UNIX
#define AF_INET        2
#define AF_INET6      10

#define PF_UNSPEC      AF_UNSPEC
#define PF_UNIX        AF_UNIX
#define PF_INET        AF_INET
#define PF_INET6       AF_INET6

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3

#define IPPROTO_IP     0
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP   17

#define SOL_SOCKET    1
#define SO_REUSEADDR  2

#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

int    socket  (int family, int type, int proto);
int    bind    (int s, const struct sockaddr* addr, socklen_t len);
int    listen  (int s, int backlog);
int    accept  (int s, struct sockaddr* addr, socklen_t* len);
int    connect (int s, const struct sockaddr* addr, socklen_t len);
ssize_t send   (int s, const void* buf, size_t n, int flags);
ssize_t recv   (int s, void* buf, size_t n, int flags);
ssize_t sendto (int s, const void* buf, size_t n, int flags,
                const struct sockaddr* to, socklen_t tolen);
ssize_t recvfrom(int s, void* buf, size_t n, int flags,
                 struct sockaddr* from, socklen_t* fromlen);
int    shutdown(int s, int how);
int    setsockopt(int s, int level, int opt, const void* val, socklen_t len);
int    getsockopt(int s, int level, int opt, void* val, socklen_t* len);

#ifdef __cplusplus
}
#endif
#endif
