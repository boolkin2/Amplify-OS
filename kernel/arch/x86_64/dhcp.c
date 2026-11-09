// ============================================================================
// dhcp.c — DHCP-клиент (RFC 2131).
// ----------------------------------------------------------------------------
// Подэтап 48.2. До него клиент умел ровно одно: при загрузке 5 секунд слать
// DISCOVER и, если повезёт, принять адрес. Дальше — тишина: ни повторов после
// загрузки, ни продления аренды (адрес оставался у нас и после её истечения).
// Теперь это конечный автомат, который крутит dhcp_tick() из главного потока
// (цикл WM и ожидание при загрузке):
//
//   SELECTING   DISCOVER с нарастающей паузой 1.5 → 3 → 6 … 30 с, без конца:
//               сервер, появившийся через час, выдаст адрес без перезагрузки;
//   REQUESTING  REQUEST на предложенный адрес, до 3 повторов по 2 с;
//   BOUND       адрес наш; в T1 (половина аренды) — RENEWING;
//   RENEWING    REQUEST напрямую серверу; в T2 (7/8 аренды) — REBINDING;
//   REBINDING   REQUEST широковещательно; аренда истекла — адрес теряем
//               (событие DHCP_EV_LOST) и начинаем сначала.
//
// Обработчик прерывания (dhcp_on_udp) только запоминает ответ сервера; адрес
// интерфейсу назначает dhcp_tick в главном потоке. Раньше netif_set_ip4
// вызывался прямо из прерывания — посреди отправки сегмента главным потоком.
//
// Два исправления для настоящих (не slirp) сетей:
//   * флаг BROADCAST в запросах: без него dnsmasq/ISC отвечают не
//     широковещательно, а на выдаваемый адрес, которого у нас ещё нет;
//   * пакеты уходят от 0.0.0.0 через udp_send_from, адрес интерфейса при этом
//     не трогается (раньше он на время отправки обнулялся).
// ============================================================================
#include "dhcp.h"
#include "udp.h"
#include "ipv4.h"
#include "netif.h"
#include "timer.h"
#include "serial.h"

#define BOOTP_REQUEST   1
#define BOOTP_REPLY     2
#define DHCP_HTYPE_ETH  1
#define DHCP_HLEN_ETH   6
#define DHCP_MAGIC      0x63825363u

#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_ACK        5
#define DHCP_NAK        6

#define OPT_SUBNET      1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_HOSTNAME    12
#define OPT_REQUESTED   50
#define OPT_LEASE       51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

#define DHCP_XID_FALLBACK 0x11223344u

#define DISCOVER_FIRST_MS   1500
#define DISCOVER_MAX_MS     30000
#define REQUEST_RETRY_MS    2000
#define REQUEST_RETRIES     3
#define RENEW_RETRY_MIN_MS  5000
#define RENEW_RETRY_MAX_MS  60000

typedef enum {
    DHCP_ST_OFF = 0,           // клиент выключен (статическая настройка)
    DHCP_ST_SELECTING,         // ждём OFFER
    DHCP_ST_OFFER_RECEIVED,    // OFFER пришёл; REQUEST пошлёт главный поток
    DHCP_ST_REQUESTING,        // ждём ACK на первый REQUEST
    DHCP_ST_BOUND,
    DHCP_ST_RENEWING,
    DHCP_ST_REBINDING,
} dhcp_state_t;

typedef struct {
    uint32_t ip, subnet, router, dns, dns2, lease, server;
} dhcp_lease_t;

static struct {
    volatile dhcp_state_t state;
    uint32_t     xid;
    uint32_t     offered_ip;
    uint32_t     server_id;

    // Заполняет обработчик прерывания, забирает dhcp_tick.
    volatile bool ack_ready;
    volatile bool nak_ready;
    dhcp_lease_t  ack;

    dhcp_lease_t cur;            // действующая аренда
    uint64_t     bound_ms;       // когда получена/продлена
    uint64_t     t1_ms, t2_ms, expire_ms;    // абсолютные моменты; 0 — бессрочно

    uint64_t     last_tx_ms;
    uint32_t     backoff_ms;
    int          tries;
    uint32_t     lease_cap;      // perf.conf dhcplease=N: потолок аренды, с
    int          events;         // DHCP_EV_* для netcfg
    uint32_t     renewals;
} g_dhcp;

static bool g_udp_bound = false;

static inline uint64_t dhcp_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t dhcp_gen_xid(void) {
    uint64_t t = dhcp_rdtsc();
    uint32_t x = (uint32_t)(t ^ (t >> 32)) ^ 0x9E3779B9u;
    net_interface_t* iface = netif_get_default();
    if (iface) {
        x ^= ((uint32_t)iface->mac[2] << 24) | ((uint32_t)iface->mac[3] << 16) |
             ((uint32_t)iface->mac[4] <<  8) |  (uint32_t)iface->mac[5];
    }
    if (x == 0) x = DHCP_XID_FALLBACK;
    return x;
}

static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

// ciaddr != 0 — продление (адрес уже наш, сервер ответит на него напрямую);
// иначе просим отвечать широковещательно и называем адрес/сервер опциями.
static size_t build_packet(uint8_t* buf, size_t cap, uint8_t msg_type,
                           uint32_t ciaddr, uint32_t requested_ip, uint32_t server_ip) {
    if (cap < 300) return 0;
    net_interface_t* iface = netif_get_default();
    if (!iface) return 0;

    for (size_t i = 0; i < 240; i++) buf[i] = 0;

    buf[0] = BOOTP_REQUEST;
    buf[1] = DHCP_HTYPE_ETH;
    buf[2] = DHCP_HLEN_ETH;
    buf[3] = 0;
    put_be32(buf + 4, g_dhcp.xid);
    if (ciaddr) put_be32(buf + 12, ciaddr);
    else        buf[10] = 0x80;                      // flags: BROADCAST

    for (int i = 0; i < 6; i++) buf[28 + i] = iface->mac[i];
    put_be32(buf + 236, DHCP_MAGIC);

    size_t off = 240;
    buf[off++] = OPT_MSG_TYPE; buf[off++] = 1; buf[off++] = msg_type;

    if (msg_type == DHCP_REQUEST && !ciaddr) {
        buf[off++] = OPT_REQUESTED; buf[off++] = 4;
        put_be32(buf + off, requested_ip); off += 4;
        buf[off++] = OPT_SERVER_ID; buf[off++] = 4;
        put_be32(buf + off, server_ip); off += 4;
    }

    static const char host[] = "amplify-os";
    buf[off++] = OPT_HOSTNAME; buf[off++] = (uint8_t)(sizeof(host) - 1);
    for (size_t i = 0; i < sizeof(host) - 1; i++) buf[off++] = (uint8_t)host[i];

    buf[off++] = OPT_PARAM_LIST; buf[off++] = 4;
    buf[off++] = OPT_SUBNET;
    buf[off++] = OPT_ROUTER;
    buf[off++] = OPT_DNS;
    buf[off++] = OPT_LEASE;
    buf[off++] = OPT_END;
    while (off < 300) buf[off++] = 0;
    return off;
}

// Обработчик прерывания: разобрать ответ и запомнить. Ничего не отправляет и
// не трогает интерфейс.
static void dhcp_on_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                        const void* data, size_t len) {
    (void)src_port; (void)dst_port;
    if (len < 240) return;
    const uint8_t* p = (const uint8_t*)data;

    if (p[0] != BOOTP_REPLY) return;
    if (get_be32(p + 4) != g_dhcp.xid) return;
    if (get_be32(p + 236) != DHCP_MAGIC) return;
    net_interface_t* iface = netif_get_default();
    if (!iface) return;
    for (int i = 0; i < 6; i++) if (p[28 + i] != iface->mac[i]) return;   // чужой клиент

    uint32_t yiaddr = get_be32(p + 16);

    uint8_t  msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0, dns2 = 0, lease = 0, sid = 0;

    size_t off = 240;
    while (off < len) {
        uint8_t code = p[off++];
        if (code == OPT_END) break;
        if (code == 0) continue;
        if (off >= len) break;
        uint8_t l = p[off++];
        if (off + l > len) break;
        switch (code) {
            case OPT_MSG_TYPE:  if (l >= 1) msg_type = p[off]; break;
            case OPT_SUBNET:    if (l >= 4) subnet = get_be32(p + off); break;
            case OPT_ROUTER:    if (l >= 4) router = get_be32(p + off); break;
            // [48.1] опция 6 может нести список адресов — берём два первых.
            case OPT_DNS:
                if (l >= 4) dns  = get_be32(p + off);
                if (l >= 8) dns2 = get_be32(p + off + 4);
                break;
            case OPT_LEASE:     if (l >= 4) lease  = get_be32(p + off); break;
            case OPT_SERVER_ID: if (l >= 4) sid    = get_be32(p + off); break;
            default: break;
        }
        off += l;
    }
    if (!sid) sid = src_ip;

    dhcp_state_t st = g_dhcp.state;
    if (msg_type == DHCP_OFFER && st == DHCP_ST_SELECTING) {
        serial_puts("[dhcp] OFFER "); netif_dump_ip4(yiaddr);
        serial_puts(" from "); netif_dump_ip4(sid); serial_puts("\n");
        g_dhcp.offered_ip = yiaddr;
        g_dhcp.server_id  = sid;
        g_dhcp.state      = DHCP_ST_OFFER_RECEIVED;
    } else if (msg_type == DHCP_ACK &&
               (st == DHCP_ST_REQUESTING || st == DHCP_ST_RENEWING || st == DHCP_ST_REBINDING)) {
        if (g_dhcp.ack_ready) return;                 // предыдущий ещё не разобран
        g_dhcp.ack.ip     = yiaddr;
        g_dhcp.ack.subnet = subnet ? subnet : 0xFFFFFF00u;
        g_dhcp.ack.router = router;
        g_dhcp.ack.dns    = dns;
        g_dhcp.ack.dns2   = dns2;
        g_dhcp.ack.lease  = lease;
        g_dhcp.ack.server = sid;
        g_dhcp.ack_ready  = true;
    } else if (msg_type == DHCP_NAK && st != DHCP_ST_OFF && st != DHCP_ST_BOUND) {
        g_dhcp.nak_ready = true;
    }
}

bool     dhcp_is_bound(void) {
    dhcp_state_t s = g_dhcp.state;
    return s == DHCP_ST_BOUND || s == DHCP_ST_RENEWING || s == DHCP_ST_REBINDING;
}
bool     dhcp_is_enabled(void)   { return g_dhcp.state != DHCP_ST_OFF; }
uint32_t dhcp_get_ip(void)       { return g_dhcp.cur.ip; }
uint32_t dhcp_get_mask(void)     { return g_dhcp.cur.subnet; }
uint32_t dhcp_get_router(void)   { return g_dhcp.cur.router; }
uint32_t dhcp_get_dns(void)      { return g_dhcp.cur.dns; }
uint32_t dhcp_get_dns2(void)     { return g_dhcp.cur.dns2; }
uint32_t dhcp_get_server(void)   { return g_dhcp.cur.server; }
uint32_t dhcp_get_lease(void)    { return g_dhcp.cur.lease; }
uint32_t dhcp_renew_count(void)  { return g_dhcp.renewals; }

uint32_t dhcp_lease_left_s(void) {
    if (!dhcp_is_bound() || !g_dhcp.expire_ms) return 0;
    uint64_t now = timer_get_uptime_ms();
    return now >= g_dhcp.expire_ms ? 0 : (uint32_t)((g_dhcp.expire_ms - now) / 1000);
}

bool dhcp_is_selecting(void) { return g_dhcp.state == DHCP_ST_SELECTING; }

const char* dhcp_state_name(void) {
    switch (g_dhcp.state) {
        case DHCP_ST_OFF:            return "off";
        case DHCP_ST_SELECTING:      return "discover";
        case DHCP_ST_OFFER_RECEIVED:
        case DHCP_ST_REQUESTING:     return "request";
        case DHCP_ST_BOUND:          return "bound";
        case DHCP_ST_RENEWING:       return "renewing";
        case DHCP_ST_REBINDING:      return "rebinding";
    }
    return "?";
}

int dhcp_take_events(void) {
    int e = g_dhcp.events;
    g_dhcp.events = 0;
    return e;
}

static void ensure_bind(void) {
    if (g_udp_bound) return;
    udp_bind(DHCP_CLIENT_PORT, dhcp_on_udp);
    g_udp_bound = true;
}

static bool send_discover(void) {
    uint8_t buf[300];
    size_t n = build_packet(buf, sizeof(buf), DHCP_DISCOVER, 0, 0, 0);
    if (n == 0) return false;
    serial_puts("[dhcp] DISCOVER xid="); serial_print_hex(g_dhcp.xid, 8);
    serial_puts("\n");
    g_dhcp.last_tx_ms = timer_get_uptime_ms();
    return udp_send_from(0, 0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, buf, n);
}

// Начать (или начать заново) поиск сервера.
static bool begin_discover(void) {
    ensure_bind();
    if (!netif_get_default()) return false;
    // xid — ДО сборки пакета ([41.2]: иначе в DISCOVER уходит предыдущий xid,
    // а ответ сверяется уже с новым и молча выбрасывается).
    g_dhcp.xid        = dhcp_gen_xid();
    g_dhcp.ack_ready  = false;
    g_dhcp.nak_ready  = false;
    g_dhcp.backoff_ms = DISCOVER_FIRST_MS;
    g_dhcp.tries      = 0;
    g_dhcp.state      = DHCP_ST_SELECTING;      // до отправки: OFFER приходит за микросекунды
    return send_discover();
}

static bool send_request(dhcp_state_t st) {
    uint8_t buf[300];
    size_t n;
    bool ok;
    g_dhcp.last_tx_ms = timer_get_uptime_ms();
    if (st == DHCP_ST_REQUESTING) {
        n = build_packet(buf, sizeof(buf), DHCP_REQUEST, 0, g_dhcp.offered_ip, g_dhcp.server_id);
        ok = n && udp_send_from(0, 0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, buf, n);
    } else {
        n = build_packet(buf, sizeof(buf), DHCP_REQUEST, g_dhcp.cur.ip, 0, 0);
        uint32_t dst = (st == DHCP_ST_RENEWING && g_dhcp.cur.server) ? g_dhcp.cur.server : 0xFFFFFFFFu;
        // Продление шлётся серверу напрямую, но только если его MAC уже
        // известен. Ждать ответа ARP здесь нельзя — это цикл WM, а 0.8 с
        // ожидания на каждую попытку были бы заметной запинкой интерфейса.
        // На ВМ 402 сервер называет себя 10.0.2.1 и на ARP не отвечает вовсе
        // (ответы приходят с другого узла) — тогда просим широковещательно,
        // не дожидаясь T2: сервер обязан принять и такой запрос.
        if (dst != 0xFFFFFFFFu && !ipv4_route_known(dst)) dst = 0xFFFFFFFFu;
        ok = n && udp_send_from(g_dhcp.cur.ip, dst, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, buf, n);
    }
    return ok;
}

static void set_timers(uint32_t lease) {
    uint64_t now = timer_get_uptime_ms();
    g_dhcp.bound_ms = now;
    if (g_dhcp.lease_cap && (lease == 0 || lease > g_dhcp.lease_cap)) lease = g_dhcp.lease_cap;
    if (lease == 0 || lease == 0xFFFFFFFFu) {          // бессрочная аренда
        g_dhcp.t1_ms = g_dhcp.t2_ms = g_dhcp.expire_ms = 0;
        return;
    }
    uint64_t ms = (uint64_t)lease * 1000;
    g_dhcp.t1_ms     = now + ms / 2;
    g_dhcp.t2_ms     = now + ms * 7 / 8;
    g_dhcp.expire_ms = now + ms;
}

void dhcp_set_lease_cap(uint32_t seconds) {
    g_dhcp.lease_cap = seconds;
    if (dhcp_is_bound() && seconds) set_timers(g_dhcp.cur.lease);
}

void dhcp_stop(void) {
    g_dhcp.state = DHCP_ST_OFF;
    g_dhcp.ack_ready = g_dhcp.nak_ready = false;
    g_dhcp.events = 0;
}

// Кнопка «Обновить DHCP» и запуск при загрузке. Есть аренда — просим сервер
// её продлить сейчас; нет — начинаем поиск заново.
bool dhcp_start(void) {
    if (!netif_get_default()) return false;
    ensure_bind();
    if (dhcp_is_bound()) {
        g_dhcp.xid   = dhcp_gen_xid();
        g_dhcp.state = DHCP_ST_RENEWING;
        g_dhcp.tries = 0;
        serial_puts("[dhcp] renew requested\n");
        return send_request(DHCP_ST_RENEWING);
    }
    return begin_discover();
}

// Старое имя: REQUEST после OFFER. Теперь это часть dhcp_tick.
bool dhcp_pump(void) {
    bool was = (g_dhcp.state == DHCP_ST_OFFER_RECEIVED);
    dhcp_tick();
    return was;
}

void dhcp_tick(void) {
    dhcp_state_t st = g_dhcp.state;
    if (st == DHCP_ST_OFF) return;
    uint64_t now = timer_get_uptime_ms();

    // ---- ответы сервера, оставленные обработчиком --------------------------
    if (g_dhcp.nak_ready) {
        g_dhcp.nak_ready = false;
        serial_puts("[dhcp] NAK — start over\n");
        if (dhcp_is_bound()) g_dhcp.events |= DHCP_EV_LOST;
        begin_discover();
        return;
    }
    if (g_dhcp.ack_ready) {
        uint64_t fl = net_irq_save();
        dhcp_lease_t a = g_dhcp.ack;
        g_dhcp.ack_ready = false;
        net_irq_restore(fl);

        bool renew = (st == DHCP_ST_RENEWING || st == DHCP_ST_REBINDING);
        g_dhcp.cur   = a;
        g_dhcp.state = DHCP_ST_BOUND;
        set_timers(a.lease);
        if (renew) {
            g_dhcp.renewals++;
            serial_puts("[dhcp] renew: lease extended, ");
            serial_print_dec(dhcp_lease_left_s()); serial_puts(" s\n");
            g_dhcp.events |= DHCP_EV_RENEWED;
        } else {
            serial_puts("[dhcp] ACK — bound, lease ");
            serial_print_dec(a.lease); serial_puts(" s\n");
            g_dhcp.events |= DHCP_EV_BOUND;
        }
        return;
    }

    // ---- таймеры -----------------------------------------------------------
    switch (st) {
    case DHCP_ST_OFFER_RECEIVED:
        // Состояние — ДО отправки (37.5d-3a): slirp отвечает ACK за 100 мкс.
        g_dhcp.state = DHCP_ST_REQUESTING;
        g_dhcp.tries = 0;
        if (send_request(DHCP_ST_REQUESTING)) serial_puts("[dhcp] REQUEST sent (from main thread)\n");
        else                                  serial_puts("[dhcp] REQUEST udp_send FAIL\n");
        break;

    case DHCP_ST_SELECTING:
        if (now - g_dhcp.last_tx_ms >= g_dhcp.backoff_ms) {
            if (g_dhcp.backoff_ms < DISCOVER_MAX_MS) {
                g_dhcp.backoff_ms *= 2;
                if (g_dhcp.backoff_ms > DISCOVER_MAX_MS) g_dhcp.backoff_ms = DISCOVER_MAX_MS;
            }
            send_discover();
        }
        break;

    case DHCP_ST_REQUESTING:
        if (now - g_dhcp.last_tx_ms >= REQUEST_RETRY_MS) {
            if (++g_dhcp.tries > REQUEST_RETRIES) {
                serial_puts("[dhcp] no ACK — start over\n");
                begin_discover();
            } else {
                send_request(DHCP_ST_REQUESTING);
            }
        }
        break;

    case DHCP_ST_BOUND:
        if (g_dhcp.t1_ms && now >= g_dhcp.t1_ms) {
            g_dhcp.xid   = dhcp_gen_xid();
            g_dhcp.state = DHCP_ST_RENEWING;
            serial_puts("[dhcp] T1 reached — renewing\n");
            send_request(DHCP_ST_RENEWING);
        }
        break;

    case DHCP_ST_RENEWING:
    case DHCP_ST_REBINDING: {
        if (g_dhcp.expire_ms && now >= g_dhcp.expire_ms) {
            serial_puts("[dhcp] lease expired — address lost\n");
            g_dhcp.events |= DHCP_EV_LOST;
            for (size_t i = 0; i < sizeof(g_dhcp.cur); i++) ((uint8_t*)&g_dhcp.cur)[i] = 0;
            begin_discover();
            break;
        }
        if (st == DHCP_ST_RENEWING && g_dhcp.t2_ms && now >= g_dhcp.t2_ms) {
            g_dhcp.state = DHCP_ST_REBINDING;
            serial_puts("[dhcp] T2 reached — rebinding (broadcast)\n");
            send_request(DHCP_ST_REBINDING);
            break;
        }
        // Повтор: половина оставшегося до следующего рубежа времени.
        uint64_t limit = (st == DHCP_ST_RENEWING && g_dhcp.t2_ms) ? g_dhcp.t2_ms : g_dhcp.expire_ms;
        uint64_t gap = (limit > now) ? (limit - now) / 2 : RENEW_RETRY_MIN_MS;
        if (gap < RENEW_RETRY_MIN_MS) gap = RENEW_RETRY_MIN_MS;
        if (gap > RENEW_RETRY_MAX_MS) gap = RENEW_RETRY_MAX_MS;
        if (now - g_dhcp.last_tx_ms >= gap) send_request(g_dhcp.state);
        break;
    }
    default: break;
    }
}

void dhcp_dump_state(void) {
    serial_puts("[dhcp] state="); serial_puts(dhcp_state_name());
    serial_puts(" ip="); netif_dump_ip4(g_dhcp.cur.ip);
    serial_puts(" gw="); netif_dump_ip4(g_dhcp.cur.router);
    serial_puts(" dns="); netif_dump_ip4(g_dhcp.cur.dns);
    serial_puts(" lease_left="); serial_print_dec(dhcp_lease_left_s());
    serial_puts("\n");
}
