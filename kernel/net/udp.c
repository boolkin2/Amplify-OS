// ============================================================================
// udp.c — реализация UDP
// ============================================================================

#include "udp.h"
#include "ipv4.h"
#include "netif.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --- Таблица биндов ---
#define UDP_MAX_BINDS 8

typedef struct {
    bool     used;
    uint16_t port;
    udp_rx_handler_t handler;
} udp_bind_t;

static udp_bind_t g_binds[UDP_MAX_BINDS];

// ----------------------------------------------------------------------------
// Helpers: BE/LE в памяти
// ----------------------------------------------------------------------------
static inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void put_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// ----------------------------------------------------------------------------
// UDP checksum с pseudo-header
// ----------------------------------------------------------------------------
static uint16_t udp_calc_checksum(uint32_t src_ip, uint32_t dst_ip,
                                  const void* udp_start, size_t udp_len) {
    // Pseudo-header: src_ip(4) | dst_ip(4) | zero(1) | proto(1) | udp_len(2).
    uint8_t pseudo[12];
    pseudo[0]  = (uint8_t)(src_ip >> 24);
    pseudo[1]  = (uint8_t)(src_ip >> 16);
    pseudo[2]  = (uint8_t)(src_ip >> 8);
    pseudo[3]  = (uint8_t)(src_ip);
    pseudo[4]  = (uint8_t)(dst_ip >> 24);
    pseudo[5]  = (uint8_t)(dst_ip >> 16);
    pseudo[6]  = (uint8_t)(dst_ip >> 8);
    pseudo[7]  = (uint8_t)(dst_ip);
    pseudo[8]  = 0;
    pseudo[9]  = 17;  // UDP protocol
    pseudo[10] = (uint8_t)(udp_len >> 8);
    pseudo[11] = (uint8_t)udp_len;

    // Общая сумма: pseudo + udp (header + data).
    // Делаем это через ручной accumulator по тем же правилам, что ipv4_checksum.
    uint32_t sum = 0;
    const uint8_t* p = pseudo;
    for (int i = 0; i < 12; i += 2) {
        sum += ((uint32_t)p[i] << 8) | (uint32_t)p[i+1];
    }
    p = (const uint8_t*)udp_start;
    size_t n = udp_len;
    while (n > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p   += 2;
        n   -= 2;
    }
    if (n == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

// ----------------------------------------------------------------------------
// Bind table
// ----------------------------------------------------------------------------
bool udp_bind(uint16_t port, udp_rx_handler_t handler) {
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_binds[i].used && g_binds[i].port == port) return false;
    }
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!g_binds[i].used) {
            g_binds[i].used    = true;
            g_binds[i].port    = port;
            g_binds[i].handler = handler;
            dbg_puts("[udp] bind port=");
            dbg_hex(port);
            dbg_puts("\n");
            return true;
        }
    }
    return false;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_binds[i].used && g_binds[i].port == port) {
            g_binds[i].used = false;
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// RX
// ----------------------------------------------------------------------------
void udp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len) {
    (void)dst_ip;
    if (len < UDP_HDR_LEN) return;
    const uint8_t* p = (const uint8_t*)payload;
    uint16_t src_port = be16(p + 0);
    uint16_t dst_port = be16(p + 2);
    uint16_t length   = be16(p + 4);
    // checksum при 0 — значит не считалась, пропускаем проверку.

    if (length > len) return;

    dbg_puts("[udp] rx ");
    netif_dump_ip4(src_ip);
    dbg_puts(":");
    dbg_hex(src_port);
    dbg_puts(" -> :");
    dbg_hex(dst_port);
    dbg_puts(" len=");
    dbg_hex(length);
    dbg_puts("\n");

    // Диспетчеризуем.
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_binds[i].used && g_binds[i].port == dst_port && g_binds[i].handler) {
            size_t data_len = length > UDP_HDR_LEN ? (size_t)(length - UDP_HDR_LEN) : 0;
            g_binds[i].handler(src_ip, src_port, dst_port,
                               p + UDP_HDR_LEN, data_len);
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// TX
// ----------------------------------------------------------------------------
bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void* payload, size_t plen) {
    if (plen > 1400) return false;  // с запасом от MTU

    uint8_t buf[UDP_HDR_LEN + 1400];
    put_be16(buf + 0, src_port);
    put_be16(buf + 2, dst_port);
    put_be16(buf + 4, (uint16_t)(UDP_HDR_LEN + plen));
    put_be16(buf + 6, 0);  // checksum = 0 пока

    if (payload && plen > 0) {
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++) buf[UDP_HDR_LEN + i] = src[i];
    }

    net_interface_t* iface = netif_get_default();
    uint32_t my_ip = iface ? iface->ip4_addr : 0;

    uint16_t csum = udp_calc_checksum(my_ip, dst_ip, buf, UDP_HDR_LEN + plen);
    // RFC: если результат 0, заменяем на 0xFFFF.
    if (csum == 0) csum = 0xFFFF;
    put_be16(buf + 6, csum);

    return ipv4_send(dst_ip, IPV4_PROTO_UDP, buf, UDP_HDR_LEN + plen);
}
