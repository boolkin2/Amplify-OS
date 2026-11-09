// ============================================================================
// arp.h — ARP (RFC 826) для AMPLIFY-OS x86_64.
// 1:1 порт kernel/net/arp.h.
// ============================================================================
#ifndef AOS_X86_64_ARP_H
#define AOS_X86_64_ARP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ARP_HTYPE_ETHERNET  1
#define ARP_PTYPE_IPV4      0x0800
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_packet_t;

void           arp_rx(const void* payload, size_t len);
const uint8_t* arp_resolve(uint32_t ip);
bool           arp_send_request(uint32_t ip);
void           arp_cache_add(uint32_t ip, const uint8_t mac[6]);
void           arp_cache_dump(void);
// [48.2] Копия MAC из кэша под запретом прерываний; false — записи нет.
bool           arp_lookup(uint32_t ip, uint8_t out[6]);
void           arp_cache_flush(void);
void           arp_announce(void);

#endif
