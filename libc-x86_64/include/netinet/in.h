#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t     sin_family;
    in_port_t       sin_port;
    struct in_addr  sin_addr;
    unsigned char   sin_zero[8];
};

struct in6_addr {
    union {
        uint8_t  s6_addr[16];
        uint16_t s6_addr16[8];
        uint32_t s6_addr32[4];
    };
};

struct sockaddr_in6 {
    sa_family_t      sin6_family;
    in_port_t        sin6_port;
    uint32_t         sin6_flowinfo;
    struct in6_addr  sin6_addr;
    uint32_t         sin6_scope_id;
};

#define INADDR_ANY        ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK   ((in_addr_t)0x7F000001)
#define INADDR_BROADCAST  ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE       ((in_addr_t)0xFFFFFFFF)

uint16_t htons(uint16_t x);
uint16_t ntohs(uint16_t x);
uint32_t htonl(uint32_t x);
uint32_t ntohl(uint32_t x);

#ifdef __cplusplus
}
#endif
#endif
