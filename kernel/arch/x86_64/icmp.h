#ifndef AOS_X86_64_ICMP_H
#define AOS_X86_64_ICMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_echo_t;

void icmp_rx(uint32_t src_ip, const void* payload, size_t len);
bool icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void* payload, size_t plen);

typedef void (*icmp_reply_cb_t)(uint32_t src_ip, uint16_t id, uint16_t seq);
void icmp_set_reply_cb(icmp_reply_cb_t cb);

#endif
