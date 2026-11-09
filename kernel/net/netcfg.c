// ============================================================================
// netcfg.c — загрузка/сохранение настроек сети.
// ============================================================================

#include "netcfg.h"
#include "netif.h"
#include "dns.h"
#include "../fs/vfs.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NETCFG_PATH "/disk/Sanctum/network.cfg"

static netcfg_t g_cfg = {0, 0, 0, 0, false};

const netcfg_t* netcfg_get(void) { return &g_cfg; }

static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static int s_strlen(const char* s) { int n=0; while (s[n]) n++; return n; }
static char* s_skip_ws(char* s) { while (*s == ' ' || *s == '\t') s++; return s; }

bool netcfg_parse_ip(const char* s, uint32_t* out) {
    uint32_t parts[4] = {0};
    int idx = 0, val = 0, digits = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            digits++;
            if (val > 255) return false;
        } else if (*s == '.') {
            if (!digits || idx >= 3) return false;
            parts[idx++] = (uint32_t)val; val = 0; digits = 0;
        } else {
            return false;
        }
        s++;
    }
    if (!digits || idx != 3) return false;
    parts[3] = (uint32_t)val;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

void netcfg_format_ip(uint32_t ip, char* out, int cap) {
    int a = (int)((ip >> 24) & 0xFF);
    int b = (int)((ip >> 16) & 0xFF);
    int c = (int)((ip >>  8) & 0xFF);
    int d = (int)( ip        & 0xFF);
    int n = 0;
    int parts[4] = { a, b, c, d };
    for (int p = 0; p < 4; p++) {
        int v = parts[p];
        char tmp[4]; int tl = 0;
        if (v == 0) { tmp[tl++] = '0'; }
        else { while (v > 0) { tmp[tl++] = '0' + (v % 10); v /= 10; } }
        for (int k = tl - 1; k >= 0; k--) {
            if (n + 1 < cap) out[n++] = tmp[k];
        }
        if (p < 3 && n + 1 < cap) out[n++] = '.';
    }
    if (n < cap) out[n] = 0;
    else if (cap > 0) out[cap - 1] = 0;
}

bool netcfg_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    g_cfg.ip = ip; g_cfg.mask = mask; g_cfg.gw = gw; g_cfg.dns = dns;
    g_cfg.valid = true;
    return netcfg_apply();
}

bool netcfg_apply(void) {
    if (!g_cfg.valid) return false;
    net_interface_t* ifc = netif_get_default();
    if (!ifc) return false;
    netif_set_ip4(ifc, g_cfg.ip, g_cfg.mask, g_cfg.gw);
    dns_set_server(g_cfg.dns);
    dbg_puts("[netcfg] applied static config\n");
    return true;
}

static bool parse_kv(char* line, uint32_t* ip, uint32_t* mask,
                     uint32_t* gw, uint32_t* dns) {
    line = s_skip_ws(line);
    if (*line == '#' || *line == 0 || *line == '\r' || *line == '\n') return false;
    char* eq = line;
    while (*eq && *eq != '=' && *eq != '\r' && *eq != '\n') eq++;
    if (*eq != '=') return false;
    *eq = 0;
    char* val = s_skip_ws(eq + 1);
    char* end = val;
    while (*end && *end != '\r' && *end != '\n') end++;
    *end = 0;
    uint32_t v;
    if (s_strncmp(line, "ip", 3) == 0) {
        if (netcfg_parse_ip(val, &v)) { *ip = v; return true; }
    } else if (s_strncmp(line, "mask", 5) == 0) {
        if (netcfg_parse_ip(val, &v)) { *mask = v; return true; }
    } else if (s_strncmp(line, "gw", 3) == 0 || s_strncmp(line, "gateway", 8) == 0) {
        if (netcfg_parse_ip(val, &v)) { *gw = v; return true; }
    } else if (s_strncmp(line, "dns", 4) == 0) {
        if (netcfg_parse_ip(val, &v)) { *dns = v; return true; }
    }
    return false;
}

void netcfg_init(void) {
    int fd = vfs_open_file(NETCFG_PATH, VFS_O_RDONLY);
    if (fd < 0) {
        dbg_puts("[netcfg] no network.cfg yet\n");
        return;
    }
    char buf[512];
    int n = vfs_read_file(fd, buf, sizeof(buf) - 1);
    vfs_close_file(fd);
    if (n <= 0) return;
    buf[n] = 0;

    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }
        parse_kv(line, &ip, &mask, &gw, &dns);
    }
    if (ip && mask) {
        g_cfg.ip = ip; g_cfg.mask = mask;
        g_cfg.gw = gw; g_cfg.dns = dns;
        g_cfg.valid = true;
        netcfg_apply();
        dbg_puts("[netcfg] loaded from disk\n");
    }
}

bool netcfg_save(void) {
    if (!g_cfg.valid) return false;
    int fd = vfs_open_file(NETCFG_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        dbg_puts("[netcfg] save: cannot open\n");
        return false;
    }
    char buf[256]; int len = 0;
    char ips[24], ms[24], gs[24], ds[24];
    netcfg_format_ip(g_cfg.ip, ips, sizeof(ips));
    netcfg_format_ip(g_cfg.mask, ms, sizeof(ms));
    netcfg_format_ip(g_cfg.gw, gs, sizeof(gs));
    netcfg_format_ip(g_cfg.dns, ds, sizeof(ds));
    const char* keys[] = { "ip", "mask", "gw", "dns" };
    char* vals[] = { ips, ms, gs, ds };
    for (int i = 0; i < 4; i++) {
        const char* k = keys[i]; const char* v = vals[i];
        while (*k && len + 1 < (int)sizeof(buf)) buf[len++] = *k++;
        if (len + 1 < (int)sizeof(buf)) buf[len++] = '=';
        while (*v && len + 1 < (int)sizeof(buf)) buf[len++] = *v++;
        if (len + 1 < (int)sizeof(buf)) buf[len++] = '\n';
    }
    int n = vfs_write_file(fd, buf, len);
    vfs_close_file(fd);
    if (n != len) {
        dbg_puts("[netcfg] save: short write\n");
        return false;
    }
    dbg_puts("[netcfg] saved to disk\n");
    return true;
}
