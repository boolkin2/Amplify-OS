// ============================================================================
// tcp.c — минимальный TCP-клиент
// ----------------------------------------------------------------------------
// Основная идея:
//   • Один статический сокет g_sock.
//   • tcp_rx() вызывается из IRQ (через ipv4_rx), меняет state и будит
//     вызывающий поток через volatile-флаги.
//   • tcp_connect/tcp_send/tcp_close работают в контексте задачи, ждут
//     изменения state через hlt-spin в цикле.
//
// Потоки safe только для одного потока, который работает с сокетом:
// так реализован обычный userland «connect → send → recv → close».
// ============================================================================

#include "tcp.h"
#include "ipv4.h"
#include "netif.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------
#define TCP_HDR_LEN     20
#define TCP_WINDOW      1460
#define TCP_RX_BUF_SIZE 4096
#define TCP_LOCAL_PORT  0xC000   // фиксированный source port

// Wait-loop: ~2 секунды при 100 Гц (считаем, что где-то есть таймер).
#define TCP_CONNECT_TIMEOUT  1000000

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
typedef struct {
    volatile tcp_state_t state;

    uint32_t  remote_ip;
    uint16_t  remote_port;
    uint16_t  local_port;

    uint32_t  snd_nxt;     // Следующий seq к отправке
    uint32_t  snd_una;     // Неподтверждённый
    uint32_t  rcv_nxt;     // Ожидаемый seq от peer

    uint8_t   rx_buf[TCP_RX_BUF_SIZE];
    volatile size_t rx_head;   // сколько уже в буфере (write ptr)
    size_t    rx_read;         // прочитано пользователем (read ptr)
} tcp_socket_t;

static tcp_socket_t g_sock;

// ----------------------------------------------------------------------------
// BE helpers
// ----------------------------------------------------------------------------
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static inline void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// ----------------------------------------------------------------------------
// TCP checksum (с pseudo-header, совпадает по алгоритму с UDP).
// ----------------------------------------------------------------------------
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void* tcp_start, size_t tcp_len) {
    uint8_t pseudo[12];
    put_be32(pseudo + 0, src_ip);
    put_be32(pseudo + 4, dst_ip);
    pseudo[8]  = 0;
    pseudo[9]  = IPV4_PROTO_TCP;
    pseudo[10] = (uint8_t)(tcp_len >> 8);
    pseudo[11] = (uint8_t)tcp_len;

    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += ((uint32_t)pseudo[i] << 8) | pseudo[i+1];
    const uint8_t* p = (const uint8_t*)tcp_start;
    size_t n = tcp_len;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

// ----------------------------------------------------------------------------
// Сборка и отправка сегмента
// ----------------------------------------------------------------------------
static bool send_segment(uint8_t flags, const void* data, size_t dlen) {
    if (dlen > TCP_WINDOW) dlen = TCP_WINDOW;

    uint8_t seg[TCP_HDR_LEN + TCP_WINDOW];
    put_be16(seg + 0,  g_sock.local_port);
    put_be16(seg + 2,  g_sock.remote_port);
    put_be32(seg + 4,  g_sock.snd_nxt);
    put_be32(seg + 8,  g_sock.rcv_nxt);
    seg[12] = 0x50;  // data offset = 5*4 = 20 байт, reserved=0
    seg[13] = flags;
    put_be16(seg + 14, TCP_WINDOW);
    put_be16(seg + 16, 0);           // checksum
    put_be16(seg + 18, 0);           // urgent ptr

    if (data && dlen > 0) {
        const uint8_t* s = (const uint8_t*)data;
        for (size_t i = 0; i < dlen; i++) seg[TCP_HDR_LEN + i] = s[i];
    }

    net_interface_t* iface = netif_get_default();
    uint32_t my_ip = iface ? iface->ip4_addr : 0;
    size_t   tlen  = TCP_HDR_LEN + dlen;
    uint16_t csum  = tcp_checksum(my_ip, g_sock.remote_ip, seg, tlen);
    put_be16(seg + 16, csum);

    // SYN/FIN тоже продвигают seq на 1 — в логическом смысле.
    if (flags & TCP_SYN) g_sock.snd_nxt++;
    if (flags & TCP_FIN) g_sock.snd_nxt++;
    g_sock.snd_nxt += dlen;

    return ipv4_send(g_sock.remote_ip, IPV4_PROTO_TCP, seg, tlen);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
tcp_state_t tcp_current_state(void) { return g_sock.state; }

bool tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    // Сбрасываем сокет.
    for (size_t i = 0; i < sizeof(g_sock); i++) ((uint8_t*)&g_sock)[i] = 0;
    g_sock.state       = TCP_STATE_CLOSED;
    g_sock.remote_ip   = dst_ip;
    g_sock.remote_port = dst_port;
    g_sock.local_port  = TCP_LOCAL_PORT;
    g_sock.snd_nxt     = 0x1000;     // начальный ISN (детерминированный)
    g_sock.snd_una     = g_sock.snd_nxt;
    g_sock.rcv_nxt     = 0;

    dbg_puts("[tcp] SYN to ");
    netif_dump_ip4(dst_ip);
    dbg_puts(":");
    dbg_hex(dst_port);
    dbg_puts("\n");

    if (!send_segment(TCP_SYN, NULL, 0)) {
        return false;
    }
    g_sock.state = TCP_STATE_SYN_SENT;

    // Спин-ждём ESTABLISHED.
    for (int i = 0; i < TCP_CONNECT_TIMEOUT; i++) {
        if (g_sock.state == TCP_STATE_ESTABLISHED) return true;
        if (g_sock.state == TCP_STATE_CLOSED)      return false;
        __asm__ volatile ("hlt");
    }
    dbg_puts("[tcp] connect timeout\n");
    g_sock.state = TCP_STATE_CLOSED;
    return false;
}

size_t tcp_send(const void* data, size_t len) {
    if (g_sock.state != TCP_STATE_ESTABLISHED) return 0;
    if (len > TCP_WINDOW) len = TCP_WINDOW;
    if (!send_segment(TCP_PSH | TCP_ACK, data, len)) return 0;
    return len;
}

bool tcp_recv(void* buf, size_t cap, size_t* got) {
    if (!got) return false;
    size_t have = g_sock.rx_head - g_sock.rx_read;
    size_t n = have < cap ? have : cap;
    if (n > 0) {
        uint8_t* out = (uint8_t*)buf;
        for (size_t i = 0; i < n; i++) out[i] = g_sock.rx_buf[g_sock.rx_read + i];
        g_sock.rx_read += n;
    }
    *got = n;
    return n > 0;
}

void tcp_close(void) {
    if (g_sock.state != TCP_STATE_ESTABLISHED &&
        g_sock.state != TCP_STATE_CLOSE_WAIT) {
        g_sock.state = TCP_STATE_CLOSED;
        return;
    }
    dbg_puts("[tcp] FIN\n");
    send_segment(TCP_FIN | TCP_ACK, NULL, 0);
    if (g_sock.state == TCP_STATE_ESTABLISHED) {
        g_sock.state = TCP_STATE_FIN_WAIT_1;
    } else {
        g_sock.state = TCP_STATE_LAST_ACK;
    }
}

// ----------------------------------------------------------------------------
// RX: обработка входящего сегмента
// ----------------------------------------------------------------------------
void tcp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len) {
    (void)dst_ip;
    if (len < TCP_HDR_LEN) return;
    const uint8_t* p = (const uint8_t*)payload;

    uint16_t src_port = be16(p + 0);
    uint16_t dst_port = be16(p + 2);
    uint32_t seq      = be32(p + 4);
    uint32_t ack      = be32(p + 8);
    uint8_t  off_res  = p[12];
    uint8_t  flags    = p[13];
    uint8_t  data_off = (off_res >> 4) * 4;

    if (data_off < TCP_HDR_LEN || data_off > len) return;

    // Фильтр по сокету.
    if (g_sock.state == TCP_STATE_CLOSED)        return;
    if (src_ip   != g_sock.remote_ip)            return;
    if (src_port != g_sock.remote_port)          return;
    if (dst_port != g_sock.local_port)           return;

    size_t data_len = len - data_off;
    const uint8_t* data = p + data_off;

    dbg_puts("[tcp] rx flags=0x");
    dbg_hex(flags);
    dbg_puts(" data=");
    dbg_hex((uint32_t)data_len);
    dbg_puts(" state=");
    dbg_hex(g_sock.state);
    dbg_puts("\n");

    // RST — полностью закрыли.
    if (flags & TCP_RST) {
        g_sock.state = TCP_STATE_CLOSED;
        return;
    }

    // --- SYN-ACK ---
    if (g_sock.state == TCP_STATE_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            g_sock.rcv_nxt = seq + 1;
            g_sock.snd_una = ack;
            // Отправляем чистый ACK.
            send_segment(TCP_ACK, NULL, 0);
            g_sock.state = TCP_STATE_ESTABLISHED;
            dbg_puts("[tcp] ESTABLISHED\n");
            return;
        }
    }

    // --- ESTABLISHED и т.п. ---
    if (g_sock.state >= TCP_STATE_ESTABLISHED) {
        // Данные, приехавшие с правильным seq.
        if (data_len > 0 && seq == g_sock.rcv_nxt) {
            size_t room = TCP_RX_BUF_SIZE - g_sock.rx_head;
            size_t n = data_len < room ? data_len : room;
            for (size_t i = 0; i < n; i++) {
                g_sock.rx_buf[g_sock.rx_head + i] = data[i];
            }
            g_sock.rx_head += n;
            g_sock.rcv_nxt += n;
            // Отправляем ACK.
            send_segment(TCP_ACK, NULL, 0);
        } else if (data_len > 0) {
            // Не тот seq — просто дублируем ACK (простейший вариант).
            send_segment(TCP_ACK, NULL, 0);
        }

        // Обновление snd_una.
        if (flags & TCP_ACK) {
            if ((int32_t)(ack - g_sock.snd_una) > 0) g_sock.snd_una = ack;
        }

        // FIN от peer.
        if (flags & TCP_FIN) {
            g_sock.rcv_nxt++;  // FIN сам по себе занимает 1 в seq
            send_segment(TCP_ACK, NULL, 0);
            switch (g_sock.state) {
                case TCP_STATE_ESTABLISHED:
                    g_sock.state = TCP_STATE_CLOSE_WAIT;
                    break;
                case TCP_STATE_FIN_WAIT_1:
                case TCP_STATE_FIN_WAIT_2:
                    g_sock.state = TCP_STATE_TIME_WAIT;
                    // Упрощённо: сразу закрываем.
                    g_sock.state = TCP_STATE_CLOSED;
                    break;
                default:
                    break;
            }
        }

        // Если мы в FIN_WAIT_1 и приехал ACK на наш FIN.
        if (g_sock.state == TCP_STATE_FIN_WAIT_1 && (flags & TCP_ACK)) {
            g_sock.state = TCP_STATE_FIN_WAIT_2;
        }
        // LAST_ACK → CLOSED.
        if (g_sock.state == TCP_STATE_LAST_ACK && (flags & TCP_ACK)) {
            g_sock.state = TCP_STATE_CLOSED;
        }
    }
}
