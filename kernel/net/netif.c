// ============================================================================
// netif.c — реализация абстракции сетевого интерфейса
// ----------------------------------------------------------------------------
// Минимум логики: один статический слот, регистрация, приёмная точка входа
// для драйвера, передача вниз к Ethernet-диспетчеру.
// ============================================================================

#include "netif.h"
#include "ethernet.h"
#include "../drivers/rtl8139.h"
#include "../debug_serial.h"

#include <stddef.h>

// Единственный активный интерфейс (пока не поддерживаем несколько карт).
static net_interface_t* g_default_iface = NULL;

static const char hexd[] = "0123456789abcdef";

void netif_dump_mac(const uint8_t* mac) {
    if (!mac) { dbg_puts("(null MAC)"); return; }
    char s[18];
    int p = 0;
    for (int i = 0; i < 6; i++) {
        s[p++] = hexd[(mac[i] >> 4) & 0xF];
        s[p++] = hexd[mac[i] & 0xF];
        if (i < 5) s[p++] = ':';
    }
    s[p] = 0;
    dbg_puts(s);
}

// --- IP helpers ---
static void u8_to_dec(uint8_t v, char* buf, int* pos) {
    char tmp[4];
    int t = 0;
    if (v == 0) { tmp[t++] = '0'; }
    else while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
    while (t > 0) buf[(*pos)++] = tmp[--t];
}

void netif_dump_ip4(uint32_t addr) {
    char s[16];
    int p = 0;
    u8_to_dec((uint8_t)((addr >> 24) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)((addr >> 16) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)((addr >>  8) & 0xFF), s, &p); s[p++] = '.';
    u8_to_dec((uint8_t)((addr      ) & 0xFF), s, &p);
    s[p] = 0;
    dbg_puts(s);
}

void netif_set_ip4(net_interface_t* iface,
                   uint32_t addr, uint32_t netmask, uint32_t gateway) {
    if (!iface) return;
    iface->ip4_addr    = addr;
    iface->ip4_netmask = netmask;
    iface->ip4_gateway = gateway;
    dbg_puts("[netif] ");
    dbg_puts(iface->name);
    dbg_puts(" ip=");
    netif_dump_ip4(addr);
    dbg_puts(" mask=");
    netif_dump_ip4(netmask);
    dbg_puts(" gw=");
    netif_dump_ip4(gateway);
    dbg_puts("\n");
}

bool netif_register(net_interface_t* iface) {
    if (!iface) return false;
    if (g_default_iface) {
        dbg_puts("[netif] already have default interface, skip\n");
        return false;
    }
    g_default_iface = iface;
    iface->up = true;

    dbg_puts("[netif] registered ");
    dbg_puts(iface->name);
    dbg_puts(" MAC=");
    netif_dump_mac(iface->mac);
    dbg_puts("\n");
    return true;
}

net_interface_t* netif_get_default(void) {
    return g_default_iface;
}

size_t netif_send(const void* data, size_t len) {
    if (!g_default_iface || !g_default_iface->up || !g_default_iface->send) {
        return 0;
    }
    size_t n = g_default_iface->send(data, len);
    if (n > 0) {
        g_default_iface->tx_packets++;
        g_default_iface->tx_bytes += n;
    }
    return n;
}

void netif_receive(const void* data, size_t len) {
    if (!g_default_iface || !g_default_iface->up) return;
    g_default_iface->rx_packets++;
    g_default_iface->rx_bytes += len;

    // Передаём на Ethernet-диспетчер.
    ethernet_rx(data, len);
}

// ----------------------------------------------------------------------------
// net_init — общая инициализация сетевой подсистемы.
// ----------------------------------------------------------------------------
// На этом этапе просто пробуем поднять RTL8139. На 26+ добавится ARP/IP/UDP.
bool net_init(void) {
    dbg_puts("[net] initializing subsystem\n");
    if (rtl8139_init()) {
        dbg_puts("[net] rtl8139 up\n");
        return true;
    }
    dbg_puts("[net] no network card found\n");
    return false;
}
