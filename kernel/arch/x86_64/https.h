// ============================================================================
// https.h — HTTPS клиент (этап 47.5, расширен в 48.1).
// ----------------------------------------------------------------------------
// Обёртка над tcp.c + tls.c + http.c для GET-запросов по HTTPS.
// Этап 48.1 добавил:
//   * HTTP/1.1 + chunked transfer-encoding;
//   * разрешение любых относительных Location (не только "/path");
//   * net_fetch() — единая точка входа для http:// и https:// с
//     кросс-схемными редиректами (именно её использует браузер).
// ============================================================================
#ifndef AOS_X86_64_HTTPS_H
#define AOS_X86_64_HTTPS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// HTTPS GET.  URL формата https://host[:port]/path.  Если не указан port — 443.
//
// require_trust:
//   true  — chain validation должна пройти; иначе fail.
//   false — невалидная цепочка не fail'ит, но печатается warning.
bool https_get(const char* url,
               char* body_buf, size_t body_cap, size_t* body_len,
               int* status_code,
               bool require_trust);

// Расширенный вариант — дополнительно возвращает HTTP-заголовки
// (без status line, строки разделены CRLF).
bool https_get_ex(const char* url,
                  char* body_buf, size_t body_cap, size_t* body_len,
                  char* hdrs_buf, size_t hdrs_cap, size_t* hdrs_len,
                  int* status_code,
                  bool require_trust);

// Follow-redirect HTTPS GET.  До `max_redirects` шагов по Location при
// 301/302/303/307/308.  final_url — где мы в итоге оказались (можно NULL).
bool https_get_follow(const char* url,
                      char* body_buf, size_t body_cap, size_t* body_len,
                      int* status_code,
                      char* final_url, size_t final_url_cap,
                      int max_redirects,
                      bool require_trust);

// [48.1] Универсальная загрузка страницы.
// * Принимает URL с http://, https:// или без схемы (тогда сначала
//   пробуется https://, затем http://).
// * Сама идёт по редиректам, включая переходы http↔https.
// * require_trust=false — пускает сайты с непроверенной цепочкой (warning).
bool net_fetch(const char* url,
               char* body_buf, size_t body_cap, size_t* body_len,
               int* status_code,
               char* final_url, size_t final_url_cap,
               int max_redirects,
               bool require_trust);

// ----------------------------------------------------------------------------
// [48.2] Загрузка для браузера: буфер и лимит задаёт вызывающий.
// ----------------------------------------------------------------------------
#include "netif.h"
#include "http.h"
#define NET_URL_MAX HTTP_URL_MAX
typedef struct {
    // вход
    char*  body;  size_t body_cap;
    // выход
    size_t body_len;
    int    status;                    // код HTTP последнего ответа
    char   final_url[NET_URL_MAX];    // куда попали после редиректов
    char   content_type[96];
    bool   truncated;                 // тело больше буфера, остаток отброшен
    bool   complete;                  // тело получено целиком
    bool   https;                     // итоговый адрес — https
    bool   cert_trusted;              // цепочка сертификата проверена
    int    redirects;
    net_error_t error;                // причина отказа (NET_OK при успехе)
} net_fetch_t;

bool net_fetch_ex(const char* url, net_fetch_t* f, int max_redirects, bool require_trust);

#endif
