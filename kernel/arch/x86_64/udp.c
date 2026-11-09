// ============================================================================
// udp.c — реализация UDP с pseudo-header checksum.
// ============================================================================
#include "udp.h"
#include "ipv4.h"
#include "netif.h"
#include "serial.h"

#define UDP_MAX_BINDS 8

typedef struct {
    bool             used;
    uint16_t         port;
    udp_rx_handler_t handler;
} udp_bind_t;

static udp_bind_t g_binds[UDP_MAX_BINDS];

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint16_t udp_calc_checksum(uint32_t src_ip, uint32_t dst_ip,
                                  const void* udp_start, size_t udp_len) {
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
    pseudo[9]  = 17;
    pseudo[10] = (uint8_t)(udp_len >> 8);
    pseudo[11] = (uint8_t)udp_len;

    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += ((uint32_t)pseudo[i] << 8) | pseudo[i+1];
    const uint8_t* p = (const uint8_t*)udp_start;
    size_t n = udp_len;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

bool udp_bind(uint16_t port, udp_rx_handler_t handler) {
    for (int i = 0; i < UDP_MAX_BINDS; i++)
        if (g_binds[i].used && g_binds[i].port == port) return false;
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!g_binds[i].used) {
            // [48.2] Слот становится «занятым» последним и под запретом
            // прерываний: udp_rx из обработчика не должен увидеть used=true
            // со старым портом или пустым обработчиком.
            uint64_t fl = net_irq_save();
            g_binds[i].port = port;
            g_binds[i].handler = handler;
            g_binds[i].used = true;
            net_irq_restore(fl);
            serial_puts("[udp] bind port=");
            serial_print_dec(port); serial_puts("\n");
            return true;
        }
    }
    return false;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_MAX_BINDS; i++)
        if (g_binds[i].used && g_binds[i].port == port) { g_binds[i].used = false; return; }
}

void udp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len) {
    (void)dst_ip;
    if (len < UDP_HDR_LEN) return;
    const uint8_t* p = (const uint8_t*)payload;
    uint16_t src_port = be16(p + 0);
    uint16_t dst_port = be16(p + 2);
    uint16_t length   = be16(p + 4);
    if (length > len) return;

    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (g_binds[i].used && g_binds[i].port == dst_port && g_binds[i].handler) {
            size_t data_len = length > UDP_HDR_LEN ? (size_t)(length - UDP_HDR_LEN) : 0;
            g_binds[i].handler(src_ip, src_port, dst_port, p + UDP_HDR_LEN, data_len);
            return;
        }
    }
}

// [48.2] Адрес отправителя — явный (см. ipv4_send_from). Буфер статический и
// под запретом прерываний: 1.4 КБ на стеке ядра (16 КБ) — лишний риск.
bool udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const void* payload, size_t plen) {
    if (plen > 1400) return false;

    static uint8_t buf[UDP_HDR_LEN + 1400];
    static uint8_t out[UDP_HDR_LEN + 1400];
    uint64_t fl = net_irq_save();
    put_be16(buf + 0, src_port);
    put_be16(buf + 2, dst_port);
    put_be16(buf + 4, (uint16_t)(UDP_HDR_LEN + plen));
    put_be16(buf + 6, 0);

    if (payload && plen > 0) {
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++) buf[UDP_HDR_LEN + i] = src[i];
    }

    uint16_t csum = udp_calc_checksum(src_ip, dst_ip, buf, UDP_HDR_LEN + plen);
    if (csum == 0) csum = 0xFFFF;
    put_be16(buf + 6, csum);
    // Копия наружу: ipv4_send при пустом кэше ARP ждёт ответа с разрешёнными
    // прерываниями, а общий buf в это время может понадобиться обработчику.
    size_t total = UDP_HDR_LEN + plen;
    for (size_t i = 0; i < total; i++) out[i] = buf[i];
    net_irq_restore(fl);

    return ipv4_send_from(src_ip, dst_ip, IPV4_PROTO_UDP, out, total);
}

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void* payload, size_t plen) {
    net_interface_t* iface = netif_get_default();
    return udp_send_from(iface ? iface->ip4_addr : 0, dst_ip, dst_port, src_port, payload, plen);
}
