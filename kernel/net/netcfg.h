// ============================================================================
// netcfg.h — пользовательская конфигурация сети (Этап 34)
// ----------------------------------------------------------------------------
// Файл: /disk/Sanctum/network.cfg (текст, по строке на параметр):
//   ip=192.168.1.10
//   mask=255.255.255.0
//   gw=192.168.1.1
//   dns=8.8.8.8
//
// При старте системы читается и применяется через netif_set_ip4() +
// dns_set_server(). Можно сохранить новые значения через netcfg_save().
// ============================================================================

#ifndef NET_NETCFG_H
#define NET_NETCFG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t ip;       // host-byte-order
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;      // 0 = use gw
    bool     valid;    // true если конфиг успешно загружен
} netcfg_t;

void netcfg_init(void);                     // прочитать с диска и применить
const netcfg_t* netcfg_get(void);
bool netcfg_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
bool netcfg_save(void);                     // записать в /Sanctum/network.cfg
bool netcfg_apply(void);                    // применить текущие значения к netif

// Утилиты для UI: парс/печать "a.b.c.d" <-> uint32 (host-byte-order)
bool netcfg_parse_ip(const char* s, uint32_t* out);
void netcfg_format_ip(uint32_t ip, char* out, int cap);

#endif // NET_NETCFG_H
