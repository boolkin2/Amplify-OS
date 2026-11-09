// ============================================================================
// sock.c — POSIX-подобные сокеты для AMPLIFY-OS x86_64 (этап 40 группа C).
// ----------------------------------------------------------------------------
// Архитектура:
//   * Пул из AOS_MAX_SOCKETS (16) слотов `aos_socket_t`.
//   * Тип TCP: существующий tcp.c поддерживает ТОЛЬКО один активный сокет
//     глобально, поэтому пул допускает только один TCP_ESTABLISHED одновременно.
//     Остальные TCP-слоты могут быть в CLOSED/bound-but-not-connected состоянии.
//   * Тип UDP: каждый слот может быть привязан к своему порту через udp_bind.
//     recvfrom складывает входящий пакет в кольцевой буфер слота.
//   * Типы IPv6/AF_UNIX/SOCK_RAW не поддерживаются — ENOSYS/EAFNOSUPPORT.
//
// listen/accept — минимальная реализация P2: listen() переключает socket в
// режим server, а accept() возвращает -ENOSYS (серверный режим tcp.c не
// реализован). Client-side connect/send/recv — полная реализация.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "serial.h"
#include "heap.h"
#include "vfs.h"
#include "syscall.h"
#include "tcp.h"
#include "udp.h"
#include "timer.h"
// [этап 46] sandboxing: net-ns gate
#include "task.h"
#include "ns.h"

#define AOS_MAX_SOCKETS      16
#define UDP_RX_BUF_SIZE      4096

// AF / SOCK constants (зеркало libc-x86_64/include/sys/socket.h — стандарт Linux).
#define AOS_AF_INET          2
#define AOS_SOCK_STREAM      1
#define AOS_SOCK_DGRAM       2

// sockaddr_in layout.
typedef struct __attribute__((packed)) {
    uint16_t sin_family;
    uint16_t sin_port;     // big-endian
    uint32_t sin_addr;     // big-endian
    uint8_t  sin_zero[8];
} sockaddr_in_t;

typedef enum {
    SOCK_UNUSED = 0,
    SOCK_TCP_NEW,           // socket() создан, ничего больше
    SOCK_TCP_BOUND,
    SOCK_TCP_CONNECTED,
    SOCK_TCP_LISTENING,
    SOCK_UDP_NEW,
    SOCK_UDP_BOUND,
} sock_state_t;

typedef struct {
    sock_state_t state;
    int          domain;
    int          type;
    int          protocol;

    uint32_t     local_ip;
    uint16_t     local_port;
    uint32_t     peer_ip;
    uint16_t     peer_port;

    // Только для UDP: кольцевой буфер для входящих датаграмм.
    uint8_t*     udp_rx_buf;        // выделяется в bind
    uint32_t     udp_rx_head;       // позиция записи
    uint32_t     udp_rx_tail;       // позиция чтения
    uint32_t     udp_rx_cap;
    // Источник последнего пакета (для recvfrom).
    uint32_t     udp_src_ip;
    uint16_t     udp_src_port;
} aos_socket_t;

static aos_socket_t g_sockets[AOS_MAX_SOCKETS];

// Для UDP-rx handler'а нужно знать, какому слоту принадлежит порт.
// Храним мапу port->slot_idx (простая линейная таблица).
static int find_socket_by_port(uint16_t port) {
    for (int i = 0; i < AOS_MAX_SOCKETS; i++) {
        if (g_sockets[i].state == SOCK_UDP_BOUND && g_sockets[i].local_port == port) {
            return i;
        }
    }
    return -1;
}

// Общий UDP-handler для всех bind'ов — складывает данные в буфер слота.
static void aos_udp_rx_handler(uint32_t src_ip, uint16_t src_port,
                               uint16_t dst_port,
                               const void* data, size_t len) {
    int idx = find_socket_by_port(dst_port);
    if (idx < 0) return;
    aos_socket_t* s = &g_sockets[idx];
    if (!s->udp_rx_buf) return;
    // Проталкиваем: сначала 8 байт header'а (src_ip + src_port + len), затем данные.
    if (len + 8 > s->udp_rx_cap - (s->udp_rx_head - s->udp_rx_tail)) return; // drop
    uint8_t hdr[8];
    *(uint32_t*)&hdr[0] = src_ip;
    *(uint16_t*)&hdr[4] = src_port;
    *(uint16_t*)&hdr[6] = (uint16_t)len;
    for (int i = 0; i < 8; i++) s->udp_rx_buf[(s->udp_rx_head + i) % s->udp_rx_cap] = hdr[i];
    s->udp_rx_head += 8;
    for (size_t i = 0; i < len; i++) {
        s->udp_rx_buf[(s->udp_rx_head + i) % s->udp_rx_cap] = ((const uint8_t*)data)[i];
    }
    s->udp_rx_head += len;
}

// ---- Выделение слота -------------------------------------------------------
static int alloc_socket_slot(void) {
    for (int i = 0; i < AOS_MAX_SOCKETS; i++) {
        if (g_sockets[i].state == SOCK_UNUSED) return i;
    }
    return -1;
}

static aos_socket_t* sock_from_fd(int fd) {
    if (vfs_fd_kind(fd) != VFS_FD_SOCKET) return 0;
    int idx = vfs_fd_socket_idx(fd);
    if (idx < 0 || idx >= AOS_MAX_SOCKETS) return 0;
    return &g_sockets[idx];
}

// ============================================================================
// Публичный API (вызывается из syscall.c)
// ============================================================================

int64_t sock_sys_socket(int domain, int type, int protocol) {
    if (domain != AOS_AF_INET) return -AOS_EAFNOSUPPORT;
    if (type != AOS_SOCK_STREAM && type != AOS_SOCK_DGRAM) return -AOS_EINVAL;

    // [этап 46] Net-namespace gate: задача в isolated net-ns не может
    // создавать internet-сокеты (loopback пока не реализован).
    if (!ns_net_allowed(task_current())) return -AOS_EACCES;

    int idx = alloc_socket_slot();
    if (idx < 0) return -AOS_EMFILE;
    aos_socket_t* s = &g_sockets[idx];
    s->state     = (type == AOS_SOCK_STREAM) ? SOCK_TCP_NEW : SOCK_UDP_NEW;
    s->domain    = domain;
    s->type      = type;
    s->protocol  = protocol;
    s->local_ip  = 0;
    s->local_port = 0;
    s->peer_ip   = 0;
    s->peer_port = 0;
    s->udp_rx_buf = 0;
    s->udp_rx_head = s->udp_rx_tail = 0;
    s->udp_rx_cap = 0;

    int fd = vfs_alloc_socket_fd(idx);
    if (fd < 0) { s->state = SOCK_UNUSED; return -AOS_EMFILE; }
    return fd;
}

int64_t sock_sys_bind(int fd, const sockaddr_in_t* addr, int addrlen) {
    if (!addr || addrlen < (int)sizeof(sockaddr_in_t)) return -AOS_EINVAL;
    if (addr->sin_family != AOS_AF_INET) return -AOS_EAFNOSUPPORT;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    uint16_t port = __builtin_bswap16(addr->sin_port);
    s->local_port = port;
    s->local_ip   = __builtin_bswap32(addr->sin_addr);

    if (s->type == AOS_SOCK_DGRAM) {
        // Аллоцируем кольцевой буфер для rx.
        s->udp_rx_buf = (uint8_t*)kmalloc(UDP_RX_BUF_SIZE);
        if (!s->udp_rx_buf) return -AOS_ENOMEM;
        s->udp_rx_cap = UDP_RX_BUF_SIZE;
        s->state = SOCK_UDP_BOUND;
        if (!udp_bind(port, aos_udp_rx_handler)) {
            kfree(s->udp_rx_buf); s->udp_rx_buf = 0;
            return -AOS_EADDRINUSE;
        }
    } else {
        s->state = SOCK_TCP_BOUND;
    }
    return 0;
}

int64_t sock_sys_connect(int fd, const sockaddr_in_t* addr, int addrlen) {
    if (!addr || addrlen < (int)sizeof(sockaddr_in_t)) return -AOS_EINVAL;
    if (addr->sin_family != AOS_AF_INET) return -AOS_EAFNOSUPPORT;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;

    uint32_t ip   = __builtin_bswap32(addr->sin_addr);
    uint16_t port = __builtin_bswap16(addr->sin_port);
    s->peer_ip   = ip;
    s->peer_port = port;

    if (s->type == AOS_SOCK_STREAM) {
        // Глобальный tcp.c — один сокет одновременно.
        if (!tcp_connect(ip, port)) return -AOS_ECONNREFUSED;
        // Ожидаем ESTABLISHED до таймаута 3 сек.
        uint64_t deadline = timer_get_uptime_ms() + 3000;
        while (tcp_current_state() != TCP_STATE_ESTABLISHED
               && timer_get_uptime_ms() < deadline) {
            timer_sleep(10);
        }
        if (tcp_current_state() != TCP_STATE_ESTABLISHED) return -AOS_ETIMEDOUT;
        s->state = SOCK_TCP_CONNECTED;
        return 0;
    }
    // UDP: connect просто фиксирует peer; рассылать можно через send().
    s->state = SOCK_UDP_BOUND;
    return 0;
}

int64_t sock_sys_send(int fd, const void* buf, uint64_t len, int flags) {
    (void)flags;
    if (!buf) return -AOS_EFAULT;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->type == AOS_SOCK_STREAM) {
        if (s->state != SOCK_TCP_CONNECTED) return -AOS_ENOTCONN;
        size_t sent = tcp_send(buf, (size_t)len);
        return (int64_t)sent;
    }
    // UDP send() без явного peer — это connect-ранее-зафиксированный.
    if (s->peer_ip == 0 || s->peer_port == 0) return -AOS_EDESTADDRREQ;
    bool ok = udp_send(s->peer_ip, s->peer_port, s->local_port, buf, (size_t)len);
    return ok ? (int64_t)len : -AOS_EIO;
}

int64_t sock_sys_recv(int fd, void* buf, uint64_t len, int flags) {
    (void)flags;
    if (!buf) return -AOS_EFAULT;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;

    if (s->type == AOS_SOCK_STREAM) {
        if (s->state != SOCK_TCP_CONNECTED) return -AOS_ENOTCONN;
        size_t got = 0;
        // Блокирующее чтение: ждём хотя бы один байт или close.
        uint64_t deadline = timer_get_uptime_ms() + 5000;
        while (got == 0 && tcp_current_state() == TCP_STATE_ESTABLISHED
               && timer_get_uptime_ms() < deadline) {
            if (tcp_recv(buf, (size_t)len, &got) && got > 0) break;
            timer_sleep(5);
        }
        return (int64_t)got;
    }
    // UDP recv: без явного sender-адреса — просто читаем следующую датаграмму.
    // Доступно в буфере: head - tail. Если 0 — ждём до 2 сек (неблокирующий
    // режим не реализован на 40).
    uint64_t deadline = timer_get_uptime_ms() + 2000;
    while (s->udp_rx_head == s->udp_rx_tail && timer_get_uptime_ms() < deadline) {
        timer_sleep(5);
    }
    if (s->udp_rx_head == s->udp_rx_tail) return 0;    // no data
    uint8_t hdr[8];
    for (int i = 0; i < 8; i++) hdr[i] = s->udp_rx_buf[(s->udp_rx_tail + i) % s->udp_rx_cap];
    s->udp_rx_tail += 8;
    uint16_t dlen = *(uint16_t*)&hdr[6];
    if (dlen > len) dlen = (uint16_t)len;
    for (uint16_t i = 0; i < dlen; i++) {
        ((uint8_t*)buf)[i] = s->udp_rx_buf[(s->udp_rx_tail + i) % s->udp_rx_cap];
    }
    s->udp_rx_tail += dlen;
    return (int64_t)dlen;
}

int64_t sock_sys_listen(int fd, int backlog) {
    (void)backlog;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->type != AOS_SOCK_STREAM) return -AOS_EOPNOTSUPP;
    // [47.5] Один listener одновременно через глобальный tcp.c.
    if (s->local_port == 0) return -AOS_EINVAL;
    if (!tcp_listen(s->local_port)) return -AOS_EADDRINUSE;
    s->state = SOCK_TCP_LISTENING;
    return 0;
}

int64_t sock_sys_accept(int fd, sockaddr_in_t* addr, int* addrlen) {
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->state != SOCK_TCP_LISTENING) return -AOS_EINVAL;
    uint32_t peer_ip;
    uint16_t peer_port;
    if (!tcp_accept(&peer_ip, &peer_port, 0)) return -AOS_ECONNREFUSED;
    // Возвращаем accept'нутый socket — но в нашем single-connection tcp.c
    // это тот же fd.  В будущем (multi-socket tcp) — выдадим новый.
    s->state     = SOCK_TCP_CONNECTED;
    s->peer_ip   = peer_ip;
    s->peer_port = peer_port;
    if (addr && addrlen && *addrlen >= (int)sizeof(sockaddr_in_t)) {
        addr->sin_family = AOS_AF_INET;
        addr->sin_addr   = __builtin_bswap32(peer_ip);
        addr->sin_port   = __builtin_bswap16(peer_port);
        for (int i = 0; i < 8; i++) addr->sin_zero[i] = 0;
        *addrlen = sizeof(sockaddr_in_t);
    }
    return fd;   // тот же fd, потому что один TCP-socket в kernel
}

int64_t sock_sys_shutdown(int fd, int how) {
    (void)how;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->type == AOS_SOCK_STREAM && s->state == SOCK_TCP_CONNECTED) {
        tcp_close();
    }
    return 0;
}

int64_t sock_sys_sendto(int fd, const void* buf, uint64_t len, int flags,
                        const sockaddr_in_t* to, int tolen) {
    (void)flags;
    if (!buf || !to || tolen < (int)sizeof(sockaddr_in_t)) return -AOS_EINVAL;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->type != AOS_SOCK_DGRAM) return -AOS_EOPNOTSUPP;
    uint32_t ip   = __builtin_bswap32(to->sin_addr);
    uint16_t port = __builtin_bswap16(to->sin_port);
    bool ok = udp_send(ip, port, s->local_port ? s->local_port : 32768,
                       buf, (size_t)len);
    return ok ? (int64_t)len : -AOS_EIO;
}

int64_t sock_sys_recvfrom(int fd, void* buf, uint64_t len, int flags,
                          sockaddr_in_t* from, int* fromlen) {
    (void)flags;
    if (!buf) return -AOS_EFAULT;
    aos_socket_t* s = sock_from_fd(fd);
    if (!s) return -AOS_EBADF;
    if (s->type != AOS_SOCK_DGRAM) return -AOS_EOPNOTSUPP;
    uint64_t deadline = timer_get_uptime_ms() + 2000;
    while (s->udp_rx_head == s->udp_rx_tail && timer_get_uptime_ms() < deadline) {
        timer_sleep(5);
    }
    if (s->udp_rx_head == s->udp_rx_tail) return 0;
    uint8_t hdr[8];
    for (int i = 0; i < 8; i++) hdr[i] = s->udp_rx_buf[(s->udp_rx_tail + i) % s->udp_rx_cap];
    s->udp_rx_tail += 8;
    uint32_t src_ip   = *(uint32_t*)&hdr[0];
    uint16_t src_port = *(uint16_t*)&hdr[4];
    uint16_t dlen     = *(uint16_t*)&hdr[6];
    if (dlen > len) dlen = (uint16_t)len;
    for (uint16_t i = 0; i < dlen; i++) {
        ((uint8_t*)buf)[i] = s->udp_rx_buf[(s->udp_rx_tail + i) % s->udp_rx_cap];
    }
    s->udp_rx_tail += dlen;
    if (from && fromlen && *fromlen >= (int)sizeof(sockaddr_in_t)) {
        from->sin_family = AOS_AF_INET;
        from->sin_addr   = __builtin_bswap32(src_ip);
        from->sin_port   = __builtin_bswap16(src_port);
        for (int i = 0; i < 8; i++) from->sin_zero[i] = 0;
        *fromlen = sizeof(sockaddr_in_t);
    }
    return (int64_t)dlen;
}

int64_t sock_sys_setsockopt(int fd, int level, int optname,
                            const void* optval, int optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;   // no-op — реальных опций не реализуем на 40
}

int64_t sock_sys_getsockopt(int fd, int level, int optname,
                            void* optval, int* optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

// ---- Ресурс-cleanup при close — вызывается vfs_close'ом косвенно ---------
// (vfs.c не знает о сокетах, поэтому мы не делаем explicit cleanup здесь;
// slot остаётся занятым, пока не будет shutdown()'нут. Для 40 это приемлемо —
// только shutdown() освобождает ресурсы tcp/udp.)
