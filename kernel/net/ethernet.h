// ============================================================================
// ethernet.h — разбор и сборка Ethernet II фреймов (Этап 25)
// ----------------------------------------------------------------------------
// Формат Ethernet II:
//   [dst MAC: 6][src MAC: 6][EtherType: 2 BE][payload][FCS: 4, добавляет NIC]
// Для RTL8139 FCS карта ставит/срезает сама — в наш RX-буфер попадают 4 байта
// FCS после пакета (и они учтены в reported length), на TX NIC добавляет сама.
//
// На этом этапе делаем только:
//   • ethernet_rx(frame, len) — разбирает входящий фрейм, печатает src/dst/type
//     в serial и передаёт дальше (в будущем — по EtherType: 0x0806 → ARP,
//     0x0800 → IPv4).
//   • ethernet_build(dst, type, payload, plen, out, out_cap) — собирает фрейм
//     для отправки.
//
// Упаковка структур через __attribute__((packed)) обязательна — Ethernet
// плотно упакован без выравнивания.
// ============================================================================

#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ETH_ALEN      6
#define ETH_HDR_LEN   14
#define ETH_MIN_LEN   60     // минимум до FCS
#define ETH_MAX_LEN   1514   // без FCS
#define ETH_MTU       1500

// Известные EtherType.
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV6  0x86DD

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;  // BE на проводе
} eth_header_t;

// Приём фрейма из netif. len включает заголовок, но без FCS.
void ethernet_rx(const void* frame, size_t len);

// Сборка фрейма. Возвращает полную длину или 0 при ошибке.
size_t ethernet_build(const uint8_t dst[ETH_ALEN],
                      uint16_t ethertype,
                      const void* payload, size_t plen,
                      void* out, size_t out_cap);

// big-endian ⇄ host (для 16-бит значений EtherType и т.д.).
static inline uint16_t eth_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
#define eth_hton16(v) eth_bswap16(v)
#define eth_ntoh16(v) eth_bswap16(v)

#endif // ETHERNET_H
