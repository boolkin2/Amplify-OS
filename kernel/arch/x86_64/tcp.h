// ============================================================================
// tcp.h — минимальный TCP-клиент (RFC 793) для AMPLIFY-OS x86_64.
// Один сокет, без retransmission, только client.
// ============================================================================
#ifndef AOS_X86_64_TCP_H
#define AOS_X86_64_TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,            // [47.5] server passive listen
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RCVD,          // [47.5] server got SYN, sent SYN-ACK
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
} tcp_state_t;

bool        tcp_connect(uint32_t dst_ip, uint16_t dst_port);
size_t      tcp_send(const void* data, size_t len);
bool        tcp_recv(void* buf, size_t cap, size_t* got);
void        tcp_close(void);
tcp_state_t tcp_current_state(void);
// [48.2]
void        tcp_abort(void);          // RST серверу и немедленное закрытие
size_t      tcp_rx_available(void);   // байт в буфере приёма
bool        tcp_peer_closed(void);    // данных больше не будет (FIN/RST, буфер пуст)
bool        tcp_was_reset(void);      // соединение оборвано RST после установления
// ICMP «узел недоступен» на наш SYN (зовёт icmp.c из обработчика прерывания).
void        tcp_icmp_unreachable(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port);

// [этап 47.5] Server-side passive open.
// tcp_listen(port): открывает passive listen на port'е (single-connection).
//   Возвращает true если success.  Дальше нужно tcp_accept() для приёма
//   входящего SYN.  Текущая реализация поддерживает один listener одновременно
//   (и один accept'нутый client после) — multi-accept в будущем.
bool        tcp_listen(uint16_t port);

// tcp_accept(timeout_ms): блокирует до получения SYN, отвечает SYN-ACK,
//   ждёт ACK на SYN-ACK, переходит в ESTABLISHED.  Возвращает peer info.
//   В peer_ip/peer_port пишется адрес client'а.  timeout_ms=0 = ждать бесконечно.
//   Возвращает true в случае успешного accept'а.
bool        tcp_accept(uint32_t* peer_ip, uint16_t* peer_port, uint32_t timeout_ms);

void tcp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len);

#endif
