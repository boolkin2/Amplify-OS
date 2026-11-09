// ============================================================================
// netif.c — netif реализация.
// ============================================================================
#include "netif.h"
#include "ethernet.h"
#include "rtl8139.h"
#include "e1000.h"
#include "virtio_net.h"
#include "serial.h"
#include "http.h"
#include "heap.h"
#include "timer.h"

static net_interface_t* g_default = 0;
static const char hexd[] = "0123456789abcdef";

void netif_dump_mac(const uint8_t* mac) {
    if (!mac) { serial_puts("(null MAC)"); return; }
    char s[18]; int p = 0;
    for (int i = 0; i < 6; i++) {
        s[p++] = hexd[(mac[i] >> 4) & 0xF];
        s[p++] = hexd[mac[i] & 0xF];
        if (i < 5) s[p++] = ':';
    }
    s[p] = 0;
    serial_puts(s);
}

static void u8_to_dec(uint8_t v, char* buf, int* pos) {
    char tmp[4]; int t = 0;
    if (v == 0) { tmp[t++] = '0'; }
    else while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t > 0) buf[(*pos)++] = tmp[--t];
}

void netif_dump_ip4(uint32_t addr) {
    char s[16]; int p = 0;
    u8_to_dec((uint8_t)((addr >> 24) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)((addr >> 16) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)((addr >>  8) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)(addr        & 0xFF), s, &p);
    s[p] = 0;
    serial_puts(s);
}

void netif_set_ip4(net_interface_t* iface, uint32_t addr, uint32_t mask, uint32_t gw) {
    if (!iface) return;
    iface->ip4_addr = addr; iface->ip4_netmask = mask; iface->ip4_gateway = gw;
    serial_puts("[netif] "); serial_puts(iface->name);
    serial_puts(" ip="); netif_dump_ip4(addr);
    serial_puts(" mask="); netif_dump_ip4(mask);
    serial_puts(" gw="); netif_dump_ip4(gw);
    serial_puts("\n");
}

bool netif_register(net_interface_t* iface) {
    if (!iface) return false;
    if (g_default) { serial_puts("[netif] default already set\n"); return false; }
    g_default = iface; iface->up = true;
    serial_puts("[netif] registered ");
    serial_puts(iface->name);
    serial_puts(" MAC=");
    netif_dump_mac(iface->mac);
    serial_puts("\n");
    return true;
}

net_interface_t* netif_get_default(void) { return g_default; }

bool netif_is_ready(void) { return g_default != 0 && g_default->up; }

size_t netif_send(const void* data, size_t len) {
    if (!g_default || !g_default->up || !g_default->send) return 0;
    size_t n = g_default->send(data, len);
    if (n > 0) { g_default->tx_packets++; g_default->tx_bytes += n; }
    return n;
}

void netif_receive(const void* data, size_t len) {
    if (!g_default || !g_default->up) return;
    g_default->rx_packets++; g_default->rx_bytes += len;
    ethernet_rx(data, len);
}

bool net_init(void) {
    serial_puts("[net] initializing\n");
    // [41.2] Сначала паравиртуальная virtio-net (ВМ QEMU/Proxmox с
    // `net0: virtio=...`); эмулируемые карты ниже — запасной путь.
    if (virtio_net_init()) {
        serial_puts("[net] virtio-net up\n");
        return true;
    }
    // Этап 47.8: пробуем e1000 первым (Intel — VMware/QEMU/Hyper-V/real HW),
    // потом RTL8139 (классический QEMU default).
    if (e1000_init()) {
        serial_puts("[net] e1000 up\n");
        return true;
    }
    if (rtl8139_init()) {
        serial_puts("[net] rtl8139 up\n");
        return true;
    }
    serial_puts("[net] no NIC found\n");
    return false;
}

const char* netif_driver_name(void) {
    if (virtio_net_is_ready()) return "virtio-net";
    if (e1000_is_ready())      return "e1000";
    if (rtl8139_is_ready())    return "rtl8139";
    return "none";
}

// ----------------------------------------------------------------------------
// [48.2] Ожидание, отмена, причина отказа, ход загрузки (описание — netif.h).
// ----------------------------------------------------------------------------
static volatile net_error_t g_net_err   = NET_OK;
static volatile bool        g_cancel    = false;
static net_idle_hook_t      g_idle_hook = 0;
static bool                 g_in_hook   = false;
static uint64_t             g_hook_ms   = 0;

static volatile net_stage_t g_stage = NET_STAGE_IDLE;
static volatile uint64_t    g_prog_done  = 0;
static volatile int64_t     g_prog_total = -1;

void        net_set_error(net_error_t e) { g_net_err = e; }
net_error_t net_last_error(void)         { return g_net_err; }

const char* net_error_name(net_error_t e) {
    switch (e) {
        case NET_OK:              return "ok";
        case NET_ERR_NO_IFACE:    return "no-iface";
        case NET_ERR_NO_ADDR:     return "no-address";
        case NET_ERR_URL:         return "bad-url";
        case NET_ERR_DNS:         return "dns";
        case NET_ERR_UNREACHABLE: return "unreachable";
        case NET_ERR_REFUSED:     return "refused";
        case NET_ERR_CONNECT:     return "connect-timeout";
        case NET_ERR_TLS:         return "tls";
        case NET_ERR_TLS_TRUST:   return "tls-trust";
        case NET_ERR_TLS_SILENT:  return "tls-silent";
        case NET_ERR_RESET:       return "reset";
        case NET_ERR_TIMEOUT:     return "timeout";
        case NET_ERR_PROTOCOL:    return "protocol";
        case NET_ERR_REDIRECTS:   return "redirects";
        case NET_ERR_CANCELLED:   return "cancelled";
    }
    return "?";
}

void net_set_idle_hook(net_idle_hook_t fn) { g_idle_hook = fn; }
void net_cancel(void)       { g_cancel = true; }
void net_cancel_clear(void) { g_cancel = false; }
bool net_cancelled(void)    { return g_cancel; }

void net_idle(void) {
    __asm__ __volatile__("sti; hlt");
    // Насос кадров — только из главного потока (сюда из обработчиков не
    // приходят: ждать в них запрещено) и без вложенности: кадр рисует окна,
    // а окно не должно запустить вторую загрузку внутри первой.
    if (!g_idle_hook || g_in_hook) return;
    uint64_t now = timer_get_uptime_ms();
    if (now - g_hook_ms < 33) return;
    g_hook_ms = now;
    g_in_hook = true;
    g_idle_hook();
    g_in_hook = false;
}

void net_progress_set(net_stage_t st, uint64_t done, int64_t total) {
    g_stage = st; g_prog_done = done; g_prog_total = total;
}
net_stage_t net_progress_stage(void) { return g_stage; }
uint64_t    net_progress_done(void)  { return g_prog_done; }
int64_t     net_progress_total(void) { return g_prog_total; }

// ----------------------------------------------------------------------------
// [41.2] Замер сети: качать файл по HTTP и напечатать скорость. Включается
// строкой `netbench=<url>` в /disk/Sanctum/perf.conf (тест поднимает на хосте
// http-сервер, из гостя он виден как 10.0.2.2). Файл берётся небольшой (тест
// отдаёт 100 КБ) и запрашивается много раз подряд — до 64 раз или 4 секунд:
// так в цифру входит и установление соединения на каждый запрос.
// Сравнивает e1000 и virtio-net на одном и том же стеке TCP: цифра упирается
// и в сам стек (установление соединения на каждый запрос), и в карту.
// ----------------------------------------------------------------------------
void net_bench_report(const char* url) {
    static char buf[128 * 1024];
    uint64_t total = 0; int reqs = 0, code = 0; size_t one = 0;
    uint64_t t0 = timer_get_uptime_ms();
    while (reqs < 64 && timer_get_uptime_ms() - t0 < 4000) {
        size_t len = 0;
        if (!http_get(url, buf, sizeof(buf), &len, &code) || code != 200) break;
        total += len; one = len; reqs++;
    }
    uint64_t ms = timer_get_uptime_ms() - t0;
    if (ms == 0) ms = 1;
    serial_puts("[netbench] ");
    serial_puts(g_default ? g_default->name : "?");
    serial_puts(virtio_net_is_ready() ? " (virtio-net): " : " (emulated NIC): ");
    if (reqs == 0) { serial_puts("FAILED, http "); serial_print_dec((uint64_t)code); serial_puts("\n"); return; }
    serial_print_dec((uint64_t)reqs); serial_puts(" requests x ");
    serial_print_dec((uint64_t)one);  serial_puts(" bytes in ");
    serial_print_dec(ms); serial_puts(" ms = ");
    serial_print_dec(total / 1024 * 1000 / ms); serial_puts(" KiB/s");
    if (virtio_net_is_ready()) {
        serial_puts(" [rx=");      serial_print_dec(virtio_net_rx_frames());
        serial_puts(" tx=");       serial_print_dec(virtio_net_tx_frames());
        serial_puts(" irq=");      serial_print_dec(virtio_net_irqs());
        serial_puts(" tx_drops="); serial_print_dec(virtio_net_tx_drops());
        serial_puts("]");
    }
    serial_puts("\n");
}
