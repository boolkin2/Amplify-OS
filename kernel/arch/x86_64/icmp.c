// ============================================================================
// icmp.c — Echo Request → Reply, плюс отправка ping наружу.
// ============================================================================
#include "icmp.h"
#include "ipv4.h"
#include "netif.h"
#include "tcp.h"
#include "serial.h"

static uint8_t g_reply_buf[1500];
static uint8_t g_send_buf[1500];

static icmp_reply_cb_t g_reply_cb = 0;

void icmp_set_reply_cb(icmp_reply_cb_t cb) { g_reply_cb = cb; }

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

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
    ((uint8_t*)&p->checksum)[1] = (uint8_t)csum;
    return ipv4_send(dst_ip, IPV4_PROTO_ICMP, g_send_buf, total);
}

void icmp_rx(uint32_t src_ip, const void* payload, size_t len) {
    if (len < sizeof(icmp_echo_t)) return;
    const icmp_echo_t* req = (const icmp_echo_t*)payload;

    if (req->type == ICMP_TYPE_ECHO_REPLY && req->code == 0) {
        serial_puts("[icmp] echo REPLY from "); netif_dump_ip4(src_ip);
        serial_puts(" seq="); serial_print_dec(be16(req->seq)); serial_puts("\n");
        if (g_reply_cb) g_reply_cb(src_ip, be16(req->id), be16(req->seq));
        return;
    }
    // [48.2] «Узел недоступен» (тип 3) и «время жизни истекло» (тип 11): в
    // теле — заголовок IP нашего пакета и первые 8 байт его содержимого (у TCP
    // это порты). Раньше такие ответы выбрасывались, и соединение с
    // недостижимым адресом ждало все 6 секунд повторов SYN; теперь TCP узнаёт
    // об отказе сразу, а браузер показывает причину.
    if (req->type == 3 || req->type == 11) {
        const uint8_t* p = (const uint8_t*)payload;
        if (len < 8 + 20 + 8) return;
        const uint8_t* ip = p + 8;
        size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
        if ((ip[0] >> 4) != 4 || ihl < 20 || len < 8 + ihl + 8) return;
        uint32_t orig_dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
                            ((uint32_t)ip[18] << 8) | ip[19];
        const uint8_t* l4 = ip + ihl;
        serial_puts(req->type == 3 ? "[icmp] destination unreachable (code "
                                   : "[icmp] time exceeded (code ");
        serial_print_dec(req->code); serial_puts(") for "); netif_dump_ip4(orig_dst);
        serial_puts(" from "); netif_dump_ip4(src_ip); serial_puts("\n");
        if (ip[9] == IPV4_PROTO_TCP)
            tcp_icmp_unreachable(orig_dst, (uint16_t)((l4[0] << 8) | l4[1]),
                                 (uint16_t)((l4[2] << 8) | l4[3]));
        return;
    }
    if (req->type != ICMP_TYPE_ECHO_REQUEST || req->code != 0) return;
    if (len > sizeof(g_reply_buf)) return;

    for (size_t i = 0; i < len; i++) g_reply_buf[i] = ((const uint8_t*)payload)[i];
    icmp_echo_t* rep = (icmp_echo_t*)g_reply_buf;
    rep->type = ICMP_TYPE_ECHO_REPLY;
    rep->code = 0;
    rep->checksum = 0;
    uint16_t csum = ipv4_checksum(g_reply_buf, len);
    ((uint8_t*)&rep->checksum)[0] = (uint8_t)(csum >> 8);
    ((uint8_t*)&rep->checksum)[1] = (uint8_t)csum;

    if (ipv4_send(src_ip, IPV4_PROTO_ICMP, g_reply_buf, len)) {
        serial_puts("[icmp] echo reply sent to "); netif_dump_ip4(src_ip); serial_puts("\n");
    }
}
