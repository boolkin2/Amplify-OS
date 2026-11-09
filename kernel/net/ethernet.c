// ============================================================================
// ethernet.c — реализация Ethernet II
// ============================================================================

#include "ethernet.h"
#include "netif.h"
#include "arp.h"
#include "ipv4.h"
#include "../debug_serial.h"

#include <stddef.h>
#include <stdint.h>

static const char hexd[] = "0123456789abcdef";

// Мини-итоговый лог фрейма (только в serial, чтобы не забить терминал).
static void log_frame(const eth_header_t* h, size_t len) {
    dbg_puts("[eth] rx ");
    netif_dump_mac(h->src);
    dbg_puts(" -> ");
    netif_dump_mac(h->dst);
    dbg_puts(" type=0x");
    uint16_t t = eth_ntoh16(h->ethertype);
    char b[5];
    for (int i = 3; i >= 0; i--) { b[i] = hexd[t & 0xF]; t >>= 4; }
    b[4] = 0;
    dbg_puts(b);
    dbg_puts(" len=");
    dbg_hex((uint32_t)len);
    dbg_puts("\n");
}

void ethernet_rx(const void* frame, size_t len) {
    if (!frame || len < ETH_HDR_LEN) return;
    const eth_header_t* h = (const eth_header_t*)frame;
    log_frame(h, len);

    uint16_t type = eth_ntoh16(h->ethertype);
    const uint8_t* payload = (const uint8_t*)frame + ETH_HDR_LEN;
    size_t plen = len - ETH_HDR_LEN;

    switch (type) {
        case ETH_TYPE_ARP:
            arp_rx(payload, plen);
            break;
        case ETH_TYPE_IPV4:
            ipv4_rx(payload, plen);
            break;
        // На 27 этапе добавим IPv6 / другие.
        default:
            break;
    }
}

size_t ethernet_build(const uint8_t dst[ETH_ALEN],
                      uint16_t ethertype,
                      const void* payload, size_t plen,
                      void* out, size_t out_cap) {
    if (!out || !dst) return 0;
    if (plen > ETH_MTU) return 0;

    size_t total = ETH_HDR_LEN + plen;
    if (total > out_cap) return 0;

    net_interface_t* iface = netif_get_default();
    if (!iface) return 0;

    eth_header_t* h = (eth_header_t*)out;
    for (int i = 0; i < ETH_ALEN; i++) h->dst[i] = dst[i];
    for (int i = 0; i < ETH_ALEN; i++) h->src[i] = iface->mac[i];
    h->ethertype = eth_hton16(ethertype);

    if (payload && plen > 0) {
        uint8_t* body = (uint8_t*)out + ETH_HDR_LEN;
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++) body[i] = src[i];
    }

    // Дотягиваем до 60 байт (минимум Ethernet до FCS). NIC добавит 4 байта FCS.
    if (total < ETH_MIN_LEN) {
        uint8_t* pad = (uint8_t*)out + total;
        for (size_t i = 0; i < (size_t)(ETH_MIN_LEN - total); i++) pad[i] = 0;
        total = ETH_MIN_LEN;
    }
    return total;
}
