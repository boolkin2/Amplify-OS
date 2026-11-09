// ============================================================================
// ipv4.h — минимальный IPv4 (только ICMP в 37.5c-1, UDP/TCP — в 37.5c-2).
// ============================================================================
#ifndef AOS_X86_64_IPV4_H
#define AOS_X86_64_IPV4_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IPV4_VERSION     4
#define IPV4_HDR_MIN_LEN 20

#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_TCP   6
#define IPV4_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} ipv4_header_t;

void     ipv4_rx(const void* payload, size_t len);
bool     ipv4_send(uint32_t dst_ip, uint8_t protocol,
                   const void* payload, size_t plen);
uint16_t ipv4_checksum(const void* data, size_t len);
// [48.2] То же, но с явным адресом отправителя (DHCP: 0.0.0.0).
bool     ipv4_send_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                        const void* payload, size_t plen);
// [48.2] Убедиться из главного потока, что MAC следующего узла для dst_ip
// известен (при необходимости ждёт ответа ARP до 0.8 с). После этого сегменты
// можно слать из критической секции/обработчика без ожиданий.
bool     ipv4_route_ready(uint32_t dst_ip);
// То же без ожидания: false — MAC пока неизвестен (запрос ARP отправлен).
bool     ipv4_route_known(uint32_t dst_ip);

#endif
