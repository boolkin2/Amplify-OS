// ============================================================================
// udp.h — UDP (RFC 768) для AMPLIFY-OS x86_64.  Порт kernel/net/udp.h.
// ============================================================================
#ifndef AOS_X86_64_UDP_H
#define AOS_X86_64_UDP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define UDP_HDR_LEN 8

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

typedef void (*udp_rx_handler_t)(uint32_t src_ip, uint16_t src_port,
                                 uint16_t dst_port,
                                 const void* data, size_t len);

bool udp_bind(uint16_t port, udp_rx_handler_t handler);
void udp_unbind(uint16_t port);

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void* payload, size_t plen);

bool udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const void* payload, size_t plen);
void udp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len);

#endif
