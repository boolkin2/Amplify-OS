// ============================================================================
// http.h — HTTP/1.1 GET-клиент для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 48.1: HTTP/1.1, chunked transfer-encoding, Content-Length,
// разбор заголовков, разрешение относительных URL, follow-redirect.
//
// API:
//   http_get      — простой GET (тело + статус).
//   http_get_ex   — GET + заголовки ответа.
//   http_get_follow — GET с переходом по 30x Location.
// Вспомогательные функции (используются также https.c):
//   http_find_header, http_body_is_chunked, http_dechunk, http_resolve_url,
//   http_url_is_https.
// ============================================================================
#ifndef AOS_X86_64_HTTP_H
#define AOS_X86_64_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// [48.2] Предел длины адреса. Было 768 (а путь — 512): адреса перенаправлений
// на страницы входа и проверки (ya.ru, mail.ru) длиннее, они молча обрезались,
// и сервер отвечал 400. Запрос = адрес + Cookie (до 2 КБ) + заголовки.
#define HTTP_URL_MAX  2048
#define HTTP_REQ_MAX  6144

// Разбор URL.  Порт по умолчанию: 443 для https://, иначе 80.
// Фрагмент (#...) отбрасывается.
bool http_parse_url(const char* url,
                    char* host_out, int host_cap,
                    int* port_out,
                    char* path_out, int path_cap);

// true если URL начинается с "https://".
bool http_url_is_https(const char* url);

bool http_get(const char* url,
              char* body_buf, size_t body_cap, size_t* body_len,
              int* status_code);

bool http_get_ex(const char* url,
                 char* body_buf, size_t body_cap, size_t* body_len,
                 char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                 int* status_code);

bool http_get_follow(const char* url,
                     char* body_buf, size_t body_cap, size_t* body_len,
                     int* status_code,
                     char* final_url, size_t final_url_cap,
                     int max_redirects);

// Поиск заголовка (case-insensitive).  Возвращает указатель на value
// без ведущих пробелов или NULL.  *vlen — длина до CR/LF.
const char* http_find_header(const char* hdrs, size_t hlen,
                             const char* name, size_t* vlen);

// true если в заголовках есть "Transfer-Encoding: chunked".
bool http_body_is_chunked(const char* hdrs, size_t hlen);

// Раскодировать chunked-тело на месте.  Возвращает новую длину.
size_t http_dechunk(char* buf, size_t len);

// Разрешить ссылку `loc` (длиной loc_len) относительно base_url.
// Поддерживает: абсолютные URL, "//host/path", "/path", "path",
// "../path", "?query", "#fragment".  Возвращает false для mailto:,
// javascript:, data: и прочих неподдерживаемых схем.
bool http_resolve_url(const char* base_url,
                      const char* loc, size_t loc_len,
                      char* out, size_t out_cap);

// ----------------------------------------------------------------------------
// [48.2] Потоковое чтение ответа (общее для http.c и https.c).
// ----------------------------------------------------------------------------
typedef struct {
    // вход
    char*   body;      size_t body_cap;    // куда писать тело (NUL в конце)
    char*   hdrs;      size_t hdrs_cap;    // заголовки без строки статуса; можно 0
    // выход
    size_t  body_len;
    size_t  hdrs_len;
    int     status;
    int64_t content_length;                // -1 — сервер длину не назвал
    bool    chunked;
    bool    truncated;                     // тело не влезло в body_cap
    bool    complete;                      // тело получено целиком
} http_resp_t;

// Источник байт: >0 — прочитано, 0 — данных пока нет, <0 — поток закончился.
typedef int (*http_recv_fn)(void* ctx, void* buf, size_t cap);

bool http_read_response(http_recv_fn rx, void* ctx, http_resp_t* r);
int  http_build_request(char* req, int cap, const char* host, int port,
                        int default_port, const char* path);
// Дополнения к следующим запросам: строки заголовков ("Cookie: …\r\n", можно
// 0) и согласие на gzip. Сбросить — (0, false).
void http_set_request_opts(const char* extra_headers, bool accept_gzip);
// GET по http:// с заполнением http_resp_t (без редиректов).
bool http_fetch_plain(const char* url, http_resp_t* r);

#endif
