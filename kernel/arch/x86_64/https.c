// ============================================================================
// https.c — HTTPS GET (этап 47.5) + net_fetch (этап 48.1, 48.2).
// ----------------------------------------------------------------------------
// DNS resolve → TCP connect → TLS handshake → HTTP/1.1 GET → разбор ответа.
//
// Подэтап 48.2:
//   * собственная копия разбора ответа (ещё один статический буфер 128 КБ)
//     удалена: тело читает общий потоковый http_read_response (http.c), ему
//     просто подставляется источник байт tls_recv;
//   * net_fetch_ex — загрузка для браузера: буфер и лимит задаёт вызывающий,
//     обратно идут статус, итоговый адрес, Content-Type, признаки «обрезано»,
//     «не целиком», «сертификат проверен» и причина отказа (net_error_t);
//   * угаданная схема (адрес без http://) повторяется по http только когда в
//     этом есть смысл: при отказе DNS или отсутствии сети второй заход лишь
//     удваивал ожидание.
// ============================================================================
#include "https.h"
#include "tcp.h"
#include "dns.h"
#include "tls.h"
#include "netif.h"
#include "timer.h"
#include "serial.h"
#include "http.h"
#include "cookies.h"
#include "inflate.h"
#include "heap.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Локальные строковые утилиты (минимальные, без libc).
static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void s_strncpy_safe(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Контекст TLS — большой объект (117 КБ). [38.1.0] Из кучи при первом
// соединении, не в .bss (CLAUDE.md §5.50); имя g_tls оставлено макросом, чтобы
// остальной код не менялся. Все обращения идут после выделения в https_*.
static tls_ctx_t* g_tls_p;
#define g_tls (*g_tls_p)

// Источник байт для http_read_response поверх TLS. tls_recv сам ждёт запись
// целиком (через net_idle — экран при этом живёт): >0 — данные, 0 —
// close_notify, <0 — соединение закрыто или ошибка записи. Для разборщика
// оба последних случая — конец потока.
static int tls_source(void* ctx, void* buf, size_t cap) {
    int n = tls_recv((tls_ctx_t*)ctx, buf, cap);
    return n > 0 ? n : -1;
}

// GET по https:// с заполнением http_resp_t (без редиректов).
// trusted_out — прошёл ли сертификат проверку цепочки.
static bool https_fetch(const char* url, http_resp_t* r, bool require_trust, bool* trusted_out) {
    static char host[256], path[HTTP_URL_MAX];
    int  port = 443;
    if (trusted_out) *trusted_out = false;
    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        serial_puts("[https] bad URL\n");
        net_set_error(NET_ERR_URL);
        return false;
    }

    net_progress_set(NET_STAGE_DNS, 0, -1);
    uint32_t ip = dns_resolve(host);
    if (!ip) {
        serial_puts("[https] dns failed for ");
        serial_puts(host);
        serial_puts("\n");
        if (net_last_error() != NET_ERR_CANCELLED)
            net_set_error(dns_last_failure() == DNS_FAIL_NO_NET ? NET_ERR_NO_ADDR : NET_ERR_DNS);
        return false;
    }

    net_progress_set(NET_STAGE_CONNECT, 0, -1);
    if (!tcp_connect(ip, (uint16_t)port)) {
        serial_puts("[https] tcp_connect failed\n");
        return false;                                // причину записал tcp_connect
    }

    net_progress_set(NET_STAGE_TLS, 0, -1);
    if (!g_tls_p) g_tls_p = (tls_ctx_t*)kcalloc(1, sizeof(tls_ctx_t));
    if (!g_tls_p) {
        serial_puts("[https] no memory for the TLS context\n");
        net_set_error(NET_ERR_TLS);
        tcp_abort();
        return false;
    }
    tls_client_init(&g_tls, -1, host);
    if (!tls_client_handshake(&g_tls)) {
        serial_puts("[https] TLS handshake failed\n");
        if (net_last_error() != NET_ERR_CANCELLED)
            net_set_error(tls_handshake_silent() ? NET_ERR_TLS_SILENT : NET_ERR_TLS);
        tcp_abort();
        return false;
    }
    if (trusted_out) *trusted_out = g_tls.server_cert_trusted;
    if (require_trust && !g_tls.server_cert_trusted) {
        serial_puts("[https] cert NOT TRUSTED, aborting (require_trust=true)\n");
        net_set_error(NET_ERR_TLS_TRUST);
        tls_close(&g_tls);
        tcp_close();
        return false;
    }

    static char req[HTTP_REQ_MAX];
    int rlen = http_build_request(req, sizeof(req), host, port, 443, path);
    net_progress_set(NET_STAGE_REQUEST, 0, -1);
    if (tls_send(&g_tls, req, rlen) != rlen) {
        serial_puts("[https] tls_send failed\n");
        net_set_error(tcp_was_reset() ? NET_ERR_RESET : NET_ERR_TIMEOUT);
        tls_close(&g_tls);
        tcp_abort();
        return false;
    }

    bool ok = http_read_response(tls_source, &g_tls, r);
    if (ok && r->complete) { tls_close(&g_tls); tcp_close(); }
    else                   { tls_close(&g_tls); tcp_abort(); }
    if (!ok) serial_puts("[https] no response\n");
    return ok;
}

static bool https_get_impl(const char* url,
                           char* body_buf, size_t body_cap, size_t* body_len,
                           char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                           int* status_code,
                           bool require_trust) {
    http_resp_t r;
    r.body = body_buf; r.body_cap = body_cap;
    r.hdrs = hdrs_buf; r.hdrs_cap = hdrs_cap;
    if (body_len)    *body_len = 0;
    if (hdrs_len)    *hdrs_len = 0;
    if (status_code) *status_code = 0;
    if (body_cap == 0) return false;
    if (!https_fetch(url, &r, require_trust, 0)) return false;
    if (body_len)    *body_len = r.body_len;
    if (hdrs_len)    *hdrs_len = r.hdrs_len;
    if (status_code) *status_code = r.status;
    return true;
}

bool https_get(const char* url,
               char* body_buf, size_t body_cap, size_t* body_len,
               int* status_code,
               bool require_trust) {
    net_set_error(NET_OK);
    return https_get_impl(url, body_buf, body_cap, body_len,
                          NULL, 0, NULL,
                          status_code, require_trust);
}

bool https_get_ex(const char* url,
                  char* body_buf, size_t body_cap, size_t* body_len,
                  char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                  int* status_code,
                  bool require_trust) {
    return https_get_impl(url, body_buf, body_cap, body_len,
                          hdrs_buf, hdrs_cap, hdrs_len,
                          status_code, require_trust);
}

// ============================================================================
// https_get_follow — follow-redirect HTTPS GET (этап 47.7, улучшен в 48.1).
// ============================================================================
bool https_get_follow(const char* url,
                      char* body_buf, size_t body_cap, size_t* body_len,
                      int* status_code,
                      char* final_url, size_t final_url_cap,
                      int max_redirects,
                      bool require_trust) {
    char cur[768];
    s_strncpy_safe(cur, url, sizeof(cur));
    static char hdrs[8192];

    for (int hop = 0; hop <= max_redirects; hop++) {
        size_t hl = 0;
        if (body_len)    *body_len = 0;
        if (status_code) *status_code = 0;

        bool ok = https_get_impl(cur,
                                 body_buf, body_cap, body_len,
                                 hdrs, sizeof(hdrs), &hl,
                                 status_code, require_trust);
        if (!ok) return false;
        if (final_url) s_strncpy_safe(final_url, cur, final_url_cap);

        int sc = status_code ? *status_code : 0;
        bool is_redirect = (sc == 301 || sc == 302 || sc == 303 ||
                            sc == 307 || sc == 308);
        if (!is_redirect || hop == max_redirects) return true;

        size_t vlen = 0;
        const char* loc = http_find_header(hdrs, hl, "Location", &vlen);
        if (!loc) return true;     // 3xx без Location — отдаём как есть.

        char next[768];
        if (!http_resolve_url(cur, loc, vlen, next, sizeof(next))) {
            serial_puts("[https] cannot follow Location (unsupported form)\n");
            return true;
        }
        serial_puts("[https] redirect ");
        serial_print_dec((uint64_t)sc);
        serial_puts(" -> ");
        serial_puts(next);
        serial_puts("\n");
        s_strncpy_safe(cur, next, sizeof(cur));

        // Редирект на http:// внутри https-клиента не тянем — это дело net_fetch.
        if (!http_url_is_https(cur)) {
            serial_puts("[https] redirect leaves TLS; use net_fetch\n");
            if (final_url) s_strncpy_safe(final_url, cur, final_url_cap);
            return true;
        }
    }
    return true;
}

// ============================================================================
// net_fetch_ex — единая загрузка для браузера (48.1 → 48.2).
// ----------------------------------------------------------------------------
//   * понимает http://, https:// и адрес без схемы ("example.com/x");
//   * редиректы между схемами идут одной цепочкой
//     (http://example.com → https://example.com/ → https://www.example.com/);
//   * тело — потоком в буфер вызывающего, лимит — его размер.
// ============================================================================
bool net_fetch_ex(const char* url, net_fetch_t* f, int max_redirects, bool require_trust) {
    if (!url || !f || !f->body || f->body_cap < 2) return false;
    if (max_redirects < 0)  max_redirects = 0;
    if (max_redirects > 10) max_redirects = 10;

    f->body_len = 0; f->status = 0; f->final_url[0] = 0; f->content_type[0] = 0;
    f->truncated = false; f->complete = false; f->https = false; f->cert_trusted = false;
    f->redirects = 0; f->error = NET_OK;
    net_set_error(NET_OK);
    net_cancel_clear();

    static char cur[NET_URL_MAX];        // static: стек ядра 16 КБ, а ниже — TLS и кадр WM
    bool scheme_guessed = false;

    // Нет схемы? — пробуем https://, затем http:// (как в обычном браузере).
    if (http_url_is_https(url) || s_strncmp(url, "http://", 7) == 0) {
        s_strncpy_safe(cur, url, sizeof(cur));
    } else {
        s_strncpy_safe(cur, "https://", sizeof(cur));
        size_t o = 8;
        for (size_t i = 0; url[i] && o + 1 < sizeof(cur); i++) cur[o++] = url[i];
        cur[o] = 0;
        scheme_guessed = true;
    }

    static char hdrs[32768];
    static char cookie_line[2048];
    static char c_host[256], c_path[HTTP_URL_MAX];
    bool ok = false;
    bool gzip_ok = true;             // просим сжатие; при сбое распаковки — без него

    for (int hop = 0; hop <= max_redirects; hop++) {
        http_resp_t r;
        r.body = f->body; r.body_cap = f->body_cap;
        r.hdrs = hdrs;    r.hdrs_cap = sizeof(hdrs);
        bool tls = http_url_is_https(cur);
        bool trusted = false;

        // Cookies этого сайта — в запрос; ответные Set-Cookie — в хранилище
        // (в том числе с ответов-перенаправлений: на них всё и держится).
        int c_port = 0;
        bool have_host = http_parse_url(cur, c_host, sizeof(c_host), &c_port, c_path, sizeof(c_path));
        cookie_line[0] = 0;
        if (have_host) cookies_header(c_host, c_path, tls, cookie_line, sizeof(cookie_line));
        http_set_request_opts(cookie_line[0] ? cookie_line : 0, gzip_ok);

        ok = tls ? https_fetch(cur, &r, require_trust, &trusted)
                 : http_fetch_plain(cur, &r);
        http_set_request_opts(0, false);
        if (ok && have_host) cookies_store(c_host, hdrs, r.hdrs_len);
        s_strncpy_safe(f->final_url, cur, sizeof(f->final_url));
        f->https = tls;
        f->cert_trusted = trusted;

        if (!ok) {
            net_error_t e = net_last_error();
            // Не вышло по https и схему мы угадывали — повторим по http, если
            // дело в самом соединении, а не в сети/DNS/отмене.
            bool worth_retry = (e == NET_ERR_REFUSED || e == NET_ERR_CONNECT || e == NET_ERR_TLS ||
                                e == NET_ERR_TLS_SILENT ||
                                e == NET_ERR_RESET || e == NET_ERR_TIMEOUT || e == NET_ERR_PROTOCOL);
            if (scheme_guessed && tls && worth_retry) {
                static char plain[NET_URL_MAX];
                s_strncpy_safe(plain, "http://", sizeof(plain));
                size_t o = 7;
                for (size_t i = 8; cur[i] && o + 1 < sizeof(plain); i++) plain[o++] = cur[i];
                plain[o] = 0;
                serial_puts("[net] https failed, retry over http: ");
                serial_puts(plain); serial_puts("\n");
                s_strncpy_safe(cur, plain, sizeof(cur));
                scheme_guessed = false;
                net_set_error(NET_OK);
                hop--;                       // повтор не считаем редиректом
                continue;
            }
            break;
        }

        scheme_guessed = false;
        f->body_len  = r.body_len;
        f->status    = r.status;
        f->truncated = r.truncated;
        f->complete  = r.complete;
        {
            size_t vl = 0;
            const char* ct = http_find_header(hdrs, r.hdrs_len, "Content-Type", &vl);
            size_t n = (ct && vl < sizeof(f->content_type)) ? vl : (ct ? sizeof(f->content_type) - 1 : 0);
            for (size_t i = 0; i < n; i++) f->content_type[i] = ct[i];
            f->content_type[n] = 0;
        }

        int sc = r.status;
        bool is_redirect = (sc == 301 || sc == 302 || sc == 303 ||
                            sc == 307 || sc == 308);
        if (!is_redirect) {
            // Сжатое тело (мы просили gzip; часть серверов сжимает и без спроса).
            size_t el = 0;
            const char* enc = http_find_header(hdrs, r.hdrs_len, "Content-Encoding", &el);
            bool packed = false;
            for (size_t i = 0; enc && i + 3 < el + 1 && !packed; i++)
                packed = (i + 4 <= el && s_strncmp(enc + i, "gzip", 4) == 0) ||
                         (i + 7 <= el && s_strncmp(enc + i, "deflate", 7) == 0);
            if (enc && el >= 2 && !packed && !(el >= 8 && s_strncmp(enc, "identity", 8) == 0)) {
                serial_puts("[net] unsupported Content-Encoding\n");
                net_set_error(NET_ERR_PROTOCOL);
                ok = false;
                break;
            }
            if (packed && r.body_len > 0) {
                uint8_t* tmp = (uint8_t*)kmalloc(r.body_len);
                inflate_result_t ir = INFLATE_ERR_DATA;
                size_t out_len = 0;
                if (tmp) {
                    for (size_t i = 0; i < r.body_len; i++) tmp[i] = (uint8_t)f->body[i];
                    ir = inflate_http(tmp, r.body_len, (uint8_t*)f->body, f->body_cap - 1, &out_len);
                    kfree(tmp);
                }
                if (ir == INFLATE_OK || ir == INFLATE_TRUNCATED) {
                    serial_puts("[net] gzip: "); serial_print_dec((uint64_t)r.body_len);
                    serial_puts(" -> "); serial_print_dec((uint64_t)out_len); serial_puts(" bytes\n");
                    f->body[out_len] = 0;
                    f->body_len = out_len;
                    if (ir == INFLATE_TRUNCATED) { f->truncated = true; f->complete = false; }
                } else if (gzip_ok) {
                    // Распаковать не вышло — один раз повторяем без сжатия.
                    serial_puts("[net] gzip decode failed, retry with identity\n");
                    gzip_ok = false;
                    hop--;
                    continue;
                } else {
                    net_set_error(NET_ERR_PROTOCOL);
                    ok = false;
                }
            }
            break;
        }

        size_t vlen = 0;
        const char* loc = http_find_header(hdrs, r.hdrs_len, "Location", &vlen);
        if (!loc) break;
        if (hop == max_redirects) {
            serial_puts("[net] too many redirects\n");
            net_set_error(NET_ERR_REDIRECTS);
            ok = false;
            break;
        }

        static char next[NET_URL_MAX];
        if (!http_resolve_url(cur, loc, vlen, next, sizeof(next))) {
            serial_puts("[net] unsupported Location, stop\n");
            break;
        }
        serial_puts("[net] redirect ");
        serial_print_dec((uint64_t)sc);
        serial_puts(" -> ");
        serial_puts(next);
        serial_puts("\n");
        s_strncpy_safe(cur, next, sizeof(cur));
        f->redirects++;
    }

    f->error = ok ? NET_OK : net_last_error();
    if (!ok && f->error == NET_OK) f->error = NET_ERR_PROTOCOL;
    net_progress_set(NET_STAGE_DONE, f->body_len, -1);
    if (!ok) {
        serial_puts("[net] fetch failed: "); serial_puts(net_error_name(f->error));
        serial_puts(" url="); serial_puts(f->final_url); serial_puts("\n");
    }
    return ok;
}

// Прежний интерфейс (shell, старые вызовы).
bool net_fetch(const char* url,
               char* body_buf, size_t body_cap, size_t* body_len,
               int* status_code,
               char* final_url, size_t final_url_cap,
               int max_redirects,
               bool require_trust) {
    static net_fetch_t f;
    f.body = body_buf; f.body_cap = body_cap;
    bool ok = net_fetch_ex(url, &f, max_redirects, require_trust);
    if (body_len)    *body_len = f.body_len;
    if (status_code) *status_code = f.status;
    if (final_url)   s_strncpy_safe(final_url, f.final_url, final_url_cap);
    return ok;
}
