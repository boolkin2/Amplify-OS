// ============================================================================
// dns.h — DNS-резолвер для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 48.1: несколько серверов с fallback'ом, повторы, кеш, случайные
// source port / query ID.
// ============================================================================
#ifndef AOS_X86_64_DNS_H
#define AOS_X86_64_DNS_H

#include <stdint.h>
#include <stdbool.h>

// Суммарный бюджет одного запроса (таймаут попытки × попытки × серверы)
// ограничен внутри dns.c; это значение оставлено для совместимости.
#define DNS_TIMEOUT_MS  3000

void     dns_init(void);
uint32_t dns_resolve(const char* name);

// Primary / secondary DNS (обычно из DHCP опции 6).
void     dns_set_server(uint32_t srv_ip);
void     dns_set_server2(uint32_t srv_ip);

// Сбросить кеш имён (например, после смены сети / нового DHCP-lease).
void     dns_cache_flush(void);

// [48.2] Текущие серверы и причина последней неудачи dns_resolve.
uint32_t dns_get_server(void);
uint32_t dns_get_server2(void);
typedef enum {
    DNS_FAIL_NONE = 0,
    DNS_FAIL_NO_NET,       // нет интерфейса или адреса
    DNS_FAIL_NO_REPLY,     // ни один сервер не ответил
    DNS_FAIL_NXDOMAIN,     // серверы отвечают: такого имени нет
} dns_failure_t;
dns_failure_t dns_last_failure(void);

#endif
