// ============================================================================
// ipv4.c — реализация IPv4
// ============================================================================

#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "netif.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "../drivers/timer.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Checksum (RFC 1071)
// ----------------------------------------------------------------------------
uint16_t ipv4_checksum(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    // Складываем 16-битными словами (big-endian: старший байт первым).
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p   += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    // Сворачиваем переносы.
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------
static uint32_t ip_from_bytes(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)(ip);
}

// Нахождение MAC destination'a (прямая сеть или через gateway).
//
// Если в кеше нет — шлём ARP Request и БЛОКИРУЮЩЕ ждём ответа до
// IPV4_ARP_WAIT_MS миллисекунд, давая IRQ отработать через `sti; hlt`.
// Без этого ipv4_send возвращал бы false при первом обращении к новой цели,
// и приложение (ping/HTTP/DNS) видело бы «ответа нет», хотя на проводе
// ARP-reply вполне приходил — мы просто не успевали его обработать.
#define IPV4_ARP_WAIT_MS 800

static const uint8_t* resolve_next_hop(uint32_t dst_ip) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return NULL;

    uint32_t my_net  = iface->ip4_addr    & iface->ip4_netmask;
    uint32_t dst_net = dst_ip             & iface->ip4_netmask;

    uint32_t next_hop = (my_net == dst_net) ? dst_ip : iface->ip4_gateway;
    if (next_hop == 0) return NULL;

    const uint8_t* mac = arp_resolve(next_hop);
    if (mac) return mac;

    // Шлём первый запрос и ждём.
    arp_send_request(next_hop);

    uint32_t t0 = (uint32_t)timer_get_uptime_ms();
    uint32_t last_retry = t0;
    while ((uint32_t)timer_get_uptime_ms() - t0 < IPV4_ARP_WAIT_MS) {
        __asm__ volatile("sti; hlt");
        mac = arp_resolve(next_hop);
        if (mac) return mac;
        // Каждые 200 мс ретраим, на случай дропа фрейма.
        if ((uint32_t)timer_get_uptime_ms() - last_retry > 200) {
            arp_send_request(next_hop);
            last_retry = (uint32_t)timer_get_uptime_ms();
        }
    }
    dbg_puts("[ipv4] arp resolve timeout\n");
    return NULL;
}

// ----------------------------------------------------------------------------
// RX
// ----------------------------------------------------------------------------
void ipv4_rx(const void* payload, size_t len) {
    if (len < IPV4_HDR_MIN_LEN) return;
    const ipv4_header_t* h = (const ipv4_header_t*)payload;

    uint8_t ver = (h->ver_ihl >> 4) & 0xF;
    uint8_t ihl = h->ver_ihl & 0xF;   // в 32-битных словах
    if (ver != IPV4_VERSION) return;
    if (ihl < 5) return;

    size_t hdr_len = (size_t)ihl * 4;
    if (hdr_len > len) return;

    uint16_t total = ((uint16_t)((const uint8_t*)&h->total_length)[0] << 8) |
                     (uint16_t)((const uint8_t*)&h->total_length)[1];
    if (total > len) return;

    // Checksum проверяется по заголовку "как лежит" — эндианность не важна,
    // т.к. складываем сырые байты 16-битными словами.
    uint16_t got = ipv4_checksum(h, hdr_len);
    if (got != 0) {
        dbg_puts("[ipv4] bad checksum\n");
        return;
    }

    // Фрагментацию не обрабатываем.
    uint16_t ff = ((uint16_t)((const uint8_t*)&h->flags_frag_off)[0] << 8) |
                  (uint16_t)((const uint8_t*)&h->flags_frag_off)[1];
    if ((ff & 0x2000) || (ff & 0x1FFF)) {
        dbg_puts("[ipv4] fragmented — drop\n");
        return;
    }

    uint32_t dst = ip_from_bytes(h->dst);
    uint32_t src = ip_from_bytes(h->src);
    net_interface_t* iface = netif_get_default();
    if (!iface) return;

    // Принимаем только пакеты к нам или broadcast.
    if (dst != iface->ip4_addr && dst != 0xFFFFFFFFu) {
        return;
    }

    dbg_puts("[ipv4] rx ");
    netif_dump_ip4(src);
    dbg_puts(" -> ");
    netif_dump_ip4(dst);
    dbg_puts(" proto=0x");
    dbg_hex(h->protocol);
    dbg_puts("\n");

    const uint8_t* inner = (const uint8_t*)payload + hdr_len;
    size_t inner_len = total - hdr_len;

    switch (h->protocol) {
        case IPV4_PROTO_ICMP:
            icmp_rx(src, inner, inner_len);
            break;
        case IPV4_PROTO_UDP:
            udp_rx(src, dst, inner, inner_len);
            break;
        case IPV4_PROTO_TCP:
            tcp_rx(src, dst, inner, inner_len);
            break;
        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// TX
// ----------------------------------------------------------------------------
bool ipv4_send(uint32_t dst_ip, uint8_t protocol,
               const void* payload, size_t plen) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return false;
    if (plen > ETH_MTU - IPV4_HDR_MIN_LEN) return false;

    const uint8_t* next_mac = resolve_next_hop(dst_ip);
    if (!next_mac) {
        dbg_puts("[ipv4] drop: no ARP for next hop\n");
        return false;
    }

    // Буфер под полный фрейм.
    uint8_t frame[ETH_MAX_LEN + 4];
    uint8_t ipbuf[IPV4_HDR_MIN_LEN + ETH_MTU];

    ipv4_header_t* h = (ipv4_header_t*)ipbuf;
    h->ver_ihl   = (uint8_t)((IPV4_VERSION << 4) | 5);
    h->tos       = 0;
    uint16_t total = (uint16_t)(IPV4_HDR_MIN_LEN + plen);
    // Big-endian запись.
    ((uint8_t*)&h->total_length)[0] = (uint8_t)(total >> 8);
    ((uint8_t*)&h->total_length)[1] = (uint8_t)(total);
    static uint16_t id_counter = 1;
    id_counter++;
    ((uint8_t*)&h->id)[0] = (uint8_t)(id_counter >> 8);
    ((uint8_t*)&h->id)[1] = (uint8_t)(id_counter);
    ((uint8_t*)&h->flags_frag_off)[0] = 0x40;  // Don't Fragment
    ((uint8_t*)&h->flags_frag_off)[1] = 0x00;
    h->ttl       = 64;
    h->protocol  = protocol;
    h->checksum  = 0;
    ip_to_bytes(iface->ip4_addr, h->src);
    ip_to_bytes(dst_ip,           h->dst);

    // Сумма по заголовку (с нулевой checksum).
    uint16_t csum = ipv4_checksum(h, IPV4_HDR_MIN_LEN);
    ((uint8_t*)&h->checksum)[0] = (uint8_t)(csum >> 8);
    ((uint8_t*)&h->checksum)[1] = (uint8_t)(csum);

    // Payload.
    if (payload && plen > 0) {
        uint8_t* body = ipbuf + IPV4_HDR_MIN_LEN;
        const uint8_t* p = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++) body[i] = p[i];
    }

    size_t fn = ethernet_build(next_mac, ETH_TYPE_IPV4,
                               ipbuf, IPV4_HDR_MIN_LEN + plen,
                               frame, sizeof(frame));
    if (fn == 0) return false;
    return netif_send(frame, fn) > 0;
}
