// ============================================================================
// icmp.c — ICMP echo reply
// ============================================================================

#include "icmp.h"
#include "ipv4.h"
#include "netif.h"
#include "../debug_serial.h"

#include <stdint.h>
#include <stddef.h>

// Собственный буфер на ответ. Для учебной ОС хватит одного — мы отвечаем
// синхронно из IRQ-контекста и сразу отправляем.
static uint8_t g_reply_buf[1500];
static uint8_t g_send_buf[1500];

static icmp_reply_cb_t g_reply_cb = 0;

void icmp_set_reply_cb(icmp_reply_cb_t cb) {
    g_reply_cb = cb;
}

// host-to-network 16-bit (для id/seq, которые на проводе BE)
static inline uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

bool icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void* payload, size_t plen) {
    if (plen + sizeof(icmp_echo_t) > sizeof(g_send_buf)) return false;
    icmp_echo_t* p = (icmp_echo_t*)g_send_buf;
    p->type = ICMP_TYPE_ECHO_REQUEST;
    p->code = 0;
    p->checksum = 0;
    p->id  = be16(id);
    p->seq = be16(seq);
    if (payload && plen) {
        const uint8_t* s = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++)
            ((uint8_t*)g_send_buf)[sizeof(icmp_echo_t) + i] = s[i];
    }
    size_t total = sizeof(icmp_echo_t) + plen;
    uint16_t csum = ipv4_checksum(g_send_buf, total);
    ((uint8_t*)&p->checksum)[0] = (uint8_t)(csum >> 8);
    ((uint8_t*)&p->checksum)[1] = (uint8_t)(csum);
    return ipv4_send(dst_ip, IPV4_PROTO_ICMP, g_send_buf, total);
}

void icmp_rx(uint32_t src_ip, const void* payload, size_t len) {
    if (len < sizeof(icmp_echo_t)) return;
    const icmp_echo_t* req = (const icmp_echo_t*)payload;

    dbg_puts("[icmp] rx type=0x");
    dbg_hex(req->type);
    dbg_puts(" from ");
    netif_dump_ip4(src_ip);
    dbg_puts("\n");

    if (req->type == ICMP_TYPE_ECHO_REPLY && req->code == 0) {
        // Echo Reply — это наш ответ на исходящий ping.
        if (g_reply_cb) {
            g_reply_cb(src_ip, be16(req->id), be16(req->seq));
        }
        return;
    }

    if (req->type != ICMP_TYPE_ECHO_REQUEST || req->code != 0) {
        return;
    }
    if (len > sizeof(g_reply_buf)) {
        dbg_puts("[icmp] echo too big — drop\n");
        return;
    }

    // Строим Reply: копируем весь запрос, правим type → Echo Reply,
    // обнуляем checksum, считаем заново.
    for (size_t i = 0; i < len; i++) g_reply_buf[i] = ((const uint8_t*)payload)[i];
    icmp_echo_t* rep = (icmp_echo_t*)g_reply_buf;
    rep->type = ICMP_TYPE_ECHO_REPLY;
    rep->code = 0;
    rep->checksum = 0;

    uint16_t csum = ipv4_checksum(g_reply_buf, len);
    ((uint8_t*)&rep->checksum)[0] = (uint8_t)(csum >> 8);
    ((uint8_t*)&rep->checksum)[1] = (uint8_t)(csum);

    if (ipv4_send(src_ip, IPV4_PROTO_ICMP, g_reply_buf, len)) {
        dbg_puts("[icmp] echo reply sent to ");
        netif_dump_ip4(src_ip);
        dbg_puts("\n");
    } else {
        dbg_puts("[icmp] echo reply failed (ARP pending?)\n");
    }
}
