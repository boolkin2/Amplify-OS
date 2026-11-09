// ============================================================================
// ipv4.h — IPv4-слой (Этап 26)
// ----------------------------------------------------------------------------
// Ровно то, что нужно для отвечать на пинги и отправлять ICMP:
//   • разобрать заголовок (20 байт + options),
//   • проверить checksum,
//   • маршрутизация по protocol в ICMP-handler (на 27 добавим UDP/TCP),
//   • сборка + отправка исходящего пакета через ARP-резолв.
//
// Фрагментация не поддерживается — отбрасываем пакеты с MF или frag_off!=0.
// Options тоже игнорируем (читаем IHL и скачем через них).
//
// RFC 791. Формат:
//   ver/ihl(1)   type_of_service(1)  total_length(2)
//   id(2)        flags_frag_off(2)
//   ttl(1)       protocol(1)         checksum(2)
//   src(4)       dst(4)              [options...]
//   [payload]
//
// Checksum — RFC 1071: 16-bit one's complement сумма полей заголовка.
// ============================================================================

#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IPV4_VERSION     4
#define IPV4_HDR_MIN_LEN 20

// Protocol numbers (IANA).
#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_TCP   6
#define IPV4_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;         // (ver<<4) | ihl (в 32-битных словах)
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag_off;  // флаги в старших 3 битах, offset в младших 13
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} ipv4_header_t;

// Приём IPv4-пакета (уже без Ethernet-заголовка).
void ipv4_rx(const void* payload, size_t len);

// Отправить IPv4-пакет. Выполняется ARP-резолв destination (либо через
// gateway, если вне сети). Если MAC неизвестен — отправляется ARP Request
// и возвращается false (пакет дропается, логика retry выше).
// src/dst — в host-byte-order.
bool ipv4_send(uint32_t dst_ip, uint8_t protocol,
               const void* payload, size_t plen);

// Checksum RFC 1071. Одинаково подходит для IP header, ICMP, UDP, TCP.
uint16_t ipv4_checksum(const void* data, size_t len);

#endif // IPV4_H
