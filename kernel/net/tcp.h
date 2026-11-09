// ============================================================================
// tcp.h — минимальный TCP-клиент (Этап 27)
// ----------------------------------------------------------------------------
// Полноценный TCP — отдельный большой проект. Здесь — учебный client-only
// вариант, достаточный, чтобы:
//   • открыть соединение (SYN → SYN-ACK → ACK);
//   • отправить payload (PSH|ACK);
//   • принять payload и подтвердить (ACK);
//   • закрыть (FIN → ACK, затем получить FIN, ответить ACK).
//
// Жёсткие ограничения:
//   • ровно один активный сокет;
//   • нет retransmission — если пакет потеряется, пользователь увидит,
//     что tcp_send вернёт true, но соединение замрёт;
//   • нет window management — окно всегда 1460;
//   • нет OOO: пакет с seq != rcv_nxt отбрасываем;
//   • нет server-режима (listen/accept) — только connect.
//
// RFC 793, заголовок 20 байт + options (игнорируем).
// Checksum считается как у UDP: pseudo-header + tcp header + data.
//
// API:
//   • tcp_connect(ip, port)      — SYN, блокирующий спинлок до ESTABLISHED.
//   • tcp_send(buf, len)         — PSH|ACK.
//   • tcp_recv(buf, cap, &got)   — неблокирующе, сколько успели накопить.
//   • tcp_close()                — FIN.
//
// Благодаря тому, что RX идёт из IRQ-handler'а сетевого драйвера, state
// меняется асинхронно — tcp_connect спин-ждёт поднятие соединения в цикле
// с `hlt`.
// ============================================================================

#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Флаги TCP (в порядке следования по RFC).
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
} tcp_state_t;

// Открыть соединение. Блокирует до ESTABLISHED или таймаута.
// dst_ip — host-byte-order.
bool tcp_connect(uint32_t dst_ip, uint16_t dst_port);

// Отправить данные. Возвращает число байт, которые поместились в один сегмент.
size_t tcp_send(const void* data, size_t len);

// Забрать ранее накопленные в буфере данные. *got заполняется.
bool tcp_recv(void* buf, size_t cap, size_t* got);

// Закрыть (FIN).
void tcp_close(void);

// Текущее состояние.
tcp_state_t tcp_current_state(void);

// Вход из ipv4_rx.
void tcp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len);

#endif // TCP_H
