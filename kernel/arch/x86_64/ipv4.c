// ============================================================================
// ipv4.c — IPv4: приём, отправка, RFC1071 checksum, выбор следующего узла.
// ----------------------------------------------------------------------------
// [48.2] Правило ожидания ARP. ipv4_send зовут из двух миров: из главного
// потока (DNS, DHCP, запрос HTTP) и из обработчика прерывания сетевой карты
// (TCP подтверждает принятый сегмент). Раньше оба пути при пустом кэше ARP
// уходили в `sti; hlt` — то есть обработчик прерывания сам разрешал
// прерывания и засыпал; драйверу virtio-net пришлось ставить защиту от
// повторного входа, а TCP получал сегменты вперемешку. Теперь ждать ответа
// ARP можно только при разрешённых прерываниях (главный поток вне
// критической секции). В обработчике и под net_irq_save запрос ARP уходит, а
// пакет отбрасывается — TCP его повторит. Чтобы до этого не доходило,
// tcp_connect заранее зовёт ipv4_route_ready() из главного потока.
// Сборка кадра — в статических буферах под запретом прерываний (раньше
// 3 КБ на стеке, в том числе на стеке прерванного потока; стек ядра 16 КБ).
// ============================================================================
#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "netif.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "timer.h"
#include "io.h"
#include "serial.h"

uint16_t ipv4_checksum(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2; len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static uint32_t ip_from_bytes(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] <<  8) | (uint32_t)b[3];
}
static void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >>  8);
    out[3] = (uint8_t)(ip);
}

#define ARP_WAIT_MS   800
#define ARP_RETRY_MS  200

static bool is_broadcast(const net_interface_t* iface, uint32_t ip) {
    if (ip == 0xFFFFFFFFu) return true;
    // Широковещательный адрес своей подсети (192.168.1.255 при /24).
    if (iface->ip4_addr && iface->ip4_netmask && iface->ip4_netmask != 0xFFFFFFFFu &&
        (ip & iface->ip4_netmask) == (iface->ip4_addr & iface->ip4_netmask) &&
        (ip | iface->ip4_netmask) == 0xFFFFFFFFu) return true;
    return false;
}

// MAC следующего узла для dst_ip. may_wait — можно ли ждать ответа ARP.
static bool next_hop_mac(uint32_t dst_ip, uint8_t mac[6], bool may_wait) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return false;

    if (is_broadcast(iface, dst_ip)) {
        for (int i = 0; i < 6; i++) mac[i] = 0xFF;
        return true;
    }
    if (iface->ip4_addr == 0) { net_set_error(NET_ERR_NO_ADDR); return false; }

    uint32_t my_net  = iface->ip4_addr & iface->ip4_netmask;
    uint32_t dst_net = dst_ip          & iface->ip4_netmask;
    uint32_t next_hop = (my_net == dst_net) ? dst_ip : iface->ip4_gateway;
    if (next_hop == 0) { net_set_error(NET_ERR_UNREACHABLE); return false; }

    if (arp_lookup(next_hop, mac)) return true;

    arp_send_request(next_hop);
    if (!may_wait) return false;

    uint64_t t0 = timer_get_uptime_ms(), last = t0;
    while (timer_get_uptime_ms() - t0 < ARP_WAIT_MS) {
        if (net_cancelled()) return false;
        net_idle();
        if (arp_lookup(next_hop, mac)) return true;
        if (timer_get_uptime_ms() - last >= ARP_RETRY_MS) {
            arp_send_request(next_hop);
            last = timer_get_uptime_ms();
        }
    }
    serial_puts("[ipv4] arp resolve timeout for "); netif_dump_ip4(next_hop); serial_puts("\n");
    net_set_error(NET_ERR_UNREACHABLE);
    return false;
}

bool ipv4_route_ready(uint32_t dst_ip) {
    uint8_t mac[6];
    return next_hop_mac(dst_ip, mac, net_irqs_enabled());
}

// Известен ли MAC следующего узла прямо сейчас. Не ждёт: если записи нет,
// уходит запрос ARP (ответ, если придёт, будет в кэше к следующему разу).
bool ipv4_route_known(uint32_t dst_ip) {
    uint8_t mac[6];
    net_error_t keep = net_last_error();
    bool ok = next_hop_mac(dst_ip, mac, false);
    net_set_error(keep);                       // проба не должна менять причину отказа
    return ok;
}

void ipv4_rx(const void* payload, size_t len) {
    if (len < IPV4_HDR_MIN_LEN) return;
    const ipv4_header_t* h = (const ipv4_header_t*)payload;

    uint8_t ver = (h->ver_ihl >> 4) & 0xF;
    uint8_t ihl = h->ver_ihl & 0xF;
    if (ver != IPV4_VERSION || ihl < 5) return;

    size_t hdr_len = (size_t)ihl * 4;
    if (hdr_len > len) return;

    uint16_t total = ((uint16_t)((const uint8_t*)&h->total_length)[0] << 8) |
                     (uint16_t)((const uint8_t*)&h->total_length)[1];
    if (total > len || total < hdr_len) return;

    uint16_t got = ipv4_checksum(h, hdr_len);
    if (got != 0) { serial_puts("[ipv4] bad checksum\n"); return; }

    uint16_t ff = ((uint16_t)((const uint8_t*)&h->flags_frag_off)[0] << 8) |
                  (uint16_t)((const uint8_t*)&h->flags_frag_off)[1];
    if ((ff & 0x2000) || (ff & 0x1FFF)) {
        serial_puts("[ipv4] fragmented — drop\n");
        return;
    }

    uint32_t dst = ip_from_bytes(h->dst);
    uint32_t src = ip_from_bytes(h->src);
    net_interface_t* iface = netif_get_default();
    if (!iface) return;
    // [48.2] Пока своего адреса нет, принимаем и пакеты, адресованные «не
    // нам»: настоящий DHCP-сервер (dnsmasq, ISC) шлёт OFFER/ACK не
    // широковещательно, а на выдаваемый адрес — по нашему MAC. Раньше такой
    // ответ здесь отбрасывался (slirp отвечает широковещательно, и в QEMU
    // этого было не видно). Без адреса слушает только DHCP (UDP), так что
    // лишнего это не впускает.
    bool for_us = (dst == iface->ip4_addr) || is_broadcast(iface, dst) ||
                  (iface->ip4_addr == 0 && h->protocol == IPV4_PROTO_UDP);
    if (!for_us) return;

    const uint8_t* inner = (const uint8_t*)payload + hdr_len;
    size_t inner_len = total - hdr_len;

    switch (h->protocol) {
        case IPV4_PROTO_ICMP: icmp_rx(src, inner, inner_len); break;
        case IPV4_PROTO_UDP:  udp_rx(src, dst, inner, inner_len); break;
        case IPV4_PROTO_TCP:  tcp_rx(src, dst, inner, inner_len); break;
        default: break;
    }
}

// src_ip задаётся явно: DHCP шлёт DISCOVER/REQUEST от 0.0.0.0, даже когда у
// интерфейса уже есть (запасной статический) адрес. Раньше DHCP для этого на
// время отправки обнулял адрес интерфейса — и ACK TCP, ушедший из прерывания
// в это окно, получал адрес отправителя 0.0.0.0.
static bool send_impl(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                      const void* payload, size_t plen) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return false;
    if (plen > ETH_MTU - IPV4_HDR_MIN_LEN) return false;

    uint8_t next_mac[6];
    if (!next_hop_mac(dst_ip, next_mac, net_irqs_enabled())) return false;

    static uint8_t  frame[ETH_MAX_LEN + 4];
    static uint8_t  ipbuf[IPV4_HDR_MIN_LEN + ETH_MTU];
    static uint16_t id_counter = 1;

    uint64_t fl = net_irq_save();

    ipv4_header_t* h = (ipv4_header_t*)ipbuf;
    h->ver_ihl = (uint8_t)((IPV4_VERSION << 4) | 5);
    h->tos = 0;
    uint16_t total = (uint16_t)(IPV4_HDR_MIN_LEN + plen);
    ((uint8_t*)&h->total_length)[0] = (uint8_t)(total >> 8);
    ((uint8_t*)&h->total_length)[1] = (uint8_t)(total);
    id_counter++;
    ((uint8_t*)&h->id)[0] = (uint8_t)(id_counter >> 8);
    ((uint8_t*)&h->id)[1] = (uint8_t)(id_counter);
    ((uint8_t*)&h->flags_frag_off)[0] = 0x40;          // DF
    ((uint8_t*)&h->flags_frag_off)[1] = 0x00;
    h->ttl = 64;
    h->protocol = protocol;
    h->checksum = 0;
    ip_to_bytes(src_ip,           h->src);
    ip_to_bytes(dst_ip,           h->dst);

    uint16_t csum = ipv4_checksum(h, IPV4_HDR_MIN_LEN);
    ((uint8_t*)&h->checksum)[0] = (uint8_t)(csum >> 8);
    ((uint8_t*)&h->checksum)[1] = (uint8_t)(csum);

    if (payload && plen > 0) {
        uint8_t* body = ipbuf + IPV4_HDR_MIN_LEN;
        const uint8_t* p = (const uint8_t*)payload;
        for (size_t i = 0; i < plen; i++) body[i] = p[i];
    }

    size_t fn = ethernet_build(next_mac, ETH_TYPE_IPV4,
                               ipbuf, IPV4_HDR_MIN_LEN + plen,
                               frame, sizeof(frame));
    bool ok = fn != 0 && netif_send(frame, fn) > 0;
    net_irq_restore(fl);
    return ok;
}

bool ipv4_send(uint32_t dst_ip, uint8_t protocol,
               const void* payload, size_t plen) {
    net_interface_t* iface = netif_get_default();
    if (!iface) return false;
    return send_impl(iface->ip4_addr, dst_ip, protocol, payload, plen);
}

bool ipv4_send_from(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                    const void* payload, size_t plen) {
    return send_impl(src_ip, dst_ip, protocol, payload, plen);
}
