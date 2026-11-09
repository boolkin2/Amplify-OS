// ============================================================================
// dns.c — простой DNS-клиент через UDP/53.
// ============================================================================

#include "dns.h"
#include "udp.h"
#include "netif.h"
#include "../debug_serial.h"
#include "../drivers/timer.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DNS_PORT      53
#define DNS_LOCAL_PORT 0xC0DE  // фикс. локальный порт

#define DNS_TYPE_A     1
#define DNS_CLASS_IN   1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static volatile uint32_t g_resolved_ip = 0;
static volatile uint16_t g_pending_id  = 0;
static volatile bool     g_got_reply   = false;
static uint32_t g_dns_server = 0;   // 0 = use gateway

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v & 0xFF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static size_t s_strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

void dns_set_server(uint32_t srv_ip) { g_dns_server = srv_ip; }

// Парсит "10.0.2.3" в host-byte-order. Возвращает 0 при ошибке.
static uint32_t parse_ip4_literal(const char* s) {
    uint32_t parts[4] = {0};
    int idx = 0, val = 0, digits = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            digits++;
            if (val > 255) return 0;
        } else if (*s == '.') {
            if (!digits || idx >= 3) return 0;
            parts[idx++] = (uint32_t)val;
            val = 0; digits = 0;
        } else {
            return 0;
        }
        s++;
    }
    if (!digits || idx != 3) return 0;
    parts[3] = (uint32_t)val;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

// RX-обработчик. Парсит ответ на наш запрос.
static void dns_on_packet(uint32_t src_ip, uint16_t src_port,
                          uint16_t dst_port,
                          const void* data, size_t len) {
    (void)src_ip; (void)src_port; (void)dst_port;
    if (len < sizeof(dns_header_t)) return;
    const uint8_t* p = (const uint8_t*)data;
    const dns_header_t* h = (const dns_header_t*)p;
    if (be16(h->id) != g_pending_id) return;
    uint16_t flags = be16(h->flags);
    if ((flags & 0x000F) != 0) {
        dbg_puts("[dns] reply rcode!=0\n");
        g_got_reply = true; return;
    }
    uint16_t ancount = be16(h->ancount);
    if (ancount == 0) {
        dbg_puts("[dns] no answer records\n");
        g_got_reply = true; return;
    }

    // Пропустить question section: name(label-string \0) + qtype(2) + qclass(2)
    size_t off = sizeof(dns_header_t);
    while (off < len && p[off] != 0) {
        if ((p[off] & 0xC0) == 0xC0) { off += 2; goto skip_qtypeclass; }
        off += p[off] + 1;
    }
    off++; // 0-byte
skip_qtypeclass:
    off += 4;

    // Прочитать первый answer
    for (uint16_t i = 0; i < ancount && off + 12 <= len; i++) {
        // Skip name (compression pointer)
        if ((p[off] & 0xC0) == 0xC0) off += 2;
        else {
            while (off < len && p[off] != 0) off += p[off] + 1;
            off++;
        }
        if (off + 10 > len) return;
        uint16_t type = (p[off] << 8) | p[off+1];
        uint16_t cls  = (p[off+2] << 8) | p[off+3];
        // ttl 4 bytes (skip)
        uint16_t rdlen = (p[off+8] << 8) | p[off+9];
        off += 10;
        if (off + rdlen > len) return;
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            uint32_t ip = ((uint32_t)p[off] << 24) | ((uint32_t)p[off+1] << 16) |
                          ((uint32_t)p[off+2] << 8) | p[off+3];
            g_resolved_ip = ip;
            g_got_reply = true;
            dbg_puts("[dns] resolved -> ");
            netif_dump_ip4(ip);
            dbg_puts("\n");
            return;
        }
        off += rdlen;
    }
    g_got_reply = true;
}

void dns_init(void) {
    udp_bind(DNS_LOCAL_PORT, dns_on_packet);
}

// Кодирует "example.com" в DNS-формат label-list: 7example3com0.
static size_t encode_qname(const char* name, uint8_t* out, size_t cap) {
    size_t pos = 0;
    size_t i = 0;
    while (name[i]) {
        // Найти следующую точку
        size_t lstart = i;
        while (name[i] && name[i] != '.') i++;
        size_t llen = i - lstart;
        if (llen == 0 || llen > 63) return 0;
        if (pos + 1 + llen >= cap) return 0;
        out[pos++] = (uint8_t)llen;
        for (size_t k = 0; k < llen; k++) out[pos++] = (uint8_t)name[lstart + k];
        if (name[i] == '.') i++;
    }
    if (pos + 1 > cap) return 0;
    out[pos++] = 0;
    return pos;
}

uint32_t dns_resolve(const char* name) {
    if (!name) return 0;
    // Литерал?
    uint32_t lit = parse_ip4_literal(name);
    if (lit) return lit;

    net_interface_t* iface = netif_get_default();
    if (!iface) {
        dbg_puts("[dns] no interface\n");
        return 0;
    }
    uint32_t srv = g_dns_server;
    if (!srv) srv = iface->ip4_gateway;
    if (!srv) srv = ipv4_addr(8, 8, 8, 8);

    // Сформировать запрос
    static uint8_t buf[256];
    uint16_t qid = (uint16_t)(timer_get_uptime_ms() & 0xFFFF) ^ 0xACE1;
    if (qid == 0) qid = 1;
    dns_header_t h;
    h.id = be16(qid);
    h.flags = be16(0x0100);   // standard query, recursion desired
    h.qdcount = be16(1);
    h.ancount = 0;
    h.nscount = 0;
    h.arcount = 0;
    size_t hlen = sizeof(dns_header_t);
    for (size_t i = 0; i < hlen; i++) buf[i] = ((uint8_t*)&h)[i];

    size_t qlen = encode_qname(name, buf + hlen, sizeof(buf) - hlen - 4);
    if (qlen == 0) return 0;
    size_t off = hlen + qlen;
    buf[off++] = 0; buf[off++] = DNS_TYPE_A;
    buf[off++] = 0; buf[off++] = DNS_CLASS_IN;

    g_resolved_ip = 0;
    g_got_reply   = false;
    g_pending_id  = qid;

    if (!udp_send(srv, DNS_PORT, DNS_LOCAL_PORT, buf, off)) {
        dbg_puts("[dns] udp_send failed\n");
        return 0;
    }

    // Ждём ответ
    uint32_t start = timer_get_uptime_ms();
    while (!g_got_reply) {
        if (timer_get_uptime_ms() - start > DNS_TIMEOUT_MS) {
            dbg_puts("[dns] timeout\n");
            return 0;
        }
        __asm__ volatile("sti; hlt");
    }
    return g_resolved_ip;
}
