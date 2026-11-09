// ============================================================================
// dhcp.h — DHCP-клиент (Этап 27)
// ----------------------------------------------------------------------------
// RFC 2131. Состояния клиента:
//   INIT → DISCOVER (broadcast) → OFFER (recv) → REQUEST (broadcast) →
//   ACK (recv) → BOUND.
//
// При успехе заполняет netif: ip4_addr, ip4_netmask, ip4_gateway.
//
// Упрощения:
//   • Игнорируем lease time, renew, rebinding — получили и забыли.
//   • Один запрос за раз, без очереди.
//   • Секвенс транзакций XID фиксированный (0x11223344); этого хватит для
//     учебной ОС, где DHCP делается строго однократно.
//   • Опции: MessageType(53), Subnet(1), Router(3), DNS(6), LeaseTime(51),
//     ServerId(54), End(255).
//
// API:
//   • dhcp_start() — запускает процесс. Возвращает true, если отправили
//     DISCOVER; само получение адреса асинхронное (через udp_rx handler).
//   • dhcp_is_bound() — можно ли уже отправлять пакеты.
// ============================================================================

#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>
#include <stdbool.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

// Запустить процесс получения IP.
bool dhcp_start(void);

// Получен ли адрес (ACK пришёл).
bool dhcp_is_bound(void);

// Распечатать текущее состояние.
void dhcp_dump_state(void);

#endif // DHCP_H
