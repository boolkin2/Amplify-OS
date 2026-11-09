// ============================================================================
// icmp.h — ICMP echo (Этап 26)
// ----------------------------------------------------------------------------
// На этом этапе поддерживаем только Echo Request/Reply — чтобы ping из хоста
// (через QEMU user-networking) возвращал pong.
//
// RFC 792. Формат ICMP echo:
//   type(1) code(1) checksum(2) id(2) seq(2) [payload...]
//   type=8 Echo Request, type=0 Echo Reply.
//
// Приём: icmp_rx() вызывается из ipv4_rx() с уже вырезанным IPv4-заголовком.
// Если это Echo Request к нам — строим Reply с тем же payload и обратной
// перестановкой type, пересчитываем checksum, и отдаём в ipv4_send() обратно
// источнику.
// ============================================================================

#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;    // big-endian на проводе
    uint16_t id;
    uint16_t seq;
    // далее payload
} icmp_echo_t;

// Вызывается из ipv4_rx. src_ip — в host-byte-order.
void icmp_rx(uint32_t src_ip, const void* payload, size_t len);

// --- Исходящий ping (Этап 34) ----------------------------------------------
// Отправить Echo Request к dst_ip с указанным id/seq и payload.
// payload может быть NULL/0 — тогда отправляется заголовок ICMP без данных.
// Возвращает true, если пакет корректно прошёл в ipv4_send.
bool icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq,
                    const void* payload, size_t plen);

// Установить коллбек на приём Echo Reply. Вызывается ровно один раз
// для каждого входящего ответа. Если NULL — отписаться.
typedef void (*icmp_reply_cb_t)(uint32_t src_ip, uint16_t id, uint16_t seq);
void icmp_set_reply_cb(icmp_reply_cb_t cb);

#endif // ICMP_H
