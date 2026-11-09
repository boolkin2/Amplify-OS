#ifndef AOS_X86_64_ETHERNET_H
#define AOS_X86_64_ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ETH_ALEN      6
#define ETH_HDR_LEN   14
#define ETH_MIN_LEN   60
#define ETH_MAX_LEN   1514
#define ETH_MTU       1500

#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV6  0x86DD

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} eth_header_t;

void   ethernet_rx(const void* frame, size_t len);
size_t ethernet_build(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
                      const void* payload, size_t plen,
                      void* out, size_t out_cap);

static inline uint16_t eth_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
#define eth_hton16(v) eth_bswap16(v)
#define eth_ntoh16(v) eth_bswap16(v)

#endif
