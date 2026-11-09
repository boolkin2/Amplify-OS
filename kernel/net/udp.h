// ============================================================================
// udp.h — User Datagram Protocol (Этап 27)
// ----------------------------------------------------------------------------
// RFC 768, простейший транспортный протокол: ничего не гарантирует, просто
// доставляет datagram'у. Нам нужен для DHCP (порт 67/68) и для
// пользовательских приложений.
//
// Заголовок 8 байт:
//   src_port(2)  dst_port(2)
//   length(2)    checksum(2)   — BE на проводе
//
// Checksum считается по pseudo-header (src_ip, dst_ip, zero, proto, udp_len)
// + заголовок UDP + данные. Для IPv4 опциональна (можно оставить 0), но
// мы всё-таки считаем — так корректнее.
//
// API:
//   • udp_bind(port, handler) — регистрирует обработчик для входящих пакетов
//     на этот порт. Возвращает false при конфликте.
//   • udp_send(dst_ip, dst_port, src_port, payload, len) — отправка.
//   • udp_rx(...) — вход из ipv4_rx.
// ============================================================================

#ifndef UDP_H
#define UDP_H

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

// Коллбэк на приём. src_ip/ports — host-byte-order.
typedef void (*udp_rx_handler_t)(uint32_t src_ip, uint16_t src_port,
                                 uint16_t dst_port,
                                 const void* data, size_t len);

bool udp_bind(uint16_t port, udp_rx_handler_t handler);
void udp_unbind(uint16_t port);

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void* payload, size_t plen);

// Вход из ipv4_rx (payload — всё после IPv4-заголовка).
void udp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len);

#endif // UDP_H
