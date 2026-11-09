// ============================================================================
// netcfg.c — настройка сети: network.conf, DHCP / статика / авто (48.2).
// Описание режимов и формата файла — в netcfg.h.
// ----------------------------------------------------------------------------
// Зачем отдельный слой. Раньше адрес интерфейсу назначал сам DHCP-клиент,
// причём из обработчика прерывания, и другого источника адреса не было: сеть
// без DHCP-сервера (ВМ 402 на изолированном мосте) оставалась без связи.
// Теперь DHCP только сообщает события, а что применять — решает этот файл.
// Всё здесь выполняется в главном потоке.
// ============================================================================
#include "netcfg.h"
#include "netif.h"
#include "dhcp.h"
#include "dns.h"
#include "arp.h"
#include "fat32.h"
#include "timer.h"
#include "serial.h"

static netcfg_t        g_cfg;                       // выбор пользователя
static netcfg_source_t g_src        = NETCFG_SRC_NONE;
static bool            g_from_file  = false;
static uint64_t        g_dhcp_since = 0;            // когда DHCP начал искать сервер

// ---------------------------------------------------------------------------
// Строки и адреса.
// ---------------------------------------------------------------------------
bool netcfg_parse_ip(const char* s, uint32_t* out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    uint32_t parts[4]; int idx = 0, val = 0, digits = 0;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (++digits > 3 || val > 255) return false;
        } else if (c == '.') {
            if (!digits || idx >= 3) return false;
            parts[idx++] = (uint32_t)val; val = 0; digits = 0;
        } else if (c == 0 || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            break;
        } else return false;
    }
    if (!digits || idx != 3) return false;
    parts[3] = (uint32_t)val;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

void netcfg_format_ip(uint32_t ip, char out[16]) {
    int p = 0;
    for (int n = 3; n >= 0; n--) {
        uint8_t v = (uint8_t)(ip >> (n * 8));
        if (v >= 100) out[p++] = (char)('0' + v / 100);
        if (v >= 10)  out[p++] = (char)('0' + (v / 10) % 10);
        out[p++] = (char)('0' + v % 10);
        if (n) out[p++] = '.';
    }
    out[p] = 0;
}

int netcfg_mask_bits(uint32_t mask) {
    int bits = 0;
    while (bits < 32 && (mask & (0x80000000u >> bits))) bits++;
    // После единиц должны идти только нули.
    if (bits < 32 && (mask & (0xFFFFFFFFu >> bits))) return -1;
    return bits;
}

const char* netcfg_mode_name(netcfg_mode_t m) {
    return m == NETCFG_STATIC ? "static" : m == NETCFG_DHCP ? "dhcp" : "auto";
}
const char* netcfg_source_name(void) {
    switch (g_src) {
        case NETCFG_SRC_DHCP:     return "dhcp";
        case NETCFG_SRC_STATIC:   return "static";
        case NETCFG_SRC_FALLBACK: return "static-fallback";
        default:                  return "none";
    }
}
const netcfg_t* netcfg_get(void)              { return &g_cfg; }
netcfg_source_t netcfg_source(void)           { return g_src; }
bool            netcfg_loaded_from_file(void) { return g_from_file; }

// Есть ли в настройке пригодный статический адрес.
static bool has_static(const netcfg_t* c) {
    const char* why;
    netcfg_t t = *c;
    t.mode = NETCFG_STATIC;
    return netcfg_validate(&t, &why);
}

bool netcfg_validate(const netcfg_t* c, const char** why) {
    static const char* dummy;
    if (!why) why = &dummy;
    *why = "";
    if (c->mode != NETCFG_STATIC) {
        // В режимах dhcp/auto статическая часть необязательна, но если адрес
        // указан — он должен быть правильным (иначе «запасной» не сработает).
        if (c->ip == 0) return true;
    }
    uint32_t a = c->ip >> 24;
    if (c->ip == 0)                      { *why = "IP-адрес не задан"; return false; }
    if (a == 0 || a == 127 || a >= 224)  { *why = "IP-адрес из недопустимого диапазона"; return false; }
    int bits = netcfg_mask_bits(c->mask);
    if (bits < 0)                        { *why = "Маска должна быть сплошной (например 255.255.255.0)"; return false; }
    if (bits < 8 || bits > 30)           { *why = "Маска: от 255.0.0.0 до 255.255.255.252"; return false; }
    uint32_t host = c->ip & ~c->mask;
    if (host == 0)                       { *why = "IP совпадает с адресом сети"; return false; }
    if (host == (~c->mask))              { *why = "IP совпадает с широковещательным адресом"; return false; }
    if (c->gateway) {
        if ((c->gateway & c->mask) != (c->ip & c->mask)) { *why = "Шлюз должен быть в той же подсети"; return false; }
        if (c->gateway == c->ip)         { *why = "Шлюз не может совпадать с адресом"; return false; }
        uint32_t gh = c->gateway & ~c->mask;
        if (gh == 0 || gh == (~c->mask)) { *why = "Недопустимый адрес шлюза"; return false; }
    }
    uint32_t d = c->dns >> 24, d2 = c->dns2 >> 24;
    if (c->dns  && (d  == 0 || d  >= 224)) { *why = "Недопустимый адрес DNS"; return false; }
    if (c->dns2 && (d2 == 0 || d2 >= 224)) { *why = "Недопустимый адрес второго DNS"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Файл.
// ---------------------------------------------------------------------------
static bool key_is(const char* line, const char* key, const char** val) {
    int i = 0;
    while (key[i] && line[i] == key[i]) i++;
    if (key[i] || line[i] != '=') return false;
    *val = line + i + 1;
    return true;
}

static void parse_conf(const char* buf, int len, netcfg_t* c) {
    int i = 0;
    while (i < len) {
        const char* line = buf + i;
        const char* v;
        uint32_t ip;
        if (key_is(line, "mode", &v)) {
            if (v[0] == 's')      c->mode = NETCFG_STATIC;
            else if (v[0] == 'd') c->mode = NETCFG_DHCP;
            else                  c->mode = NETCFG_AUTO;
        }
        else if (key_is(line, "ip", &v)      && netcfg_parse_ip(v, &ip)) c->ip = ip;
        else if (key_is(line, "mask", &v)    && netcfg_parse_ip(v, &ip)) c->mask = ip;
        else if (key_is(line, "gateway", &v) && netcfg_parse_ip(v, &ip)) c->gateway = ip;
        else if (key_is(line, "dns2", &v)    && netcfg_parse_ip(v, &ip)) c->dns2 = ip;
        else if (key_is(line, "dns", &v)     && netcfg_parse_ip(v, &ip)) c->dns = ip;
        while (i < len && buf[i] != '\n') i++;
        i++;
    }
}

static bool load_file(netcfg_t* c) {
    if (!fat32_is_mounted()) return false;
    static char buf[512];
    int64_t n = fat32_read_file(NETCFG_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    netcfg_t t = { NETCFG_AUTO, 0, 0, 0, 0, 0 };
    parse_conf(buf, (int)n, &t);
    *c = t;
    return true;
}

static int put_s(char* b, int n, const char* s) { while (*s) b[n++] = *s++; return n; }
static int put_ip(char* b, int n, const char* key, uint32_t ip) {
    if (!ip) return n;
    char t[16];
    netcfg_format_ip(ip, t);
    n = put_s(b, n, key); b[n++] = '=';
    n = put_s(b, n, t);   b[n++] = '\n';
    return n;
}

bool netcfg_save(void) {
    if (!fat32_is_mounted()) return false;
    fat32_mkdir_p("/disk/Sanctum");
    static char b[320];
    int n = 0;
    n = put_s(b, n, "# AMPLIFY-OS network settings (Mesh)\n");
    n = put_s(b, n, "mode="); n = put_s(b, n, netcfg_mode_name(g_cfg.mode)); b[n++] = '\n';
    n = put_ip(b, n, "ip",      g_cfg.ip);
    n = put_ip(b, n, "mask",    g_cfg.mask);
    n = put_ip(b, n, "gateway", g_cfg.gateway);
    n = put_ip(b, n, "dns",     g_cfg.dns);
    n = put_ip(b, n, "dns2",    g_cfg.dns2);
    bool ok = fat32_save_file(NETCFG_PATH, b, (uint64_t)n) >= 0;
    serial_puts(ok ? "[netcfg] saved " NETCFG_PATH "\n" : "[netcfg] save FAILED\n");
    if (ok) g_from_file = true;
    return ok;
}

// ---------------------------------------------------------------------------
// Применение.
// ---------------------------------------------------------------------------
static void log_applied(const char* what) {
    net_interface_t* iface = netif_get_default();
    serial_puts("[netcfg] "); serial_puts(what);
    serial_puts(": ip=");   netif_dump_ip4(iface ? iface->ip4_addr : 0);
    serial_puts(" mask=");  netif_dump_ip4(iface ? iface->ip4_netmask : 0);
    serial_puts(" gw=");    netif_dump_ip4(iface ? iface->ip4_gateway : 0);
    serial_puts(" dns=");   netif_dump_ip4(dns_get_server());
    serial_puts("\n");
}

static void set_addr(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns, uint32_t dns2) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return;
    bool changed = iface->ip4_addr != ip || iface->ip4_netmask != mask || iface->ip4_gateway != gw;
    // Три поля меняются вместе: обработчик прерывания не должен увидеть новый
    // адрес со старой маской.
    uint64_t fl = net_irq_save();
    iface->ip4_addr = ip; iface->ip4_netmask = mask; iface->ip4_gateway = gw;
    net_irq_restore(fl);
    dns_set_server(dns);
    dns_set_server2(dns2);
    if (changed) {
        // Другая сеть — прежние соседи и имена недействительны.
        arp_cache_flush();
        dns_cache_flush();
        if (ip) arp_announce();
    }
}

static void apply_static(netcfg_source_t src) {
    // DNS не указан — пробуем шлюз (обычно он же DNS-прокси); dns.c добавит
    // к списку и публичные серверы.
    set_addr(g_cfg.ip, g_cfg.mask, g_cfg.gateway, g_cfg.dns, g_cfg.dns2);
    g_src = src;
    log_applied(src == NETCFG_SRC_FALLBACK ? "DHCP silent, static fallback applied" : "static applied");
}

static void apply_dhcp_lease(void) {
    set_addr(dhcp_get_ip(), dhcp_get_mask(), dhcp_get_router(), dhcp_get_dns(), dhcp_get_dns2());
    g_src = NETCFG_SRC_DHCP;
    log_applied("DHCP lease applied");
}

static void drop_addr(void) {
    set_addr(0, 0, 0, 0, 0);
    g_src = NETCFG_SRC_NONE;
}

// Привести систему в соответствие с g_cfg.
static void activate(void) {
    if (!netif_get_default()) return;
    if (g_cfg.mode == NETCFG_STATIC) {
        dhcp_stop();
        apply_static(NETCFG_SRC_STATIC);
        return;
    }
    // dhcp / auto. Статический адрес от прошлого режима в режиме dhcp убираем;
    // в auto он остаётся рабочим запасным, пока сервер не ответит.
    if (g_cfg.mode == NETCFG_DHCP && (g_src == NETCFG_SRC_STATIC || g_src == NETCFG_SRC_FALLBACK)) drop_addr();
    if (g_cfg.mode == NETCFG_AUTO && g_src == NETCFG_SRC_STATIC) {
        if (has_static(&g_cfg)) g_src = NETCFG_SRC_FALLBACK; else drop_addr();
    }
    g_dhcp_since = timer_get_uptime_ms();
    if (dhcp_is_bound() && g_src != NETCFG_SRC_DHCP) apply_dhcp_lease();   // аренда ещё жива
    dhcp_start();
}

bool netcfg_apply(const netcfg_t* c, const char** why) {
    if (!netcfg_validate(c, why)) return false;
    g_cfg = *c;
    serial_puts("[netcfg] apply mode="); serial_puts(netcfg_mode_name(g_cfg.mode)); serial_puts("\n");
    activate();
    return true;
}

void netcfg_tick(void) {
    if (!netif_get_default()) return;
    dhcp_tick();
    int ev = dhcp_take_events();
    if (g_cfg.mode == NETCFG_STATIC) return;

    if (ev & (DHCP_EV_BOUND | DHCP_EV_RENEWED)) apply_dhcp_lease();
    if (ev & DHCP_EV_LOST) {
        if (g_src == NETCFG_SRC_DHCP) {
            serial_puts("[netcfg] DHCP address lost\n");
            drop_addr();
        }
        g_dhcp_since = timer_get_uptime_ms();
    }
    // auto: DHCP молчит — берём адрес из файла. DHCP продолжает спрашивать.
    if (g_cfg.mode == NETCFG_AUTO && g_src == NETCFG_SRC_NONE && !dhcp_is_bound() &&
        has_static(&g_cfg) && timer_get_uptime_ms() - g_dhcp_since >= NETCFG_FALLBACK_MS) {
        apply_static(NETCFG_SRC_FALLBACK);
    }
}

void netcfg_boot(void) {
    if (!netif_get_default()) return;
    netcfg_t t = { NETCFG_AUTO, 0, 0, 0, 0, 0 };
    g_from_file = load_file(&t);
    const char* why = "";
    if (g_from_file && !netcfg_validate(&t, &why)) {
        serial_puts("[netcfg] network.conf rejected, using DHCP\n");
        t.mode = NETCFG_AUTO; t.ip = t.mask = t.gateway = t.dns = t.dns2 = 0;
    }
    g_cfg = t;
    serial_puts("[netcfg] mode="); serial_puts(netcfg_mode_name(g_cfg.mode));
    serial_puts(g_from_file ? " (network.conf)\n" : " (default, no network.conf)\n");

    dns_init();
    activate();
    if (g_cfg.mode != NETCFG_STATIC) {
        // Ждём адрес: без запасной статики — до 5 с (как раньше), с ней —
        // пока не сработает запасной адрес (3 с).
        uint64_t t0 = timer_get_uptime_ms();
        while (g_src == NETCFG_SRC_NONE && timer_get_uptime_ms() - t0 < NETCFG_BOOT_WAIT_MS) {
            netcfg_tick();
            __asm__ __volatile__("sti; hlt");
        }
        if (g_src == NETCFG_SRC_NONE)
            serial_puts("[net] DHCP not bound at boot — interface idle, DHCP keeps retrying\n");
    }
}

void netcfg_late_load(void) {
    if (g_from_file || !netif_get_default()) return;
    netcfg_t t;
    if (!load_file(&t)) return;
    const char* why = "";
    if (!netcfg_validate(&t, &why)) { serial_puts("[netcfg] network.conf rejected\n"); return; }
    g_from_file = true;
    g_cfg = t;
    serial_puts("[netcfg] network.conf loaded late, mode=");
    serial_puts(netcfg_mode_name(g_cfg.mode)); serial_puts("\n");
    activate();
}
