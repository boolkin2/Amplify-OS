// ============================================================================
// http.h — простой HTTP/1.0 GET-клиент (Этап 34)
// ----------------------------------------------------------------------------
// Использует tcp_connect + tcp_send + tcp_recv. Поддерживает только
// GET-запросы по http:// (без HTTPS, без перенаправлений).
//
// API:
//   http_get(url, body_buf, body_cap, &body_len, status_code) — блокирующий
//   GET. Возвращает true при успехе. body_buf заполняется содержимым
//   ответа (без HTTP-заголовков).
//
// Парсинг URL: "http://host[:port]/path".
// ============================================================================

#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool http_parse_url(const char* url,
                    char* host_out, int host_cap,
                    int* port_out,
                    char* path_out, int path_cap);

bool http_get(const char* url,
              char* body_buf, size_t body_cap, size_t* body_len,
              int* status_code);

#endif // NET_HTTP_H
