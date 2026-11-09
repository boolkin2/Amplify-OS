// ============================================================================
// netif.h — абстракция сетевого интерфейса.
// 1:1 порт kernel/net/netif.h, но 64-битный.
// ============================================================================
#ifndef AOS_X86_64_NETIF_H
#define AOS_X86_64_NETIF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NETIF_NAME_LEN  8
#define NETIF_MAC_LEN   6
#define NETIF_MTU       1500

typedef size_t (*netif_send_fn)(const void* data, size_t len);

typedef struct net_interface {
    char     name[NETIF_NAME_LEN];
    uint8_t  mac[NETIF_MAC_LEN];
    netif_send_fn send;

    uint32_t ip4_addr;
    uint32_t ip4_netmask;
    uint32_t ip4_gateway;

    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;

    bool     up;
} net_interface_t;

bool             netif_register(net_interface_t* iface);
net_interface_t* netif_get_default(void);
size_t           netif_send(const void* data, size_t len);
void             netif_receive(const void* data, size_t len);
void             netif_dump_mac(const uint8_t* mac);
void             netif_dump_ip4(uint32_t addr);
void             netif_set_ip4(net_interface_t* iface,
                               uint32_t addr, uint32_t netmask, uint32_t gateway);

bool             net_init(void);
// [41.2] Замер: скачать url по HTTP, строка `[netbench] ...` в serial.
void             net_bench_report(const char* url);

// ----------------------------------------------------------------------------
// [48.2] Общие службы сетевого стека.
//
// 1) Критические секции. Кадры приходят прерыванием в любой момент (virtio-net
//    по MSI-X, e1000 — из обработчика таймера), и весь разбор протоколов идёт
//    прямо в обработчике. Всё, что делит с ним главный поток (сборка сегмента
//    TCP, кэш ARP, кольцо приёма), берётся под net_irq_save/net_irq_restore.
// 2) Ожидание. Вместо голого `sti; hlt` стек зовёт net_idle(): она спит до
//    прерывания и не чаще раза в 33 мс вызывает «насос» (net_set_idle_hook) —
//    им браузер и Mesh перерисовывают экран, пока идёт загрузка. Насос зовётся
//    только из главного потока и не вкладывается сам в себя.
// 3) Отмена. net_cancel() заставляет все ожидания стека завершиться с ошибкой
//    NET_ERR_CANCELLED (клавиша Esc во время загрузки страницы).
// 4) Причина отказа. Слои записывают её в net_set_error — браузер показывает
//    понятную страницу ошибки, а не «см. serial».
// ----------------------------------------------------------------------------
static inline uint64_t net_irq_save(void) {
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl));
    return fl;
}
static inline void net_irq_restore(uint64_t fl) {
    if (fl & 0x200) __asm__ __volatile__("sti");
}
// Разрешены ли прерывания: false — мы в обработчике либо в критической
// секции, ждать там нельзя (ни ARP-ответа, ни чего-либо ещё).
static inline bool net_irqs_enabled(void) {
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0" : "=r"(fl));
    return (fl & 0x200) != 0;
}

typedef enum {
    NET_OK = 0,
    NET_ERR_NO_IFACE,      // сетевой карты нет
    NET_ERR_NO_ADDR,       // адрес не получен и не задан
    NET_ERR_URL,           // адрес страницы не разобрать
    NET_ERR_DNS,           // имя не разрешилось
    NET_ERR_UNREACHABLE,   // нет ARP-ответа от узла/шлюза
    NET_ERR_REFUSED,       // RST в ответ на SYN
    NET_ERR_CONNECT,       // SYN без ответа
    NET_ERR_TLS,           // рукопожатие TLS не удалось
    NET_ERR_TLS_TRUST,     // сертификат не прошёл проверку
    NET_ERR_TLS_SILENT,    // сервер не ответил на приветствие TLS ни байтом (48.2)
    NET_ERR_RESET,         // соединение сброшено посреди обмена
    NET_ERR_TIMEOUT,       // сервер замолчал
    NET_ERR_PROTOCOL,      // ответ не похож на HTTP
    NET_ERR_REDIRECTS,     // слишком длинная цепочка перенаправлений
    NET_ERR_CANCELLED,     // отменено пользователем
} net_error_t;

void        net_set_error(net_error_t e);
net_error_t net_last_error(void);
const char* net_error_name(net_error_t e);     // короткое имя для serial

typedef void (*net_idle_hook_t)(void);
void net_set_idle_hook(net_idle_hook_t fn);    // 0 — убрать
void net_idle(void);                           // спать до прерывания (+ насос)
void net_cancel(void);
void net_cancel_clear(void);
bool net_cancelled(void);

// Ход загрузки для индикатора: этап и число принятых байт тела.
typedef enum {
    NET_STAGE_IDLE = 0, NET_STAGE_DNS, NET_STAGE_CONNECT, NET_STAGE_TLS,
    NET_STAGE_REQUEST, NET_STAGE_BODY, NET_STAGE_DONE
} net_stage_t;
void        net_progress_set(net_stage_t st, uint64_t done, int64_t total);
net_stage_t net_progress_stage(void);
uint64_t    net_progress_done(void);
int64_t     net_progress_total(void);          // -1 — длина неизвестна

// Какой драйвер обслуживает интерфейс: "virtio-net", "e1000", "rtl8139".
const char* netif_driver_name(void);

// Готов ли default-интерфейс к работе (зарегистрирован + up).
// Заменяет rtl8139_is_ready() / e1000_is_ready() для driver-agnostic кода
// (DHCP, smoke-тесты, etc.).
bool             netif_is_ready(void);

static inline uint32_t ipv4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c <<  8) | (uint32_t)d;
}

static inline uint32_t net_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >>  8) | ((v & 0xFF000000u) >> 24);
}
#define net_hton32(v) net_bswap32(v)
#define net_ntoh32(v) net_bswap32(v)

#endif
