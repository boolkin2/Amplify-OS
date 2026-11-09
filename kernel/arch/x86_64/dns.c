// ============================================================================
// dns.c — DNS A-resolver через UDP/53.
// ----------------------------------------------------------------------------
// Этап 48.1 (полный выход в интернет):
//   * список DNS-серверов с fallback'ом: DHCP primary → DHCP secondary →
//     шлюз (NAT DNS-proxy на VMware/VirtualBox/QEMU) → 8.8.8.8 → 1.1.1.1;
//   * повторные попытки (2 на сервер) с коротким таймаутом вместо одного
//     длинного — «мертвый» сервер больше не блокирует загрузку страницы;
//   * кеш ответов (32 записи, TTL из ответа, минимум 30 с) — редиректы и
//     повторные запросы к тому же хосту идут без сетевого round-trip'а;
//   * случайный source port + случайный query ID (анти-спуфинг, RFC 5452);
//   * корректный проход по CNAME-цепочке (берём первую A-запись в ответе).
// Подэтап 48.2:
//   * общий предел времени на одно имя (DNS_TOTAL_MS): раньше шесть серверов
//     по две попытки давали до 18 секунд «зависшей» загрузки страницы;
//   * сервер, ответивший последним, в следующий раз спрашивается первым —
//     мёртвый основной DNS стоит одного таймаута, а не таймаута на каждое имя;
//   * при неудаче различаем «имени нет» (ответил хотя бы один сервер) и
//     «серверы молчат» — dns_last_failure(), это видно на странице ошибки;
//   * ожидание через net_idle (экран не замирает), отмена по net_cancelled;
//   * ответ принимается только от того сервера, которому ушёл запрос.
// ============================================================================
#include "dns.h"
#include "udp.h"
#include "netif.h"
#include "timer.h"
#include "serial.h"

#define DNS_PORT       53

#define DNS_TYPE_A     1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN   1

#define DNS_TRY_TIMEOUT_MS  1200   // таймаут одной попытки
#define DNS_TRIES_PER_SRV   2      // попыток на каждый сервер
#define DNS_TOTAL_MS        7000   // предел на одно имя
#define DNS_MAX_SERVERS     7
#define DNS_CACHE_SIZE      32
#define DNS_CACHE_MIN_TTL   30     // секунд
#define DNS_CACHE_MAX_TTL   3600   // секунд
#define DNS_NAME_MAX        128

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static volatile uint32_t g_resolved_ip  = 0;
static volatile uint32_t g_resolved_ttl = 0;
static volatile uint16_t g_pending_id   = 0;
static volatile bool     g_got_reply    = false;
static volatile bool     g_neg_reply    = false;   // ответ есть, но адреса нет

static uint32_t g_dns_server  = 0;   // primary   (DHCP опция 6 #1 или network.conf)
static uint32_t g_dns_server2 = 0;   // secondary (DHCP опция 6 #2 или network.conf)
static uint32_t g_last_good   = 0;   // сервер, давший последний ответ
static volatile uint32_t g_query_srv = 0;   // кому ушёл текущий запрос
static uint16_t g_local_port  = 0;
static dns_failure_t g_failure = DNS_FAIL_NONE;

typedef struct {
    char     name[DNS_NAME_MAX];
    uint32_t ip;
    uint64_t expire_ms;
} dns_cache_entry_t;

static dns_cache_entry_t g_cache[DNS_CACHE_SIZE];
static uint32_t g_cache_next = 0;

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static inline uint64_t dns_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t dns_now_ms(void) { return timer_get_uptime_ms(); }

// Смена серверов (другая сеть) — прежний «удачный» сервер забываем.
void dns_set_server(uint32_t srv_ip)  { if (g_dns_server != srv_ip) g_last_good = 0; g_dns_server  = srv_ip; }
void dns_set_server2(uint32_t srv_ip) { g_dns_server2 = srv_ip; }
uint32_t dns_get_server(void)  { return g_dns_server; }
uint32_t dns_get_server2(void) { return g_dns_server2; }
dns_failure_t dns_last_failure(void) { return g_failure; }

// ---------------------------------------------------------------------------
// Строковые утилиты (без libc).
// ---------------------------------------------------------------------------
static bool name_eq_ci(const char* a, const char* b) {
    size_t i = 0;
    for (;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}

static void name_copy(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

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
        } else { return 0; }
        s++;
    }
    if (!digits || idx != 3) return 0;
    parts[3] = (uint32_t)val;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

// ---------------------------------------------------------------------------
// Кеш.
// ---------------------------------------------------------------------------
static uint32_t cache_lookup(const char* name) {
    uint64_t now = dns_now_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!g_cache[i].ip) continue;
        if (g_cache[i].expire_ms <= now) { g_cache[i].ip = 0; continue; }
        if (name_eq_ci(g_cache[i].name, name)) return g_cache[i].ip;
    }
    return 0;
}

static void cache_store(const char* name, uint32_t ip, uint32_t ttl_sec) {
    if (!ip) return;
    if (ttl_sec < DNS_CACHE_MIN_TTL) ttl_sec = DNS_CACHE_MIN_TTL;
    if (ttl_sec > DNS_CACHE_MAX_TTL) ttl_sec = DNS_CACHE_MAX_TTL;

    int slot = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_cache[i].ip && name_eq_ci(g_cache[i].name, name)) { slot = i; break; }
        if (!g_cache[i].ip && slot < 0) slot = i;
    }
    if (slot < 0) slot = (int)(g_cache_next++ % DNS_CACHE_SIZE);

    name_copy(g_cache[slot].name, name, DNS_NAME_MAX);
    g_cache[slot].ip        = ip;
    g_cache[slot].expire_ms = dns_now_ms() + (uint64_t)ttl_sec * 1000ull;
}

void dns_cache_flush(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) g_cache[i].ip = 0;
}

// ---------------------------------------------------------------------------
// Разбор ответа.
// ---------------------------------------------------------------------------
static void dns_on_packet(uint32_t src_ip, uint16_t src_port,
                          uint16_t dst_port,
                          const void* data, size_t len) {
    (void)dst_port;
    if (src_port != DNS_PORT) return;
    // Ответ — только от спрошенного сервера (и только пока мы ждём).
    if (g_got_reply || src_ip != g_query_srv) return;
    if (len < sizeof(dns_header_t)) return;
    const uint8_t* p = (const uint8_t*)data;
    const dns_header_t* h = (const dns_header_t*)p;
    if (be16(h->id) != g_pending_id) return;

    uint16_t flags = be16(h->flags);
    if ((flags & 0x8000) == 0) return;            // не response
    if ((flags & 0x000F) != 0) {
        serial_puts("[dns] reply rcode!=0\n");
        g_neg_reply = true; g_got_reply = true; return;
    }
    uint16_t qdcount = be16(h->qdcount);
    uint16_t ancount = be16(h->ancount);
    if (ancount == 0) {
        serial_puts("[dns] no answer\n");
        g_neg_reply = true; g_got_reply = true; return;
    }

    // Пропустить question-секцию (может быть несколько записей).
    size_t off = sizeof(dns_header_t);
    for (uint16_t q = 0; q < qdcount && off < len; q++) {
        while (off < len) {
            uint8_t l = p[off];
            if (l == 0) { off++; break; }
            if ((l & 0xC0) == 0xC0) { off += 2; break; }
            off += (size_t)l + 1;
        }
        off += 4;                                  // QTYPE + QCLASS
    }

    // Пройти по answer'ам: CNAME пропускаем, берём первую A-запись
    // (RFC 1034 — сервер обязан положить в тот же ответ A для конечного имени).
    for (uint16_t i = 0; i < ancount && off + 10 <= len; i++) {
        if ((p[off] & 0xC0) == 0xC0) off += 2;
        else {
            while (off < len) {
                uint8_t l = p[off];
                if (l == 0) { off++; break; }
                if ((l & 0xC0) == 0xC0) { off += 2; break; }
                off += (size_t)l + 1;
            }
        }
        if (off + 10 > len) break;
        uint16_t type  = (uint16_t)(p[off] << 8) | p[off+1];
        uint16_t cls   = (uint16_t)(p[off+2] << 8) | p[off+3];
        uint32_t ttl   = ((uint32_t)p[off+4] << 24) | ((uint32_t)p[off+5] << 16) |
                         ((uint32_t)p[off+6] << 8)  |  (uint32_t)p[off+7];
        uint16_t rdlen = (uint16_t)(p[off+8] << 8) | p[off+9];
        off += 10;
        if (off + rdlen > len) break;
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            uint32_t ip = ((uint32_t)p[off] << 24) | ((uint32_t)p[off+1] << 16) |
                          ((uint32_t)p[off+2] << 8) | p[off+3];
            g_resolved_ip  = ip;
            g_resolved_ttl = ttl;
            g_got_reply    = true;
            serial_puts("[dns] resolved -> "); netif_dump_ip4(ip); serial_puts("\n");
            return;
        }
        off += rdlen;
    }
    // Только CNAME без A — считаем отрицательным ответом, попробуем др. сервер.
    g_neg_reply = true;
    g_got_reply = true;
}

void dns_init(void) {
    // Случайный source port (RFC 5452) в диапазоне 20000..60000.
    uint64_t t = dns_rdtsc();
    g_local_port = (uint16_t)(20000 + ((uint32_t)(t ^ (t >> 29)) % 40000));
    if (!udp_bind(g_local_port, dns_on_packet)) {
        // Порт занят — берём фиксированный fallback.
        g_local_port = 0xC0DE;
        udp_bind(g_local_port, dns_on_packet);
    }
    dns_cache_flush();
}

static size_t encode_qname(const char* name, uint8_t* out, size_t cap) {
    size_t pos = 0, i = 0;
    while (name[i]) {
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

// Собрать список серверов-кандидатов (с дедупликацией).
static int build_server_list(uint32_t* out) {
    net_interface_t* iface = netif_get_default();
    uint32_t cand[DNS_MAX_SERVERS];
    int n = 0;
    cand[n++] = g_last_good;
    cand[n++] = g_dns_server;
    cand[n++] = g_dns_server2;
    cand[n++] = iface ? iface->ip4_gateway : 0;
    cand[n++] = ipv4_addr(8, 8, 8, 8);
    cand[n++] = ipv4_addr(1, 1, 1, 1);
    cand[n++] = ipv4_addr(8, 8, 4, 4);

    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (!cand[i]) continue;
        bool dup = false;
        for (int k = 0; k < cnt; k++) if (out[k] == cand[i]) { dup = true; break; }
        if (!dup) out[cnt++] = cand[i];
    }
    return cnt;
}

static uint32_t dns_query_one(const char* name, uint32_t srv, uint32_t* ttl_out) {
    static uint8_t buf[512];

    uint64_t t = dns_rdtsc();
    uint16_t qid = (uint16_t)((t ^ (t >> 17) ^ dns_now_ms()) & 0xFFFF);
    if (qid == 0) qid = 1;

    dns_header_t h;
    h.id = be16(qid);
    h.flags = be16(0x0100);     // standard query, recursion desired
    h.qdcount = be16(1);
    h.ancount = 0; h.nscount = 0; h.arcount = 0;
    size_t hlen = sizeof(dns_header_t);
    for (size_t i = 0; i < hlen; i++) buf[i] = ((uint8_t*)&h)[i];

    size_t qlen = encode_qname(name, buf + hlen, sizeof(buf) - hlen - 4);
    if (qlen == 0) return 0;
    size_t off = hlen + qlen;
    buf[off++] = 0; buf[off++] = DNS_TYPE_A;
    buf[off++] = 0; buf[off++] = DNS_CLASS_IN;

    g_resolved_ip  = 0;
    g_resolved_ttl = 0;
    g_got_reply    = false;
    g_neg_reply    = false;
    g_pending_id   = qid;
    g_query_srv    = srv;

    if (!udp_send(srv, DNS_PORT, g_local_port, buf, off)) {
        // Нет ARP-ответа от сервера/шлюза — ждать ответа DNS бессмысленно.
        g_query_srv = 0;
        return 0;
    }

    uint64_t start_ms = dns_now_ms();
    while (!g_got_reply) {
        if (dns_now_ms() - start_ms > DNS_TRY_TIMEOUT_MS || net_cancelled()) {
            g_query_srv = 0;
            return 0;
        }
        net_idle();
    }
    g_query_srv = 0;
    if (ttl_out) *ttl_out = g_resolved_ttl;
    return g_resolved_ip;
}

uint32_t dns_resolve(const char* name) {
    if (!name || !name[0]) return 0;

    // IP-литерал — без сети.
    uint32_t lit = parse_ip4_literal(name);
    if (lit) return lit;

    // Кеш.
    uint32_t cached = cache_lookup(name);
    if (cached) return cached;

    g_failure = DNS_FAIL_NONE;
    net_interface_t* iface = netif_get_default();
    if (!iface || !iface->up) { serial_puts("[dns] no iface\n"); g_failure = DNS_FAIL_NO_NET; return 0; }
    if (iface->ip4_addr == 0) { serial_puts("[dns] no address\n"); g_failure = DNS_FAIL_NO_NET; return 0; }
    if (!g_local_port) dns_init();

    uint32_t servers[DNS_MAX_SERVERS];
    int nsrv = build_server_list(servers);

    uint64_t t0 = dns_now_ms();
    int negatives = 0;
    for (int s = 0; s < nsrv; s++) {
        for (int attempt = 0; attempt < DNS_TRIES_PER_SRV; attempt++) {
            if (net_cancelled() || dns_now_ms() - t0 > DNS_TOTAL_MS) goto out;
            uint32_t ttl = 0;
            uint32_t ip  = dns_query_one(name, servers[s], &ttl);
            if (ip) {
                g_last_good = servers[s];
                cache_store(name, ip, ttl);
                return ip;
            }
            // Отрицательный ответ (NXDOMAIN / нет A) — этот сервер отвечает,
            // повторять к нему бессмысленно, идём к следующему.
            if (g_neg_reply) { negatives++; g_last_good = servers[s]; break; }
        }
        // Два сервера подряд сказали «такого имени нет» — верим.
        if (negatives >= 2) break;
        serial_puts("[dns] server "); netif_dump_ip4(servers[s]);
        serial_puts(" no usable answer, trying next\n");
    }
out:
    g_failure = negatives ? DNS_FAIL_NXDOMAIN : DNS_FAIL_NO_REPLY;
    serial_puts(negatives ? "[dns] name not found: " : "[dns] servers do not answer for ");
    serial_puts(name); serial_puts("\n");
    return 0;
}
