// ============================================================================
// arp.h — Address Resolution Protocol (Этап 26)
// ----------------------------------------------------------------------------
// ARP превращает IPv4-адреса в MAC-адреса. Без него нельзя отправить IP-пакет
// в канал Ethernet — мы просто не знаем, в кого его класть.
//
// RFC 826. Формат ARP over Ethernet (28 байт):
//   htype (2) = 1 для Ethernet
//   ptype (2) = 0x0800 для IPv4
//   hlen  (1) = 6
//   plen  (1) = 4
//   op    (2) = 1 request, 2 reply
//   sha   (6) sender MAC
//   spa   (4) sender IP
//   tha   (6) target MAC (в request — 0)
//   tpa   (4) target IP
//
// На этом этапе делаем:
//   • arp_handle_packet() — отвечаем на запросы, обращённые к нашему IP;
//     запоминаем пару в кэше.
//   • arp_resolve(ip)     — ищем MAC в кэше. Возвращает NULL если не знаем.
//   • arp_send_request(ip) — отправляем who-has broadcast.
//
// Таблица — 16 записей, замена по FIFO (простейший вариант для учебной ОС).
// Timeout по записям пока не делаем.
// ============================================================================

#ifndef ARP_H
#define ARP_H

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

// Обработка входящего ARP-пакета (уже без Ethernet-заголовка).
void arp_rx(const void* payload, size_t len);

// Поиск MAC по IP в кэше. Возвращает указатель на MAC (6 байт) или NULL.
// IP передаётся в host-byte-order.
const uint8_t* arp_resolve(uint32_t ip);

// Отправить ARP Request (who-has). IP в host-byte-order.
bool arp_send_request(uint32_t ip);

// Ручное добавление записи (пригодится для отладки и DHCP на 27 этапе).
void arp_cache_add(uint32_t ip, const uint8_t mac[6]);

// Распечатать содержимое кэша в serial.
void arp_cache_dump(void);

#endif // ARP_H
