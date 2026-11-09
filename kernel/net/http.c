// ============================================================================
// http.c — простой HTTP/1.0 GET клиент.
// ============================================================================

#include "http.h"
#include "tcp.h"
#include "dns.h"
#include "../drivers/timer.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static int s_strlen(const char* s) { int n=0; while (s[n]) n++; return n; }
static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static char s_tolower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int s_strncasecmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char x = s_tolower(a[i]); char y = s_tolower(b[i]);
        if (x != y) return (unsigned char)x - (unsigned char)y;
        if (!x) return 0;
    }
    return 0;
}

bool http_parse_url(const char* url,
                    char* host_out, int host_cap,
                    int* port_out,
                    char* path_out, int path_cap) {
    if (!url) return false;
    const char* p = url;
    if (s_strncasecmp(p, "http://", 7) == 0) p += 7;
    else if (s_strncasecmp(p, "https://", 8) == 0) p += 8;  // не поддерживаем, но парсим как http

    // host[:port]
    const char* host_start = p;
    int port = 80;
    while (*p && *p != '/' && *p != ':') p++;
    int host_len = (int)(p - host_start);
    if (host_len <= 0 || host_len >= host_cap) return false;
    for (int i = 0; i < host_len; i++) host_out[i] = host_start[i];
    host_out[host_len] = 0;

    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    }
    *port_out = port;

    // path (всё до конца)
    if (*p == 0) {
        if (path_cap < 2) return false;
        path_out[0] = '/'; path_out[1] = 0;
    } else {
        int n = 0;
        while (p[n] && n < path_cap - 1) { path_out[n] = p[n]; n++; }
        path_out[n] = 0;
    }
    return true;
}

bool http_get(const char* url,
              char* body_buf, size_t body_cap, size_t* body_len,
              int* status_code) {
    char host[128], path[256];
    int  port = 80;
    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        dbg_puts("[http] bad URL\n");
        return false;
    }

    uint32_t ip = dns_resolve(host);
    if (!ip) {
        dbg_puts("[http] dns failed for ");
        dbg_puts(host); dbg_puts("\n");
        return false;
    }

    if (!tcp_connect(ip, (uint16_t)port)) {
        dbg_puts("[http] tcp_connect failed\n");
        return false;
    }

    // Сформировать запрос
    char req[512];
    int len = 0;
    const char* prefix = "GET ";
    while (*prefix) req[len++] = *prefix++;
    for (int i = 0; path[i] && len < (int)sizeof(req) - 64; i++) req[len++] = path[i];
    const char* mid = " HTTP/1.0\r\nHost: ";
    while (*mid) req[len++] = *mid++;
    for (int i = 0; host[i] && len < (int)sizeof(req) - 32; i++) req[len++] = host[i];
    const char* tail = "\r\nUser-Agent: AmplifyOS/1.0\r\nConnection: close\r\n\r\n";
    while (*tail) req[len++] = *tail++;

    if (tcp_send(req, (size_t)len) == 0) {
        dbg_puts("[http] tcp_send failed\n");
        tcp_close();
        return false;
    }

    // Буфер ответа
    static char raw[16 * 1024];
    size_t raw_len = 0;
    uint32_t t0 = (uint32_t)timer_get_uptime_ms();
    while ((uint32_t)timer_get_uptime_ms() - t0 < 5000) {
        size_t got = 0;
        if (tcp_recv(raw + raw_len, sizeof(raw) - raw_len, &got) && got > 0) {
            raw_len += got;
            t0 = (uint32_t)timer_get_uptime_ms();
            if (raw_len >= sizeof(raw) - 64) break;
        } else {
            if (tcp_current_state() == 0) break;  // CLOSED
            __asm__ volatile("sti; hlt");
        }
    }

    tcp_close();

    if (raw_len == 0) {
        dbg_puts("[http] no data\n");
        return false;
    }

    // Парсим status line
    *status_code = 0;
    if (raw_len > 12 && s_strncmp(raw, "HTTP/", 5) == 0) {
        // HTTP/1.x SSS ...
        int sp = 0; while (sp < (int)raw_len && raw[sp] != ' ') sp++;
        int code = 0;
        while (sp < (int)raw_len && raw[sp] == ' ') sp++;
        while (sp < (int)raw_len && raw[sp] >= '0' && raw[sp] <= '9') {
            code = code * 10 + raw[sp] - '0'; sp++;
        }
        *status_code = code;
    }

    // Найти конец заголовков "\r\n\r\n"
    size_t body_off = 0;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i+1] == '\n' && raw[i+2] == '\r' && raw[i+3] == '\n') {
            body_off = i + 4; break;
        }
        if (raw[i] == '\n' && raw[i+1] == '\n') {
            body_off = i + 2; break;
        }
    }
    if (body_off == 0) body_off = raw_len;  // нет заголовков?

    size_t bl = raw_len - body_off;
    if (bl > body_cap - 1) bl = body_cap - 1;
    for (size_t i = 0; i < bl; i++) body_buf[i] = raw[body_off + i];
    body_buf[bl] = 0;
    *body_len = bl;
    return true;
}
