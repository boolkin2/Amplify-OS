// ============================================================================
// netif.h — абстракция сетевого интерфейса (Этап 25)
// ----------------------------------------------------------------------------
// Задача: отвязать верхние слои (Ethernet, ARP, IP на 26-27 этапах) от
// конкретного железа. Драйвер (rtl8139.c) регистрирует объект net_interface_t
// с именем "eth0", MAC'ом и коллбэком send(). Высшие слои могут вызывать
// netif_send() не зная, какая карта стоит за этим.
//
// Для приёма: драйвер по IRQ вынимает пакет и передаёт его в netif_receive().
// Дальше — диспетчеризация на Ethernet-слой.
//
// На этом этапе полноценного роутинга нет — считается, что интерфейс один.
// Этого достаточно, чтобы видеть ARP/IP трафик от QEMU-slirp.
// ============================================================================

#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NETIF_NAME_LEN   8
#define NETIF_MAC_LEN    6
#define NETIF_MTU        1500

typedef size_t (*netif_send_fn)(const void* data, size_t len);

typedef struct net_interface {
    char     name[NETIF_NAME_LEN];
    uint8_t  mac[NETIF_MAC_LEN];
    netif_send_fn send;

    // IPv4 — пока заглушка (заполнится на 26 этапе через DHCP или static).
    uint32_t ip4_addr;
    uint32_t ip4_netmask;
    uint32_t ip4_gateway;

    // Счётчики.
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t tx_packets;
    uint32_t tx_bytes;

    bool     up;
} net_interface_t;

// --- API ---

// Регистрирует интерфейс. Возвращает false, если регистр занят (на этом
// этапе поддерживается ровно один интерфейс).
bool netif_register(net_interface_t* iface);

// Возвращает текущий активный интерфейс (или NULL).
net_interface_t* netif_get_default(void);

// Отправить пакет через default-интерфейс.
// Принимает полный Ethernet-фрейм, уже с dst/src/type.
size_t netif_send(const void* data, size_t len);

// Вызывается драйвером при получении нового пакета (из IRQ handler'a).
// Передаёт пакет в Ethernet-слой. Данные *копировать не нужно* — стек сам
// решит, что с ним делать до следующего пакета.
void netif_receive(const void* data, size_t len);

// Общая инициализация подсистемы (вызывается из kernel.c).
// Поднимает драйверы сетевых карт.
bool net_init(void);

// Печатает MAC в формате "aa:bb:cc:dd:ee:ff" в serial log.
void netif_dump_mac(const uint8_t* mac);

// Задать статическую IP-конфигурацию интерфейсу.
// Адреса передаются в host-byte-order (например, через ipv4_addr(10,0,2,15)).
void netif_set_ip4(net_interface_t* iface,
                   uint32_t addr, uint32_t netmask, uint32_t gateway);

// Печатает IPv4 в формате "a.b.c.d".
void netif_dump_ip4(uint32_t addr);

// Helper: собрать uint32_t host-byte-order из октетов a.b.c.d
static inline uint32_t ipv4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8)  | (uint32_t)d;
}

// Byteswap 32-bit.
static inline uint32_t net_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define net_hton32(v) net_bswap32(v)
#define net_ntoh32(v) net_bswap32(v)

#endif // NETIF_H
