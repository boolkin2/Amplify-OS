// ============================================================================
// http.c — HTTP/1.1 GET-клиент: разбор URL, заголовков и ПОТОКОВОЕ чтение ответа.
// ----------------------------------------------------------------------------
// Этап 48.1: HTTP/1.1 с Accept/Connection: close, chunked, Content-Length,
// редиректы, http_resolve_url для относительных ссылок.
//
// Подэтап 48.2 — потоковый разбор (http_read_response):
//   Раньше ответ целиком читался в статический буфер 128 КБ (HTTP_RAW_CAP), из
//   него тело копировалось в буфер вызывающего, а chunked раскодировался уже
//   потом, на месте. Такой же код, слово в слово, жил в https.c со своими
//   128 КБ. Страница больше 128 КБ обрезалась молча, а при chunked — ещё и
//   посреди куска. Теперь:
//     * заголовки копятся в небольшом буфере (HTTP_HDR_CAP), тело пишется
//       сразу в буфер вызывающего — лимит один, у вызывающего (браузер: 2 МБ);
//     * chunked раскодируется на лету конечным автоматом;
//     * если тело не влезло — флаг truncated, чтение прекращается, соединение
//       рвётся (RST), а не тянет остаток файла в никуда;
//     * один и тот же разборщик работает поверх TCP и поверх TLS: источник
//       байт — функция http_recv_fn (https.c передаёт tls_recv).
//   Причина отказа — в net_set_error (netif.h), ход загрузки — net_progress_set.
// ============================================================================

#include "http.h"
#include "tcp.h"
#include "dns.h"
#include "netif.h"
#include "timer.h"
#include "serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HTTP_HDR_CAP      (32 * 1024)      // с десятком Set-Cookie заголовки бывают больше 16 КБ
#define HTTP_IDLE_MS      30000      // тишина между кусками ответа (сервер собирает страницу на ходу)
#define HTTP_TOTAL_MS     300000     // вся загрузка; прервать раньше — Esc

// ----------------------------------------------------------------------------
// Tiny string helpers.
// ----------------------------------------------------------------------------
static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static char s_tolower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int s_strncasecmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char x = s_tolower(a[i]); char y = s_tolower(b[i]);
        if (x != y) return (unsigned char)x - (unsigned char)y;
        if (!x) return 0;
    }
    return 0;
}
static size_t s_len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void s_copy(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

bool http_url_is_https(const char* url) {
    return url && s_strncasecmp(url, "https://", 8) == 0;
}

// ----------------------------------------------------------------------------
// Парсинг URL: [scheme://]host[:port][/path][?query]  (#fragment отбрасывается).
// ----------------------------------------------------------------------------
bool http_parse_url(const char* url,
                    char* host_out, int host_cap,
                    int* port_out,
                    char* path_out, int path_cap) {
    if (!url) return false;
    const char* p = url;
    int port = 80;
    if (s_strncasecmp(p, "http://",  7) == 0) { p += 7; port = 80;  }
    else if (s_strncasecmp(p, "https://", 8) == 0) { p += 8; port = 443; }

    const char* host_start = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    int host_len = (int)(p - host_start);
    if (host_len <= 0 || host_len >= host_cap) return false;
    for (int i = 0; i < host_len; i++) host_out[i] = host_start[i];
    host_out[host_len] = 0;

    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
        if (port <= 0 || port > 65535) return false;
    }
    *port_out = port;

    if (*p == 0 || *p == '#') {
        if (path_cap < 2) return false;
        path_out[0] = '/'; path_out[1] = 0;
    } else {
        int n = 0;
        if (*p == '?') { if (path_cap < 3) return false; path_out[n++] = '/'; }
        int i = 0;
        while (p[i] && p[i] != '#' && n < path_cap - 1) { path_out[n++] = p[i]; i++; }
        path_out[n] = 0;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Заголовки.
// ----------------------------------------------------------------------------
const char* http_find_header(const char* hdrs, size_t hlen,
                             const char* name, size_t* vlen) {
    if (!hdrs || !name || !vlen) return NULL;
    size_t nlen = s_len(name);
    if (nlen == 0 || hlen == 0) return NULL;
    for (size_t i = 0; i + nlen + 1 <= hlen; i++) {
        bool at_line_start = (i == 0) || (hdrs[i-1] == '\n');
        if (!at_line_start) continue;
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            if (s_tolower(hdrs[i + k]) != s_tolower(name[k])) { match = false; break; }
        }
        if (!match || hdrs[i + nlen] != ':') continue;
        size_t v = i + nlen + 1;
        while (v < hlen && (hdrs[v] == ' ' || hdrs[v] == '\t')) v++;
        size_t end = v;
        while (end < hlen && hdrs[end] != '\r' && hdrs[end] != '\n') end++;
        *vlen = end - v;
        return hdrs + v;
    }
    return NULL;
}

bool http_body_is_chunked(const char* hdrs, size_t hlen) {
    size_t vlen = 0;
    const char* v = http_find_header(hdrs, hlen, "Transfer-Encoding", &vlen);
    if (!v) return false;
    for (size_t i = 0; i + 7 <= vlen; i++) {
        if (s_strncasecmp(v + i, "chunked", 7) == 0) return true;
    }
    return false;
}

static long header_content_length(const char* hdrs, size_t hlen) {
    size_t vlen = 0;
    const char* v = http_find_header(hdrs, hlen, "Content-Length", &vlen);
    if (!v) return -1;
    long n = 0; bool any = false;
    for (size_t i = 0; i < vlen; i++) {
        if (v[i] < '0' || v[i] > '9') break;
        n = n * 10 + (v[i] - '0'); any = true;
    }
    return any ? n : -1;
}

// ----------------------------------------------------------------------------
// chunked → plain, на месте.
// Формат: <hex-размер>[;ext]CRLF <данные> CRLF ... 0 CRLF [trailer] CRLF
// ----------------------------------------------------------------------------
size_t http_dechunk(char* buf, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        // Размер куска в hex.
        size_t size = 0;
        bool   have_digit = false;
        while (in < len) {
            char c = buf[in];
            int  d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) break;
            size = size * 16 + (size_t)d;
            have_digit = true;
            in++;
        }
        if (!have_digit) break;                       // мусор — стоп
        // Пропустить chunk-extension до CRLF.
        while (in < len && buf[in] != '\n') in++;
        if (in < len) in++;                           // съесть '\n'
        if (size == 0) break;                         // last-chunk
        if (in + size > len) size = len - in;         // обрезанный ответ
        for (size_t i = 0; i < size; i++) buf[out + i] = buf[in + i];
        out += size;
        in  += size;
        // Пропустить CRLF после данных.
        if (in < len && buf[in] == '\r') in++;
        if (in < len && buf[in] == '\n') in++;
    }
    return out;
}

// ----------------------------------------------------------------------------
// Разрешение относительных URL.
// ----------------------------------------------------------------------------
static bool scheme_supported(const char* s, size_t len) {
    if (len >= 7 && s_strncasecmp(s, "http://", 7) == 0) return true;
    if (len >= 8 && s_strncasecmp(s, "https://", 8) == 0) return true;
    return false;
}

// Есть ли в начале строки чужая схема ("mailto:", "javascript:", "tel:").
static bool has_foreign_scheme(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ':') return true;
        if (c == '/' || c == '?' || c == '#') return false;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return false;
}

bool http_resolve_url(const char* base_url,
                      const char* loc, size_t loc_len,
                      char* out, size_t out_cap) {
    if (!loc || !out || out_cap < 2) return false;

    // Обрезать пробелы и фрагмент.
    while (loc_len > 0 && (loc[0] == ' ' || loc[0] == '\t')) { loc++; loc_len--; }
    size_t cut = loc_len;
    for (size_t i = 0; i < loc_len; i++) if (loc[i] == '#') { cut = i; break; }
    loc_len = cut;
    while (loc_len > 0 && (loc[loc_len-1] == ' ' || loc[loc_len-1] == '\t' ||
                           loc[loc_len-1] == '\r' || loc[loc_len-1] == '\n')) loc_len--;
    if (loc_len == 0) return false;

    // Абсолютный URL.
    if (scheme_supported(loc, loc_len)) {
        size_t n = loc_len < out_cap - 1 ? loc_len : out_cap - 1;
        for (size_t i = 0; i < n; i++) out[i] = loc[i];
        out[n] = 0;
        return true;
    }
    // Другая схема (mailto:, javascript:, data:, tel:) — не наше.
    if (has_foreign_scheme(loc, loc_len) &&
        !(loc_len >= 2 && loc[0] == '/' && loc[1] == '/')) return false;

    if (!base_url) return false;

    // Разобрать base: scheme://authority/path
    const char* b = base_url;
    size_t scheme_len = 0;
    while (b[scheme_len] && b[scheme_len] != ':') scheme_len++;
    if (b[scheme_len] != ':' || b[scheme_len+1] != '/' || b[scheme_len+2] != '/') return false;
    size_t auth_start = scheme_len + 3;
    size_t auth_end   = auth_start;
    while (b[auth_end] && b[auth_end] != '/' && b[auth_end] != '?' && b[auth_end] != '#') auth_end++;

    // static: адреса бывают длиной в пару килобайт (перенаправления на
    // страницы входа и проверки), а стек ядра — 32 КБ. Функция зовётся только
    // из главного потока (разбор страницы, перенаправления).
    static char tmp[HTTP_URL_MAX];
    size_t o = 0;

    // "//host/path" — protocol-relative: берём схему из base.
    if (loc_len >= 2 && loc[0] == '/' && loc[1] == '/') {
        for (size_t i = 0; i < scheme_len && o < sizeof(tmp) - 1; i++) tmp[o++] = b[i];
        tmp[o++] = ':';
        for (size_t i = 0; i < loc_len && o < sizeof(tmp) - 1; i++) tmp[o++] = loc[i];
        tmp[o] = 0;
        s_copy(out, tmp, out_cap);
        return true;
    }

    // Основа для абсолютного/относительного пути: scheme://authority
    for (size_t i = 0; i < auth_end && o < sizeof(tmp) - 1; i++) tmp[o++] = b[i];

    if (loc[0] == '/') {
        // Абсолютный путь.
        for (size_t i = 0; i < loc_len && o < sizeof(tmp) - 1; i++) tmp[o++] = loc[i];
        tmp[o] = 0;
        s_copy(out, tmp, out_cap);
        return true;
    }

    // Собрать текущий путь base без query и без последнего сегмента.
    size_t path_start = auth_end;
    size_t path_end   = path_start;
    while (b[path_end] && b[path_end] != '?' && b[path_end] != '#') path_end++;

    if (loc[0] == '?') {
        // Только query — путь остаётся текущим.
        if (path_end == path_start) { if (o < sizeof(tmp) - 1) tmp[o++] = '/'; }
        else for (size_t i = path_start; i < path_end && o < sizeof(tmp) - 1; i++) tmp[o++] = b[i];
        for (size_t i = 0; i < loc_len && o < sizeof(tmp) - 1; i++) tmp[o++] = loc[i];
        tmp[o] = 0;
        s_copy(out, tmp, out_cap);
        return true;
    }

    // Каталог base'а (всё до последнего '/').
    size_t dir_end = path_start;
    for (size_t i = path_start; i < path_end; i++) if (b[i] == '/') dir_end = i + 1;
    if (dir_end == path_start) {
        if (o < sizeof(tmp) - 1) tmp[o++] = '/';
    } else {
        for (size_t i = path_start; i < dir_end && o < sizeof(tmp) - 1; i++) tmp[o++] = b[i];
    }
    for (size_t i = 0; i < loc_len && o < sizeof(tmp) - 1; i++) tmp[o++] = loc[i];
    tmp[o] = 0;

    // Нормализация "./" и "../" в пути.
    {
        static char norm[HTTP_URL_MAX];
        size_t no = 0;
        // Скопировать scheme://authority как есть.
        size_t i = 0;
        while (i < o && no < sizeof(norm) - 1) {
            norm[no++] = tmp[i];
            if (i >= auth_end - 1) { i++; break; }
            i++;
        }
        // Дальше — посегментно.
        size_t path_base = no;
        while (i < o) {
            if (tmp[i] == '.' && i + 1 < o && tmp[i+1] == '/' &&
                (no == path_base || norm[no-1] == '/')) { i += 2; continue; }
            if (tmp[i] == '.' && i + 2 < o && tmp[i+1] == '.' && tmp[i+2] == '/' &&
                (no == path_base || norm[no-1] == '/')) {
                i += 3;
                if (no > path_base) no--;                       // сойти с '/'
                while (no > path_base && norm[no-1] != '/') no--;
                continue;
            }
            if (no < sizeof(norm) - 1) norm[no++] = tmp[i];
            i++;
        }
        norm[no] = 0;
        s_copy(out, norm, out_cap);
    }
    return true;
}

// ----------------------------------------------------------------------------
// Запрос.
// ----------------------------------------------------------------------------
// Дополнения к запросу на время одной загрузки (их ставит net_fetch_ex):
// готовые строки заголовков (Cookie) и согласие на сжатый ответ.
static const char* g_req_extra = 0;
static bool        g_req_gzip  = false;
void http_set_request_opts(const char* extra_headers, bool accept_gzip) {
    g_req_extra = extra_headers;
    g_req_gzip  = accept_gzip;
}

int http_build_request(char* req, int cap, const char* host, int port,
                       int default_port, const char* path) {
    int len = 0;
    #define PUT(str) do { const char* q_ = (str); while (*q_ && len < cap - 1) req[len++] = *q_++; } while (0)
    PUT("GET ");
    // Путь: пробел и байты вне ASCII (кириллица в ссылке, записанная как есть)
    // в строке запроса недопустимы — уходят как %XX.
    for (const unsigned char* q = (const unsigned char*)path; *q && len < cap - 4; q++) {
        if (*q <= 0x20 || *q >= 0x7F) {
            static const char hx[] = "0123456789ABCDEF";
            req[len++] = '%'; req[len++] = hx[*q >> 4]; req[len++] = hx[*q & 15];
        } else req[len++] = (char)*q;
    }
    PUT(" HTTP/1.1\r\nHost: ");
    PUT(host);
    if (port != default_port) {
        char pb[8]; int pn = 0; int pv = port;
        while (pv > 0 && pn < 6) { pb[pn++] = (char)('0' + pv % 10); pv /= 10; }
        if (len < cap - 1) req[len++] = ':';
        while (pn > 0 && len < cap - 1) req[len++] = pb[--pn];
    }
    PUT("\r\nUser-Agent: Mozilla/5.0 (AmplifyOS; x86_64) AmplifyBrowser/2.0"
        "\r\nAccept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5"
        "\r\nAccept-Language: ru,en;q=0.8"
        "\r\nAccept-Encoding: ");
    PUT(g_req_gzip ? "gzip" : "identity");
    PUT("\r\n");
    if (g_req_extra) PUT(g_req_extra);
    PUT("Connection: close\r\n\r\n");
    #undef PUT
    req[len] = 0;
    return len;
}

// ----------------------------------------------------------------------------
// Потоковое чтение ответа.
// ----------------------------------------------------------------------------
typedef enum { BODY_NONE = 0, BODY_LENGTH, BODY_CHUNKED, BODY_CLOSE } body_mode_t;
typedef enum { CS_SIZE = 0, CS_EXT, CS_DATA, CS_DATA_END, CS_TRAILER } chunk_state_t;

typedef struct {
    http_resp_t*  r;
    body_mode_t   mode;
    bool          done;
    int64_t       remaining;       // BODY_LENGTH: сколько ещё ждём
    chunk_state_t cs;
    uint64_t      chunk_left;
    bool          chunk_any_digit;
    int           trailer_len;
} body_ctx_t;

static void body_put(body_ctx_t* c, const char* p, size_t n) {
    http_resp_t* r = c->r;
    size_t room = (r->body_cap > 0) ? r->body_cap - 1 - r->body_len : 0;
    size_t take = n < room ? n : room;
    for (size_t i = 0; i < take; i++) r->body[r->body_len + i] = p[i];
    r->body_len += take;
    if (take < n) { r->truncated = true; c->done = true; }
}

static void body_feed(body_ctx_t* c, const char* p, size_t n) {
    switch (c->mode) {
    case BODY_NONE:
        c->done = true;
        return;
    case BODY_CLOSE:
        body_put(c, p, n);
        return;
    case BODY_LENGTH: {
        size_t take = (int64_t)n < c->remaining ? n : (size_t)c->remaining;
        body_put(c, p, take);
        c->remaining -= (int64_t)take;
        if (c->remaining <= 0) { c->done = true; c->r->complete = !c->r->truncated; }
        return;
    }
    case BODY_CHUNKED:
        // Формат: <hex-размер>[;ext]CRLF <данные> CRLF ... 0 CRLF [trailer] CRLF
        for (size_t i = 0; i < n && !c->done; ) {
            char ch = p[i];
            switch (c->cs) {
            case CS_SIZE: {
                int d = -1;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                if (d >= 0) { c->chunk_left = c->chunk_left * 16 + (uint64_t)d; c->chunk_any_digit = true; }
                else if (ch == ';') c->cs = CS_EXT;
                else if (ch == '\n') {
                    if (!c->chunk_any_digit) { c->done = true; break; }      // мусор вместо размера
                    if (c->chunk_left == 0) { c->cs = CS_TRAILER; c->trailer_len = 0; }
                    else c->cs = CS_DATA;
                }
                i++;
                break;
            }
            case CS_EXT:
                if (ch == '\n') {
                    if (c->chunk_left == 0) { c->cs = CS_TRAILER; c->trailer_len = 0; }
                    else c->cs = CS_DATA;
                }
                i++;
                break;
            case CS_DATA: {
                size_t avail = n - i;
                size_t take = avail < c->chunk_left ? avail : (size_t)c->chunk_left;
                body_put(c, p + i, take);
                i += take;
                c->chunk_left -= take;
                if (c->chunk_left == 0) c->cs = CS_DATA_END;
                break;
            }
            case CS_DATA_END:
                if (ch == '\n') { c->cs = CS_SIZE; c->chunk_left = 0; c->chunk_any_digit = false; }
                i++;
                break;
            case CS_TRAILER:
                if (ch == '\n') {
                    if (c->trailer_len == 0) { c->done = true; c->r->complete = !c->r->truncated; }
                    c->trailer_len = 0;
                } else if (ch != '\r') c->trailer_len++;
                i++;
                break;
            }
        }
        return;
    }
}

bool http_read_response(http_recv_fn rx, void* ctx, http_resp_t* r) {
    static char    hdr[HTTP_HDR_CAP];
    static uint8_t io[8192];
    size_t hdr_len = 0;
    bool   in_body = false, eof = false, timed_out = false;
    body_ctx_t bc;
    for (size_t i = 0; i < sizeof(bc); i++) ((uint8_t*)&bc)[i] = 0;
    bc.r = r;

    r->body_len = 0; r->hdrs_len = 0; r->status = 0; r->content_length = -1;
    r->chunked = false; r->truncated = false; r->complete = false;
    if (r->body && r->body_cap) r->body[0] = 0;
    if (r->hdrs && r->hdrs_cap) r->hdrs[0] = 0;

    uint64_t t_idle = timer_get_uptime_ms(), t_start = t_idle;

    while (!bc.done) {
        uint64_t now = timer_get_uptime_ms();
        if (net_cancelled()) { net_set_error(NET_ERR_CANCELLED); return false; }
        if (now - t_idle > HTTP_IDLE_MS || now - t_start > HTTP_TOTAL_MS) { timed_out = true; break; }

        int n = rx(ctx, io, sizeof(io));
        if (n == 0) { net_idle(); continue; }
        if (n < 0)  { eof = true; break; }
        t_idle = timer_get_uptime_ms();

        const char* p = (const char*)io;
        size_t left = (size_t)n;

    again:
        if (!in_body) {
            // Копим заголовки до пустой строки.
            size_t scan_from = hdr_len > 3 ? hdr_len - 3 : 0;
            size_t take = left < HTTP_HDR_CAP - 1 - hdr_len ? left : HTTP_HDR_CAP - 1 - hdr_len;
            for (size_t i = 0; i < take; i++) hdr[hdr_len + i] = p[i];
            size_t total = hdr_len + take;
            size_t end = 0;
            for (size_t i = scan_from; i + 1 < total; i++) {
                if (hdr[i] == '\n' && hdr[i+1] == '\n') { end = i + 2; break; }
                if (i + 3 < total && hdr[i] == '\r' && hdr[i+1] == '\n' &&
                    hdr[i+2] == '\r' && hdr[i+3] == '\n') { end = i + 4; break; }
            }
            if (!end) {
                hdr_len = total;
                if (hdr_len >= HTTP_HDR_CAP - 1) {                 // заголовки без конца
                    net_set_error(NET_ERR_PROTOCOL);
                    return false;
                }
                continue;
            }
            // Сколько байт этого куска ушло на заголовки.
            size_t used = end - hdr_len;
            hdr_len = end;
            hdr[hdr_len] = 0;
            p += used; left -= used;

            // Строка статуса: "HTTP/1.1 200 OK".
            if (hdr_len < 12 || s_strncmp(hdr, "HTTP/", 5) != 0) {
                net_set_error(NET_ERR_PROTOCOL);
                return false;
            }
            size_t sp = 0;
            while (sp < hdr_len && hdr[sp] != ' ') sp++;
            while (sp < hdr_len && hdr[sp] == ' ') sp++;
            int code = 0;
            while (sp < hdr_len && hdr[sp] >= '0' && hdr[sp] <= '9') { code = code * 10 + hdr[sp] - '0'; sp++; }

            size_t line_end = 0;
            while (line_end < hdr_len && hdr[line_end] != '\n') line_end++;
            if (line_end < hdr_len) line_end++;

            // 1xx (100 Continue) — промежуточный ответ: выбросить и читать дальше.
            if (code >= 100 && code < 200) {
                // Остаток куска — начало настоящего ответа: разбираем его тем
                // же путём сразу. (Раньше остаток просто откладывался, и если
                // настоящий ответ уже целиком лежал в нём, а сервер больше
                // ничего не слал, чтение ждало до таймаута.)
                hdr_len = 0;
                if (left) goto again;
                continue;
            }

            r->status = code;
            // Заголовки без строки статуса и без завершающей пустой строки.
            size_t hb_end = hdr_len;
            while (hb_end > line_end && (hdr[hb_end-1] == '\n' || hdr[hb_end-1] == '\r')) hb_end--;
            size_t hl = hb_end > line_end ? hb_end - line_end : 0;
            const char* hs = hdr + line_end;
            if (r->hdrs && r->hdrs_cap > 0) {
                size_t c2 = hl < r->hdrs_cap - 1 ? hl : r->hdrs_cap - 1;
                for (size_t i = 0; i < c2; i++) r->hdrs[i] = hs[i];
                r->hdrs[c2] = 0;
                r->hdrs_len = c2;
            }

            r->chunked        = http_body_is_chunked(hs, hl);
            r->content_length = header_content_length(hs, hl);
            if (code == 204 || code == 304)      { bc.mode = BODY_NONE; r->complete = true; }
            else if (r->chunked)                 bc.mode = BODY_CHUNKED;
            else if (r->content_length == 0)     { bc.mode = BODY_NONE; r->complete = true; }
            else if (r->content_length > 0)      { bc.mode = BODY_LENGTH; bc.remaining = r->content_length; }
            else                                 bc.mode = BODY_CLOSE;
            in_body = true;
            net_progress_set(NET_STAGE_BODY, 0, r->content_length);
            if (bc.mode == BODY_NONE) { bc.done = true; break; }
            if (left == 0) continue;
        }

        body_feed(&bc, p, left);
        net_progress_set(NET_STAGE_BODY, r->body_len, r->content_length);
    }

    if (r->body && r->body_cap) r->body[r->body_len] = 0;

    if (!in_body) {
        // Ответа (или хотя бы его заголовков) мы так и не увидели.
        if (timed_out)             net_set_error(NET_ERR_TIMEOUT);
        else if (tcp_was_reset())  net_set_error(NET_ERR_RESET);
        else                       net_set_error(NET_ERR_PROTOCOL);
        serial_puts("[http] no response headers\n");
        return false;
    }
    // Без длины и без chunked конец тела — это закрытие соединения сервером.
    if (bc.mode == BODY_CLOSE && eof && !r->truncated) r->complete = true;
    if (r->truncated) {
        serial_puts("[http] body larger than buffer — truncated at ");
        serial_print_dec(r->body_len); serial_puts(" bytes\n");
    } else if (!r->complete) {
        serial_puts("[http] body incomplete: ");
        serial_print_dec(r->body_len); serial_puts(" bytes\n");
    }
    return true;
}

// Источник байт поверх TCP: >0 — байты, 0 — пока нет, -1 — сервер закрыл.
static int plain_recv(void* ctx, void* buf, size_t cap) {
    (void)ctx;
    size_t got = 0;
    if (tcp_recv(buf, cap, &got) && got > 0) return (int)got;
    return tcp_peer_closed() ? -1 : 0;
}

// ----------------------------------------------------------------------------
// Главная реализация GET поверх TCP.
// ----------------------------------------------------------------------------
bool http_fetch_plain(const char* url, http_resp_t* r) {
    static char host[256], path[HTTP_URL_MAX];
    int  port = 80;
    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        serial_puts("[http] bad URL\n");
        net_set_error(NET_ERR_URL);
        return false;
    }
    if (http_url_is_https(url)) {
        serial_puts("[http] https:// URL passed to plain HTTP client\n");
        net_set_error(NET_ERR_URL);
        return false;
    }

    net_progress_set(NET_STAGE_DNS, 0, -1);
    uint32_t ip = dns_resolve(host);
    if (!ip) {
        serial_puts("[http] dns failed for "); serial_puts(host); serial_puts("\n");
        if (net_last_error() != NET_ERR_CANCELLED)
            net_set_error(dns_last_failure() == DNS_FAIL_NO_NET ? NET_ERR_NO_ADDR : NET_ERR_DNS);
        return false;
    }

    net_progress_set(NET_STAGE_CONNECT, 0, -1);
    if (!tcp_connect(ip, (uint16_t)port)) {
        serial_puts("[http] tcp_connect failed\n");
        return false;                               // причину записал tcp_connect
    }

    // Запрос HTTP/1.1. Connection: close — одно соединение = один ответ.
    static char req[HTTP_REQ_MAX];                  // длинный адрес + Cookie
    int len = http_build_request(req, sizeof(req), host, port, 80, path);
    net_progress_set(NET_STAGE_REQUEST, 0, -1);
    {
        size_t sent = 0;
        while (sent < (size_t)len) {
            size_t n = tcp_send(req + sent, (size_t)len - sent);
            if (n == 0) {
                serial_puts("[http] tcp_send failed\n");
                net_set_error(tcp_was_reset() ? NET_ERR_RESET : NET_ERR_TIMEOUT);
                tcp_abort();
                return false;
            }
            sent += n;
        }
    }

    bool ok = http_read_response(plain_recv, 0, r);
    // Тело получено целиком — вежливое закрытие; иначе (обрезали, отмена,
    // ошибка) рвём соединение, чтобы сервер не слал остаток.
    if (ok && r->complete) tcp_close(); else tcp_abort();
    return ok;
}

static bool http_get_impl(const char* url,
                          char* body_buf, size_t body_cap, size_t* body_len,
                          char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                          int* status_code) {
    http_resp_t r;
    r.body = body_buf; r.body_cap = body_cap;
    r.hdrs = hdrs_buf; r.hdrs_cap = hdrs_cap;
    if (body_len)    *body_len = 0;
    if (hdrs_len)    *hdrs_len = 0;
    if (status_code) *status_code = 0;
    if (body_cap == 0) return false;
    if (!http_fetch_plain(url, &r)) return false;
    if (body_len)    *body_len = r.body_len;
    if (hdrs_len)    *hdrs_len = r.hdrs_len;
    if (status_code) *status_code = r.status;
    return true;
}

bool http_get(const char* url,
              char* body_buf, size_t body_cap, size_t* body_len,
              int* status_code) {
    return http_get_impl(url, body_buf, body_cap, body_len,
                         NULL, 0, NULL, status_code);
}

bool http_get_ex(const char* url,
                 char* body_buf, size_t body_cap, size_t* body_len,
                 char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                 int* status_code) {
    return http_get_impl(url, body_buf, body_cap, body_len,
                         hdrs_buf, hdrs_cap, hdrs_len, status_code);
}

bool http_get_follow(const char* url,
                     char* body_buf, size_t body_cap, size_t* body_len,
                     int* status_code,
                     char* final_url, size_t final_url_cap,
                     int max_redirects) {
    char cur[768];
    s_copy(cur, url, sizeof(cur));
    static char hdrs[8192];

    for (int hop = 0; hop <= max_redirects; hop++) {
        size_t hl = 0;
        if (body_len)    *body_len = 0;
        if (status_code) *status_code = 0;

        if (!http_get_impl(cur, body_buf, body_cap, body_len,
                           hdrs, sizeof(hdrs), &hl, status_code)) return false;
        if (final_url) s_copy(final_url, cur, final_url_cap);

        int sc = status_code ? *status_code : 0;
        bool is_redirect = (sc == 301 || sc == 302 || sc == 303 ||
                            sc == 307 || sc == 308);
        if (!is_redirect || hop == max_redirects) return true;

        size_t vlen = 0;
        const char* loc = http_find_header(hdrs, hl, "Location", &vlen);
        if (!loc) return true;

        char next[768];
        if (!http_resolve_url(cur, loc, vlen, next, sizeof(next))) {
            serial_puts("[http] cannot follow Location\n");
            return true;
        }
        serial_puts("[http] redirect -> "); serial_puts(next); serial_puts("\n");
        s_copy(cur, next, sizeof(cur));

        // Редирект на https:// плайн-клиент выполнить не может — этим
        // занимается net_fetch() в https.c (его использует браузер).
        if (http_url_is_https(cur)) {
            serial_puts("[http] redirect to https:// requires net_fetch\n");
            if (final_url) s_copy(final_url, cur, final_url_cap);
            return true;
        }
    }
    return true;
}
