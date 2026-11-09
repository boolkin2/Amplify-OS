// ============================================================================
// arp.c — ARP реализация.
// ----------------------------------------------------------------------------
// [48.2] Кэш делят главный поток (arp_lookup перед отправкой, arp_cache_add из
// DHCP) и обработчик прерывания сетевой карты (arp_rx). Раньше arp_resolve
// возвращал указатель внутрь кэша: пока вызывающий копировал MAC в кадр,
// обработчик мог перезаписать слот по кругу (FIFO) — кадр уходил бы на чужой
// адрес. Теперь MAC копируется наружу при запрещённых прерываниях.
// ============================================================================
#include "arp.h"
#include "ethernet.h"
#include "netif.h"
#include "serial.h"

#define ARP_CACHE_SIZE 16

typedef struct {
    bool     used;
    uint32_t ip;          // host-byte-order
    uint8_t  mac[6];
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
static int          g_next_slot = 0;

static uint32_t ip_from_bytes(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] <<  8) | (uint32_t)b[3];
}

static void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >>  8);
    out[3] = (uint8_t)(ip);
}

static void mac_copy(uint8_t dst[6], const uint8_t src[6]) {
    for (int i = 0; i < 6; i++) dst[i] = src[i];
}

void arp_cache_add(uint32_t ip, const uint8_t mac[6]) {
    uint64_t fl = net_irq_save();
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE && slot < 0; i++)
        if (g_cache[i].used && g_cache[i].ip == ip) slot = i;
    for (int i = 0; i < ARP_CACHE_SIZE && slot < 0; i++)
        if (!g_cache[i].used) slot = i;
    if (slot < 0) {                       // FIFO
        slot = g_next_slot;
        g_next_slot = (g_next_slot + 1) % ARP_CACHE_SIZE;
    }
    g_cache[slot].used = true;
    g_cache[slot].ip   = ip;
    mac_copy(g_cache[slot].mac, mac);
    net_irq_restore(fl);
}

bool arp_lookup(uint32_t ip, uint8_t out[6]) {
    bool found = false;
    uint64_t fl = net_irq_save();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) {
            mac_copy(out, g_cache[i].mac);
            found = true;
            break;
        }
    }
    net_irq_restore(fl);
    return found;
}

// Старый интерфейс (указатель в кэш) оставлен для sock.c/отладки; новый код
// пользуется arp_lookup.
const uint8_t* arp_resolve(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].used && g_cache[i].ip == ip) return g_cache[i].mac;
    }
    return 0;
}

// Сеть перенастроили (другой адрес/шлюз): старые соответствия недействительны.
void arp_cache_flush(void) {
    uint64_t fl = net_irq_save();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) g_cache[i].used = false;
    g_next_slot = 0;
    net_irq_restore(fl);
}

void arp_cache_dump(void) {
    serial_puts("[arp] cache:\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].used) continue;
        serial_puts("  "); netif_dump_ip4(g_cache[i].ip);
        serial_puts(" -> "); netif_dump_mac(g_cache[i].mac); serial_puts("\n");
    }
}

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
    size_t n = ethernet_build(target_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt), frame, sizeof(frame));
    if (n > 0) netif_send(frame, n);
}

bool arp_send_request(uint32_t ip) {
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    arp_send(ARP_OP_REQUEST, bcast, ip);
    return true;
}

// Gratuitous ARP: объявить соседям свой адрес (после смены настроек). Заодно
// коммутатор и шлюз обновляют у себя соответствие адрес — MAC.
void arp_announce(void) {
    net_interface_t* iface = netif_get_default();
    if (!iface || iface->ip4_addr == 0) return;
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    arp_send(ARP_OP_REQUEST, bcast, iface->ip4_addr);
}

void arp_rx(const void* payload, size_t len) {
    if (len < sizeof(arp_packet_t)) return;
    const arp_packet_t* p = (const arp_packet_t*)payload;

    if (eth_ntoh16(p->htype) != ARP_HTYPE_ETHERNET) return;
    if (eth_ntoh16(p->ptype) != ARP_PTYPE_IPV4)    return;
    if (p->hlen != 6 || p->plen != 4)              return;

    uint16_t op = eth_ntoh16(p->oper);
    uint32_t spa = ip_from_bytes(p->spa);
    uint32_t tpa = ip_from_bytes(p->tpa);

    net_interface_t* iface = netif_get_default();
    if (!iface) return;

    // Запоминаем только то, что нас касается: ответы и запросы к нам. Раньше в
    // кэш шёл отправитель ЛЮБОГО ARP-кадра в сегменте — в живой сети 16 слотов
    // быстро забивались чужими адресами и вытесняли шлюз.
    bool to_us = iface->ip4_addr != 0 && tpa == iface->ip4_addr;
    if (spa != 0 && (op == ARP_OP_REPLY || to_us)) arp_cache_add(spa, p->sha);

    if (op == ARP_OP_REQUEST && to_us) arp_send(ARP_OP_REPLY, p->sha, spa);
}
