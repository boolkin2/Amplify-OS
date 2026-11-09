// ============================================================================
// dhcp.c — реализация DHCP-клиента
// ----------------------------------------------------------------------------
// Формат DHCP/BOOTP пакета (RFC 2131):
//   op(1) htype(1) hlen(1) hops(1)
//   xid(4) secs(2) flags(2)
//   ciaddr(4) yiaddr(4) siaddr(4) giaddr(4)
//   chaddr(16)     — первые 6 байт = MAC клиента
//   sname(64) file(128)
//   options(var, начинается с magic cookie 0x63825363)
//
// Мы шлём DISCOVER как broadcast (src=0.0.0.0, dst=255.255.255.255),
// потому что на момент DISCOVER у нас ещё нет IP.
// ============================================================================

#include "dhcp.h"
#include "udp.h"
#include "netif.h"
#include "arp.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Константы DHCP
// ----------------------------------------------------------------------------
#define BOOTP_REQUEST   1
#define BOOTP_REPLY     2
#define DHCP_HTYPE_ETH  1
#define DHCP_HLEN_ETH   6
#define DHCP_MAGIC      0x63825363u

#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6

#define OPT_SUBNET      1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_REQUESTED   50
#define OPT_LEASE       51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

#define DHCP_XID        0x11223344u

// ----------------------------------------------------------------------------
// Состояние клиента
// ----------------------------------------------------------------------------
typedef enum {
    DHCP_ST_INIT = 0,
    DHCP_ST_SELECTING,   // отправили DISCOVER, ждём OFFER
    DHCP_ST_REQUESTING,  // отправили REQUEST, ждём ACK
    DHCP_ST_BOUND,
} dhcp_state_t;

static struct {
    dhcp_state_t state;
    uint32_t     offered_ip;      // из OFFER
    uint32_t     server_id;
    uint32_t     subnet;
    uint32_t     router;
    uint32_t     dns;
    uint32_t     lease;
} g_dhcp;

// ----------------------------------------------------------------------------
// BE helpers
// ----------------------------------------------------------------------------
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// ----------------------------------------------------------------------------
// Построение DHCP-пакета
// ----------------------------------------------------------------------------
// Возвращает длину. buf должен быть >= 300 байт (RFC минимум).
static size_t build_packet(uint8_t* buf, size_t cap, uint8_t msg_type,
                           uint32_t requested_ip, uint32_t server_ip) {
    if (cap < 300) return 0;
    net_interface_t* iface = netif_get_default();
    if (!iface) return 0;

    // Зануляем весь BOOTP-заголовок.
    for (size_t i = 0; i < 240; i++) buf[i] = 0;

    buf[0] = BOOTP_REQUEST;
    buf[1] = DHCP_HTYPE_ETH;
    buf[2] = DHCP_HLEN_ETH;
    buf[3] = 0;                        // hops
    put_be32(buf + 4, DHCP_XID);       // xid
    // secs, flags, ciaddr, yiaddr, siaddr, giaddr = 0

    // chaddr
    for (int i = 0; i < 6; i++) buf[28 + i] = iface->mac[i];

    // Magic cookie
    put_be32(buf + 236, DHCP_MAGIC);

    // Options: MessageType, затем specific, затем End.
    size_t off = 240;
    buf[off++] = OPT_MSG_TYPE; buf[off++] = 1; buf[off++] = msg_type;

    if (msg_type == DHCP_REQUEST) {
        // Requested IP
        buf[off++] = OPT_REQUESTED; buf[off++] = 4;
        put_be32(buf + off, requested_ip); off += 4;
        // Server ID
        buf[off++] = OPT_SERVER_ID; buf[off++] = 4;
        put_be32(buf + off, server_ip); off += 4;
    }

    // Parameter Request List — что хотим узнать
    buf[off++] = OPT_PARAM_LIST; buf[off++] = 4;
    buf[off++] = OPT_SUBNET;
    buf[off++] = OPT_ROUTER;
    buf[off++] = OPT_DNS;
    buf[off++] = OPT_LEASE;

    buf[off++] = OPT_END;

    // Выравниваем до 300 байт — некоторые сервера требуют.
    while (off < 300) buf[off++] = 0;
    return off;
}

// ----------------------------------------------------------------------------
// RX: разбираем входящие OFFER/ACK
// ----------------------------------------------------------------------------
static void dhcp_on_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                       const void* data, size_t len) {
    (void)src_ip; (void)src_port; (void)dst_port;
    if (len < 240) return;
    const uint8_t* p = (const uint8_t*)data;

    // Проверяем xid, magic и что это BOOTP reply.
    if (p[0] != BOOTP_REPLY) return;
    if (get_be32(p + 4) != DHCP_XID) return;
    if (get_be32(p + 236) != DHCP_MAGIC) return;

    uint32_t yiaddr = get_be32(p + 16);

    // Парсим options.
    uint8_t msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0, lease = 0, sid = 0;

    size_t off = 240;
    while (off < len) {
        uint8_t code = p[off++];
        if (code == OPT_END) break;
        if (code == 0) continue;      // Pad
        if (off >= len) break;
        uint8_t l = p[off++];
        if (off + l > len) break;
        switch (code) {
            case OPT_MSG_TYPE:
                if (l >= 1) msg_type = p[off];
                break;
            case OPT_SUBNET:
                if (l >= 4) subnet = get_be32(p + off);
                break;
            case OPT_ROUTER:
                if (l >= 4) router = get_be32(p + off);
                break;
            case OPT_DNS:
                if (l >= 4) dns = get_be32(p + off);
                break;
            case OPT_LEASE:
                if (l >= 4) lease = get_be32(p + off);
                break;
            case OPT_SERVER_ID:
                if (l >= 4) sid = get_be32(p + off);
                break;
            default:
                break;
        }
        off += l;
    }

    if (msg_type == DHCP_OFFER && g_dhcp.state == DHCP_ST_SELECTING) {
        dbg_puts("[dhcp] OFFER ");
        netif_dump_ip4(yiaddr);
        dbg_puts(" from ");
        netif_dump_ip4(sid);
        dbg_puts("\n");
        g_dhcp.offered_ip = yiaddr;
        g_dhcp.server_id  = sid;

        // Отправляем REQUEST (broadcast).
        uint8_t buf[300];
        size_t n = build_packet(buf, sizeof(buf), DHCP_REQUEST, yiaddr, sid);
        if (n > 0) {
            // До получения IP отправляем с src=0.0.0.0, dst=255.255.255.255.
            // Для этого временно обнулим iface->ip4_addr.
            net_interface_t* iface = netif_get_default();
            uint32_t save_ip = iface ? iface->ip4_addr : 0;
            if (iface) iface->ip4_addr = 0;
            // Заранее добавим broadcast в ARP-кэш как FF:FF:FF:FF:FF:FF.
            static const uint8_t bcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
            arp_cache_add(0xFFFFFFFFu, bcast_mac);
            udp_send(0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, buf, n);
            if (iface) iface->ip4_addr = save_ip;
            g_dhcp.state = DHCP_ST_REQUESTING;
        }
    } else if (msg_type == DHCP_ACK && g_dhcp.state == DHCP_ST_REQUESTING) {
        dbg_puts("[dhcp] ACK — bound\n");
        g_dhcp.subnet = subnet;
        g_dhcp.router = router;
        g_dhcp.dns    = dns;
        g_dhcp.lease  = lease;

        net_interface_t* iface = netif_get_default();
        if (iface) {
            netif_set_ip4(iface, yiaddr, subnet ? subnet : 0xFFFFFF00u, router);
        }
        g_dhcp.state = DHCP_ST_BOUND;
    } else if (msg_type == DHCP_NAK) {
        dbg_puts("[dhcp] NAK — start over\n");
        g_dhcp.state = DHCP_ST_INIT;
    }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
bool dhcp_is_bound(void) { return g_dhcp.state == DHCP_ST_BOUND; }

void dhcp_dump_state(void) {
    dbg_puts("[dhcp] state=");
    dbg_hex((uint32_t)g_dhcp.state);
    dbg_puts(" ip=");
    netif_dump_ip4(g_dhcp.offered_ip);
    dbg_puts(" gw=");
    netif_dump_ip4(g_dhcp.router);
    dbg_puts("\n");
}

bool dhcp_start(void) {
    // Регистрируем обработчик на клиентский порт 68.
    if (!udp_bind(DHCP_CLIENT_PORT, dhcp_on_udp)) {
        dbg_puts("[dhcp] udp_bind failed\n");
        // Возможно уже забинжен — считаем это не фатальным.
    }

    // Гарантируем, что у iface есть broadcast ARP.
    static const uint8_t bcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    arp_cache_add(0xFFFFFFFFu, bcast_mac);

    // Сохраним текущий IP и обнулим — DHCP шлёт с 0.0.0.0.
    net_interface_t* iface = netif_get_default();
    uint32_t save_ip = iface ? iface->ip4_addr : 0;
    if (iface) iface->ip4_addr = 0;

    uint8_t buf[300];
    size_t n = build_packet(buf, sizeof(buf), DHCP_DISCOVER, 0, 0);
    if (n == 0) {
        if (iface) iface->ip4_addr = save_ip;
        return false;
    }

    dbg_puts("[dhcp] DISCOVER\n");
    g_dhcp.state = DHCP_ST_SELECTING;
    bool ok = udp_send(0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, buf, n);

    if (iface) iface->ip4_addr = save_ip;
    return ok;
}
