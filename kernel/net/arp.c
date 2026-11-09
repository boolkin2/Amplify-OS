// ============================================================================
// arp.c — реализация ARP
// ============================================================================

#include "arp.h"
#include "ethernet.h"
#include "netif.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ARP_CACHE_SIZE 16

typedef struct {
    bool     used;
    uint32_t ip;         // host-byte-order
    uint8_t  mac[6];
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
static int          g_next_slot = 0;  // FIFO-замена

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

static void mac_copy(uint8_t dst[6], const uint8_t src[6]) {
    for (int i = 0; i < 6; i++) dst[i] = src[i];
}

// ----------------------------------------------------------------------------
// Cache
// ----------------------------------------------------------------------------
void arp_cache_add(uint32_t ip, const uint8_t mac[6]) {
    // Обновляем существующую, если есть.
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) {
            mac_copy(g_cache[i].mac, mac);
            return;
        }
    }
    // Иначе берём свободный, иначе — по FIFO.
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) {
            g_cache[i].used = true;
            g_cache[i].ip   = ip;
            mac_copy(g_cache[i].mac, mac);
            return;
        }
    }
    g_cache[g_next_slot].used = true;
    g_cache[g_next_slot].ip   = ip;
    mac_copy(g_cache[g_next_slot].mac, mac);
    g_next_slot = (g_next_slot + 1) % ARP_CACHE_SIZE;
}

const uint8_t* arp_resolve(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) {
            return g_cache[i].mac;
        }
    }
    return NULL;
}

void arp_cache_dump(void) {
    dbg_puts("[arp] cache:\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        dbg_puts("  ");
        netif_dump_ip4(g_cache[i].ip);
        dbg_puts(" -> ");
        netif_dump_mac(g_cache[i].mac);
        dbg_puts("\n");
    }
}

// ----------------------------------------------------------------------------
// Отправка ARP
// ----------------------------------------------------------------------------
static void arp_send(uint16_t oper, const uint8_t target_mac[6], uint32_t target_ip) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return;

    arp_packet_t pkt;
    pkt.htype = eth_hton16(ARP_HTYPE_ETHERNET);
    pkt.ptype = eth_hton16(ARP_PTYPE_IPV4);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.oper  = eth_hton16(oper);
    mac_copy(pkt.sha, iface->mac);
    ip_to_bytes(iface->ip4_addr, pkt.spa);
    mac_copy(pkt.tha, target_mac);
    ip_to_bytes(target_ip, pkt.tpa);

    uint8_t frame[64];
    size_t n = ethernet_build(target_mac, ETH_TYPE_ARP,
                              &pkt, sizeof(pkt), frame, sizeof(frame));
    if (n > 0) {
        netif_send(frame, n);
    }
}

bool arp_send_request(uint32_t ip) {
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    static const uint8_t zero[6]  = {0,0,0,0,0,0};
    (void)zero;
    dbg_puts("[arp] who-has ");
    netif_dump_ip4(ip);
    dbg_puts("?\n");
    arp_send(ARP_OP_REQUEST, bcast, ip);
    return true;
}

// ----------------------------------------------------------------------------
// Приём
// ----------------------------------------------------------------------------
void arp_rx(const void* payload, size_t len) {
    if (len < sizeof(arp_packet_t)) return;
    const arp_packet_t* p = (const arp_packet_t*)payload;

    // Проверим, что это IPv4-over-Ethernet.
    if (eth_ntoh16(p->htype) != ARP_HTYPE_ETHERNET) return;
    if (eth_ntoh16(p->ptype) != ARP_PTYPE_IPV4)    return;
    if (p->hlen != 6 || p->plen != 4)              return;

    uint16_t op = eth_ntoh16(p->oper);
    uint32_t spa = ip_from_bytes(p->spa);
    uint32_t tpa = ip_from_bytes(p->tpa);

    // Любой увиденный (spa, sha) кешируем — это полезная информация.
    arp_cache_add(spa, p->sha);

    net_interface_t* iface = netif_get_default();
    if (!iface || iface->ip4_addr == 0) return;

    if (op == ARP_OP_REQUEST && tpa == iface->ip4_addr) {
        // На нас запрос — отвечаем.
        dbg_puts("[arp] request for us from ");
        netif_dump_ip4(spa);
        dbg_puts(", replying\n");
        arp_send(ARP_OP_REPLY, p->sha, spa);
    } else if (op == ARP_OP_REPLY) {
        dbg_puts("[arp] reply: ");
        netif_dump_ip4(spa);
        dbg_puts(" is at ");
        netif_dump_mac(p->sha);
        dbg_puts("\n");
    }
}
