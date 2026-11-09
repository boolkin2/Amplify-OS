// ============================================================================
// tcp.c — TCP-клиент (один сокет) + passive listen для серверного режима.
// ----------------------------------------------------------------------------
// Этап 48.1: кольцевой буфер приёма 64 КБ, окно по свободному месту, MSS,
// случайные порт и ISN, повтор SYN и данных.
//
// Подэтап 48.2 — что исправлено:
//   1) Гонки с обработчиком прерывания. Сегменты разбирает tcp_rx прямо в
//      прерывании сетевой карты (virtio-net — в любой момент), и оттуда же
//      уходят подтверждения. send_segment собирает сегмент в общем статическом
//      буфере: ACK из прерывания, вклинившийся в сборку сегмента с данными в
//      главном потоке, портил запрос. Теперь сборка и отправка — одна
//      критическая секция; под ней же сброс сокета в tcp_connect/tcp_listen,
//      закрытие и чтение кольца. Ждать ARP внутри секции нельзя (ipv4.c),
//      поэтому маршрут проверяется заранее: ipv4_route_ready в tcp_connect.
//   2) FIN. Раньше любой сегмент с FIN увеличивал rcv_nxt — повтор FIN от
//      сервера сдвигал счётчик дважды, а FIN, пришедший раньше потерянных
//      данных, обрывал страницу. FIN принимается только по порядку.
//   3) RST. Принимается, только если попадает в окно (в SYN_SENT — если
//      подтверждает наш SYN): посторонний сегмент соединение не рвёт. На
//      сегменты «ничьих» соединений (остатки прошлой страницы, на которую мы
//      уже не смотрим) отвечаем RST — сервер не повторяет их минутами.
//   4) Закрытие. tcp_close ждёт подтверждения своего FIN (с одним повтором);
//      закрытие с непрочитанными данными или посреди потока — RST (tcp_abort).
//   5) Причина отказа connect различается: нет маршрута / отказ (RST) /
//      молчание — её показывает страница ошибки браузера.
// Сокет по-прежнему один на систему: соединения — строго по очереди.
// Сегменты не по порядку не накапливаются (повторный ACK просит переслать).
// ============================================================================
#include "tcp.h"
#include "ipv4.h"
#include "netif.h"
#include "timer.h"
#include "serial.h"

#define TCP_HDR_LEN     20
#define TCP_MSS         1460
#define TCP_RX_BUF_SIZE (64 * 1024)

#define TCP_CONNECT_TIMEOUT_MS  6000
#define TCP_SYN_RETRY_MS        1200
#define TCP_SYN_RETRIES         3
#define TCP_SEND_ACK_WAIT_MS    600
#define TCP_SEND_RETRIES        3
#define TCP_FIN_WAIT_MS         250     // сколько tcp_close ждёт завершения
#define TCP_FIN_RETRY_MS        100

typedef struct {
    volatile tcp_state_t state;

    uint32_t  remote_ip;
    uint16_t  remote_port;
    uint16_t  local_port;

    volatile uint32_t snd_nxt;
    volatile uint32_t snd_una;
    volatile uint32_t rcv_nxt;

    volatile bool refused;               // RST в ответ на SYN
    volatile bool unreachable;           // ICMP «узел недоступен» в ответ на SYN
    volatile bool reset;                 // RST после установления

    uint8_t   rx_buf[TCP_RX_BUF_SIZE];   // кольцевой буфер
    volatile size_t rx_head;             // куда пишет IRQ
    volatile size_t rx_tail;             // откуда читает потребитель
    volatile size_t rx_count;            // байт в буфере
} tcp_socket_t;

static tcp_socket_t g_sock;

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}
static inline void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static inline uint64_t tcp_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t now_ms(void) { return timer_get_uptime_ms(); }

// Свободное место в RX-буфере → анонсируемое окно.
static uint16_t rx_window(void) {
    size_t room = TCP_RX_BUF_SIZE - g_sock.rx_count;
    if (room > 65535) room = 65535;
    return (uint16_t)room;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const void* tcp_start, size_t tcp_len) {
    uint8_t pseudo[12];
    put_be32(pseudo + 0, src_ip);
    put_be32(pseudo + 4, dst_ip);
    pseudo[8]  = 0;
    pseudo[9]  = IPV4_PROTO_TCP;
    pseudo[10] = (uint8_t)(tcp_len >> 8);
    pseudo[11] = (uint8_t)tcp_len;

    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += ((uint32_t)pseudo[i] << 8) | pseudo[i+1];
    const uint8_t* p = (const uint8_t*)tcp_start;
    size_t n = tcp_len;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

// Собрать и отправить сегмент с явными полями. Вся функция — критическая
// секция: общий буфер seg и ipv4_send внутри неё не прерываются обработчиком.
static bool emit(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                 const void* data, size_t dlen) {
    if (dlen > TCP_MSS) dlen = TCP_MSS;
    bool   with_mss = (flags & TCP_SYN) != 0;      // SYN несёт опцию MSS (4 байта)
    size_t hdr_len  = TCP_HDR_LEN + (with_mss ? 4u : 0u);

    static uint8_t seg[TCP_HDR_LEN + 4 + TCP_MSS];
    uint64_t fl = net_irq_save();

    put_be16(seg + 0,  src_port);
    put_be16(seg + 2,  dst_port);
    put_be32(seg + 4,  seq);
    put_be32(seg + 8,  ack);
    seg[12] = (uint8_t)((hdr_len / 4) << 4);
    seg[13] = flags;
    put_be16(seg + 14, window);
    put_be16(seg + 16, 0);
    put_be16(seg + 18, 0);
    if (with_mss) {
        seg[20] = 2; seg[21] = 4;                    // kind=MSS, len=4
        put_be16(seg + 22, TCP_MSS);
    }
    if (data && dlen > 0) {
        const uint8_t* s = (const uint8_t*)data;
        for (size_t i = 0; i < dlen; i++) seg[hdr_len + i] = s[i];
    }

    net_interface_t* iface = netif_get_default();
    uint32_t my_ip = iface ? iface->ip4_addr : 0;
    size_t   tlen  = hdr_len + dlen;
    put_be16(seg + 16, tcp_checksum(my_ip, dst_ip, seg, tlen));

    bool ok = ipv4_send(dst_ip, IPV4_PROTO_TCP, seg, tlen);
    net_irq_restore(fl);
    return ok;
}

// Сегмент текущего соединения: seq берётся из snd_nxt и продвигается.
static bool send_segment(uint8_t flags, const void* data, size_t dlen) {
    if (dlen > TCP_MSS) dlen = TCP_MSS;
    uint64_t fl = net_irq_save();
    uint32_t seq = g_sock.snd_nxt;
    uint32_t adv = (uint32_t)dlen;
    if (flags & TCP_SYN) adv++;
    if (flags & TCP_FIN) adv++;
    g_sock.snd_nxt = seq + adv;
    bool ok = emit(g_sock.remote_ip, g_sock.local_port, g_sock.remote_port,
                   seq, g_sock.rcv_nxt, flags, rx_window(), data, dlen);
    net_irq_restore(fl);
    return ok;
}

// Повторная отправка уже отправленного сегмента: snd_nxt не трогаем.
static void resend_segment(uint32_t seq, uint8_t flags, const void* data, size_t dlen) {
    uint64_t fl = net_irq_save();
    emit(g_sock.remote_ip, g_sock.local_port, g_sock.remote_port,
         seq, g_sock.rcv_nxt, flags, rx_window(), data, dlen);
    net_irq_restore(fl);
}

// Ответ RST на сегмент, который не относится ни к какому соединению
// (RFC 793: seq = ack пришедшего сегмента, либо ACK на всё принятое).
static void reply_rst(uint32_t peer_ip, uint16_t peer_port, uint16_t my_port,
                      uint32_t seq, uint32_t ack, uint8_t flags, size_t data_len) {
    if (flags & TCP_RST) return;                   // на RST не отвечают
    if (flags & TCP_ACK) {
        emit(peer_ip, my_port, peer_port, ack, 0, TCP_RST, 0, 0, 0);
    } else {
        uint32_t a = seq + (uint32_t)data_len;
        if (flags & TCP_SYN) a++;
        if (flags & TCP_FIN) a++;
        emit(peer_ip, my_port, peer_port, 0, a, TCP_RST | TCP_ACK, 0, 0, 0);
    }
}

tcp_state_t tcp_current_state(void) { return g_sock.state; }
size_t      tcp_rx_available(void)  { return g_sock.rx_count; }

// Данных больше не будет: соединение закрыто (FIN от сервера или RST) и
// буфер пуст. Читающий цикл по этому признаку заканчивает ждать.
bool tcp_peer_closed(void) {
    tcp_state_t st = g_sock.state;
    bool closing = (st == TCP_STATE_CLOSED || st == TCP_STATE_CLOSE_WAIT ||
                    st == TCP_STATE_LAST_ACK);
    return closing && g_sock.rx_count == 0;
}

bool tcp_was_reset(void) { return g_sock.reset; }

// Маршрутизатор ответил, что адрес недостижим. Верим только во время
// установления соединения и только при совпадении всех трёх примет нашего SYN
// (посторонний ICMP уже работающее соединение не рвёт).
void tcp_icmp_unreachable(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
    uint64_t fl = net_irq_save();
    if (g_sock.state == TCP_STATE_SYN_SENT && g_sock.remote_ip == dst_ip &&
        g_sock.remote_port == dst_port && g_sock.local_port == src_port) {
        g_sock.unreachable = true;
        g_sock.state = TCP_STATE_CLOSED;
    }
    net_irq_restore(fl);
}

static uint16_t pick_local_port(void) {
    uint64_t t = tcp_rdtsc();
    uint32_t r = (uint32_t)(t ^ (t >> 31) ^ (uint32_t)now_ms());
    return (uint16_t)(49152u + (r % 16000u));      // ephemeral range
}

static uint32_t pick_isn(void) {
    uint64_t t = tcp_rdtsc();
    uint32_t isn = (uint32_t)(t ^ (t >> 27)) ^ 0x5BD1E995u;
    if (isn == 0) isn = 0x1000;
    return isn;
}

// Сбросить сокет в начальное состояние. Под запретом прерываний: иначе
// сегмент прошлого соединения, пришедший посреди обнуления, разбирался бы по
// наполовину стёртой структуре.
static void sock_reset(void) {
    uint64_t fl = net_irq_save();
    for (size_t i = 0; i < sizeof(g_sock); i++) ((uint8_t*)&g_sock)[i] = 0;
    g_sock.state = TCP_STATE_CLOSED;
    net_irq_restore(fl);
}

bool tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    // Прошлое соединение, которое никто не закрыл, — сбросить у сервера.
    if (g_sock.state >= TCP_STATE_ESTABLISHED) tcp_abort();
    sock_reset();

    net_interface_t* iface = netif_get_default();
    if (!iface)                { net_set_error(NET_ERR_NO_IFACE); return false; }
    if (iface->ip4_addr == 0)  { net_set_error(NET_ERR_NO_ADDR);  return false; }
    // MAC следующего узла — заранее и из главного потока (см. ipv4.c).
    if (!ipv4_route_ready(dst_ip)) {
        if (net_last_error() == NET_OK) net_set_error(NET_ERR_UNREACHABLE);
        serial_puts("[tcp] no route to "); netif_dump_ip4(dst_ip); serial_puts("\n");
        return false;
    }

    uint64_t fl = net_irq_save();
    g_sock.remote_ip   = dst_ip;
    g_sock.remote_port = dst_port;
    g_sock.local_port  = pick_local_port();
    g_sock.snd_nxt     = pick_isn();
    g_sock.snd_una     = g_sock.snd_nxt;
    g_sock.rcv_nxt     = 0;
    uint32_t syn_seq   = g_sock.snd_nxt;
    // Состояние — ДО отправки: SYN-ACK от slirp приходит через микросекунды,
    // и обработчик должен уже видеть SYN_SENT.
    g_sock.state = TCP_STATE_SYN_SENT;
    bool sent = send_segment(TCP_SYN, 0, 0);
    net_irq_restore(fl);

    serial_puts("[tcp] SYN to "); netif_dump_ip4(dst_ip);
    serial_puts(":"); serial_print_dec(dst_port);
    serial_puts(" from port "); serial_print_dec(g_sock.local_port);
    serial_puts("\n");
    if (!sent) { g_sock.state = TCP_STATE_CLOSED; net_set_error(NET_ERR_UNREACHABLE); return false; }

    uint64_t start_ms = now_ms();
    uint64_t last_syn = start_ms;
    int      retries  = 0;
    while (g_sock.state == TCP_STATE_SYN_SENT) {
        uint64_t t = now_ms();
        if (net_cancelled()) {
            g_sock.state = TCP_STATE_CLOSED;
            net_set_error(NET_ERR_CANCELLED);
            return false;
        }
        if (t - start_ms > TCP_CONNECT_TIMEOUT_MS) {
            serial_puts("[tcp] connect timeout\n");
            g_sock.state = TCP_STATE_CLOSED;
            net_set_error(NET_ERR_CONNECT);
            return false;
        }
        if (t - last_syn > TCP_SYN_RETRY_MS && retries < TCP_SYN_RETRIES) {
            retries++;
            last_syn = t;
            serial_puts("[tcp] SYN retransmit\n");
            resend_segment(syn_seq, TCP_SYN, 0, 0);
        }
        net_idle();
    }
    if (g_sock.state >= TCP_STATE_ESTABLISHED) return true;
    net_set_error(g_sock.refused ? NET_ERR_REFUSED
                : g_sock.unreachable ? NET_ERR_UNREACHABLE : NET_ERR_CONNECT);
    return false;
}

size_t tcp_send(const void* data, size_t len) {
    if (g_sock.state != TCP_STATE_ESTABLISHED) return 0;
    if (len > TCP_MSS) len = TCP_MSS;

    uint64_t fl = net_irq_save();
    uint32_t seq = g_sock.snd_nxt;
    bool sent = send_segment(TCP_PSH | TCP_ACK, data, len);
    net_irq_restore(fl);
    if (!sent) return 0;
    uint32_t target = seq + (uint32_t)len;

    // Ждём ACK; при потере — повтор. Данные от сервера тоже означают, что
    // запрос дошёл.
    uint64_t t0 = now_ms();
    int retries = 0;
    while ((int32_t)(g_sock.snd_una - target) < 0) {
        if (g_sock.state != TCP_STATE_ESTABLISHED &&
            g_sock.state != TCP_STATE_CLOSE_WAIT) break;
        if (g_sock.rx_count > 0) break;
        if (net_cancelled()) break;
        if (now_ms() - t0 > TCP_SEND_ACK_WAIT_MS) {
            if (++retries > TCP_SEND_RETRIES) {
                serial_puts("[tcp] send: no ACK after retries\n");
                break;
            }
            serial_puts("[tcp] data retransmit\n");
            resend_segment(seq, TCP_PSH | TCP_ACK, data, len);
            t0 = now_ms();
        }
        net_idle();
    }
    return len;
}

bool tcp_recv(void* buf, size_t cap, size_t* got) {
    if (!got) return false;
    // [41.2] Кольцо делят главный поток и обработчик прерывания (rx_store):
    // копируем при запрещённых прерываниях (до 64 КБ — десятки микросекунд)
    // и уменьшаем счётчик, а не перезаписываем его.
    uint64_t fl = net_irq_save();
    size_t have = g_sock.rx_count;
    size_t n = have < cap ? have : cap;
    if (n > 0) {
        uint8_t* out = (uint8_t*)buf;
        size_t tail = g_sock.rx_tail;
        for (size_t i = 0; i < n; i++) {
            out[i] = g_sock.rx_buf[tail];
            if (++tail == TCP_RX_BUF_SIZE) tail = 0;
        }
        g_sock.rx_tail   = tail;
        g_sock.rx_count -= n;
    }
    // Освободили место — сообщаем об обновлённом окне (window update), иначе
    // сервер может ждать бесконечно после zero-window. Шлём, когда буфер был
    // заполнен хотя бы наполовину: мелкие чтения окно не дёргают.
    bool update = n > 0 && g_sock.state == TCP_STATE_ESTABLISHED &&
                  have >= TCP_RX_BUF_SIZE / 2;
    if (update) send_segment(TCP_ACK, 0, 0);
    net_irq_restore(fl);
    *got = n;
    return n > 0;
}

// Немедленный разрыв: RST серверу, сокет закрыт, буфер приёма выброшен.
void tcp_abort(void) {
    uint64_t fl = net_irq_save();
    tcp_state_t st = g_sock.state;
    if (st == TCP_STATE_SYN_SENT || st >= TCP_STATE_ESTABLISHED) {
        emit(g_sock.remote_ip, g_sock.local_port, g_sock.remote_port,
             g_sock.snd_nxt, g_sock.rcv_nxt, TCP_RST | TCP_ACK, 0, 0, 0);
        serial_puts("[tcp] RST sent (abort)\n");
    }
    g_sock.state = TCP_STATE_CLOSED;
    g_sock.rx_count = 0; g_sock.rx_head = 0; g_sock.rx_tail = 0;
    net_irq_restore(fl);
}

void tcp_close(void) {
    uint64_t fl = net_irq_save();
    tcp_state_t st = g_sock.state;
    if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_CLOSE_WAIT) {
        // LISTEN / SYN_* / уже закрыто — просто закрываем.
        if (st == TCP_STATE_SYN_SENT || st == TCP_STATE_SYN_RCVD) {
            net_irq_restore(fl);
            tcp_abort();
            return;
        }
        if (st < TCP_STATE_ESTABLISHED) g_sock.state = TCP_STATE_CLOSED;
        net_irq_restore(fl);
        return;
    }
    // Непрочитанные данные: вежливый FIN тут обманул бы сервер («всё принято»),
    // а он продолжил бы слать остаток страницы. Рвём соединение (RFC 2525).
    if (g_sock.rx_count > 0) {
        net_irq_restore(fl);
        tcp_abort();
        return;
    }
    uint32_t fin_seq = g_sock.snd_nxt;
    g_sock.state = (st == TCP_STATE_ESTABLISHED) ? TCP_STATE_FIN_WAIT_1 : TCP_STATE_LAST_ACK;
    send_segment(TCP_FIN | TCP_ACK, 0, 0);
    net_irq_restore(fl);
    serial_puts("[tcp] FIN\n");

    // Ждём завершения недолго: сервер с `Connection: close` уже прислал свой
    // FIN, подтверждение нашего приходит за миллисекунды. Если нас вызвали из
    // обработчика/секции (ждать нельзя) — выходим сразу.
    if (!net_irqs_enabled()) return;
    uint64_t t0 = now_ms();
    bool resent = false;
    while (g_sock.state != TCP_STATE_CLOSED && now_ms() - t0 < TCP_FIN_WAIT_MS) {
        // Наш FIN подтверждён, а сервер свой ещё не прислал — данные от него
        // нам уже не нужны; дальше ждать нечего.
        if (g_sock.state == TCP_STATE_FIN_WAIT_2 && now_ms() - t0 >= TCP_FIN_RETRY_MS) break;
        if (!resent && now_ms() - t0 >= TCP_FIN_RETRY_MS &&
            (g_sock.state == TCP_STATE_FIN_WAIT_1 || g_sock.state == TCP_STATE_LAST_ACK)) {
            resent = true;
            resend_segment(fin_seq, TCP_FIN | TCP_ACK, 0, 0);
        }
        __asm__ __volatile__("sti; hlt");
    }
    // Не дождались — рвём, чтобы у сервера не осталось полуоткрытого
    // соединения, а к нам не шли его повторы.
    if (g_sock.state != TCP_STATE_CLOSED) tcp_abort();
}

// ---- [этап 47.5] Server-side passive open --------------------------------
bool tcp_listen(uint16_t port) {
    if (g_sock.state >= TCP_STATE_ESTABLISHED) tcp_abort();
    sock_reset();
    uint64_t fl = net_irq_save();
    g_sock.local_port = port;
    g_sock.state      = TCP_STATE_LISTEN;
    net_irq_restore(fl);
    serial_puts("[tcp] LISTEN on port ");
    serial_print_dec(port);
    serial_puts("\n");
    return true;
}

bool tcp_accept(uint32_t* peer_ip, uint16_t* peer_port, uint32_t timeout_ms) {
    if (g_sock.state != TCP_STATE_LISTEN && g_sock.state != TCP_STATE_SYN_RCVD) {
        return false;
    }
    uint64_t start_ms = now_ms();
    while (g_sock.state != TCP_STATE_ESTABLISHED && g_sock.state != TCP_STATE_CLOSED) {
        if (timeout_ms && now_ms() - start_ms > timeout_ms) {
            return false;
        }
        net_idle();
    }
    if (g_sock.state != TCP_STATE_ESTABLISHED) return false;
    if (peer_ip)   *peer_ip   = g_sock.remote_ip;
    if (peer_port) *peer_port = g_sock.remote_port;
    return true;
}

// Записать в кольцевой буфер; возвращает сколько байт реально влезло.
// Зовётся только из tcp_rx (прерывания запрещены).
static size_t rx_store(const uint8_t* data, size_t len) {
    size_t room = TCP_RX_BUF_SIZE - g_sock.rx_count;
    size_t n = len < room ? len : room;
    size_t head = g_sock.rx_head;
    for (size_t i = 0; i < n; i++) {
        g_sock.rx_buf[head] = data[i];
        if (++head == TCP_RX_BUF_SIZE) head = 0;
    }
    g_sock.rx_head  = head;
    g_sock.rx_count += n;
    return n;
}

void tcp_rx(uint32_t src_ip, uint32_t dst_ip,
            const void* payload, size_t len) {
    (void)dst_ip;
    if (len < TCP_HDR_LEN) return;
    const uint8_t* p = (const uint8_t*)payload;

    uint16_t src_port = be16(p + 0);
    uint16_t dst_port = be16(p + 2);
    uint32_t seq      = be32(p + 4);
    uint32_t ack      = be32(p + 8);
    uint8_t  off_res  = p[12];
    uint8_t  flags    = p[13];
    uint8_t  data_off = (off_res >> 4) * 4;
    if (data_off < TCP_HDR_LEN || data_off > len) return;
    size_t data_len = len - data_off;
    const uint8_t* data = p + data_off;

    // Обработчик обычно и так идёт с запрещёнными прерываниями; секция нужна
    // на случай вызова из главного потока (опрос e1000 при загрузке).
    uint64_t fl = net_irq_save();
    tcp_state_t st = g_sock.state;

    // [47.5] Server-side: LISTEN ещё не знает remote_ip/port.
    if (st == TCP_STATE_LISTEN) {
        if (dst_port == g_sock.local_port && (flags & TCP_SYN) && !(flags & (TCP_ACK | TCP_RST))) {
            g_sock.remote_ip   = src_ip;
            g_sock.remote_port = src_port;
            g_sock.snd_nxt     = pick_isn();
            g_sock.snd_una     = g_sock.snd_nxt;
            g_sock.rcv_nxt     = seq + 1;
            g_sock.state       = TCP_STATE_SYN_RCVD;
            send_segment(TCP_SYN | TCP_ACK, 0, 0);
            serial_puts("[tcp] SYN_RCVD from "); netif_dump_ip4(src_ip);
            serial_puts("\n");
        } else if (dst_port != g_sock.local_port) {
            reply_rst(src_ip, src_port, dst_port, seq, ack, flags, data_len);
        }
        net_irq_restore(fl);
        return;
    }

    // Сегмент не нашего соединения (или сокет закрыт): вежливый отказ.
    bool ours = st != TCP_STATE_CLOSED && src_ip == g_sock.remote_ip &&
                src_port == g_sock.remote_port && dst_port == g_sock.local_port;
    if (!ours) {
        reply_rst(src_ip, src_port, dst_port, seq, ack, flags, data_len);
        net_irq_restore(fl);
        return;
    }

    if (st == TCP_STATE_SYN_RCVD) {
        if (flags & TCP_RST) { g_sock.state = TCP_STATE_LISTEN; net_irq_restore(fl); return; }
        if ((flags & TCP_ACK) && ack == g_sock.snd_nxt) {
            g_sock.snd_una = ack;
            g_sock.state = st = TCP_STATE_ESTABLISHED;
            serial_puts("[tcp] ESTABLISHED (server)\n");
        } else { net_irq_restore(fl); return; }
    }

    if (st == TCP_STATE_SYN_SENT) {
        if (flags & TCP_RST) {
            // RST действителен, только если подтверждает наш SYN.
            if ((flags & TCP_ACK) && ack == g_sock.snd_nxt) {
                serial_puts("[tcp] connection refused (RST)\n");
                g_sock.refused = true;
                g_sock.state = TCP_STATE_CLOSED;
            }
        } else if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == g_sock.snd_nxt) {
                g_sock.rcv_nxt = seq + 1;
                g_sock.snd_una = ack;
                g_sock.state   = TCP_STATE_ESTABLISHED;
                send_segment(TCP_ACK, 0, 0);
                serial_puts("[tcp] ESTABLISHED\n");
            } else {
                // SYN-ACK на чужой (старый) SYN: сбросить полуоткрытое соединение.
                emit(src_ip, dst_port, src_port, ack, 0, TCP_RST, 0, 0, 0);
            }
        }
        net_irq_restore(fl);
        return;
    }

    // ---- соединение установлено (или закрывается) ------------------------
    int32_t delta = (int32_t)(seq - g_sock.rcv_nxt);

    if (flags & TCP_RST) {
        // В окне приёма — настоящий; иначе посторонний/устаревший, игнорируем.
        if (delta >= 0 && delta <= 65535) {
            serial_puts("[tcp] RST from peer\n");
            g_sock.reset = true;
            g_sock.state = TCP_STATE_CLOSED;
        }
        net_irq_restore(fl);
        return;
    }

    if (flags & TCP_ACK) {
        // Подтверждать можно только уже отправленное.
        if ((int32_t)(ack - g_sock.snd_una) > 0 && (int32_t)(ack - g_sock.snd_nxt) <= 0)
            g_sock.snd_una = ack;
    }

    bool need_ack = false;
    bool in_order_end = false;       // после этого сегмента поток цел до его конца
    if (data_len > 0) {
        bool can_take = (st == TCP_STATE_ESTABLISHED || st == TCP_STATE_FIN_WAIT_1 ||
                         st == TCP_STATE_FIN_WAIT_2);
        if (delta == 0 && can_take) {
            size_t n = rx_store(data, data_len);
            g_sock.rcv_nxt += (uint32_t)n;
            in_order_end = (n == data_len);
        } else if (delta < 0 && can_take) {
            // Повтор уже принятого: берём только новый хвост.
            size_t skip = (size_t)(-(int64_t)delta);
            if (skip < data_len) {
                size_t n = rx_store(data + skip, data_len - skip);
                g_sock.rcv_nxt += (uint32_t)n;
                in_order_end = (n == data_len - skip);
            } else if (skip == data_len) {
                in_order_end = true;               // весь сегмент уже у нас
            }
        }
        // delta > 0 — сегмент «из будущего» (предыдущий потерян): повторный
        // ACK на rcv_nxt просит сервер переслать пропущенное.
        need_ack = true;
    } else {
        in_order_end = (delta == 0);
    }

    if (flags & TCP_FIN) {
        // FIN занимает номер сразу за данными сегмента. Принимаем его, только
        // когда всё до него получено; повтор уже принятого FIN лишь
        // подтверждаем заново.
        if (in_order_end) {
            g_sock.rcv_nxt++;
            switch (g_sock.state) {
                case TCP_STATE_ESTABLISHED: g_sock.state = TCP_STATE_CLOSE_WAIT; break;
                case TCP_STATE_FIN_WAIT_1:
                    // Встречное закрытие: если наш FIN уже подтверждён — всё.
                    g_sock.state = (g_sock.snd_una == g_sock.snd_nxt)
                                 ? TCP_STATE_CLOSED : TCP_STATE_LAST_ACK;
                    break;
                case TCP_STATE_FIN_WAIT_2:  g_sock.state = TCP_STATE_CLOSED; break;
                default: break;
            }
        }
        need_ack = true;
    }

    // Наш FIN подтверждён.
    if ((flags & TCP_ACK) && g_sock.snd_una == g_sock.snd_nxt) {
        if (g_sock.state == TCP_STATE_FIN_WAIT_1)    g_sock.state = TCP_STATE_FIN_WAIT_2;
        else if (g_sock.state == TCP_STATE_LAST_ACK) g_sock.state = TCP_STATE_CLOSED;
    }

    if (need_ack) send_segment(TCP_ACK, 0, 0);
    net_irq_restore(fl);
}
