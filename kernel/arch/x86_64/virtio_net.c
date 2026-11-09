// ============================================================================
// virtio_net.c — драйвер virtio-net (см. virtio_net.h).
// ----------------------------------------------------------------------------
// Очереди (virtio 1.x §5.1): 0 — приём, 1 — передача. Перед каждым кадром идёт
// заголовок virtio_net_hdr (12 байт в версии 1.0: поле num_buffers есть
// всегда). Контрольные суммы и сегментацию хосту не отдаём (features: только
// MAC), поэтому заголовок на передаче — нули, на приёме — пропускаем.
//
// Приём: 64 буфера по 2 КБ постоянно лежат у устройства; заполненные оно
// возвращает через used и шлёт прерывание MSI-X. Обработчик отдаёт кадр стеку
// (netif_receive — так же, из прерывания, делает e1000) и сразу возвращает
// буфер устройству. Раз в 10 мс тот же разбор зовёт таймер — страховка на
// случай потерянного прерывания (и единственный путь, если MSI-X нет).
//
// Передача: кадр копируется в свой слот (заголовок + данные одним куском) и
// кладётся в очередь; ответа не ждём, слоты собираем обратно перед следующей
// отправкой. netif_send зовут и из главного потока, и из прерывания (ответы
// TCP/ARP на принятый кадр) — поэтому передача идёт с запрещёнными прерываниями.
// ============================================================================
#include "virtio_net.h"
#include "memory.h"
#include "virtio.h"
#include "netif.h"
#include "lapic.h"
#include "timer.h"
#include "serial.h"

#define VNET_F_MAC          5
#define VNET_HDR_LEN        12
#define VNET_BUF_SIZE       2048          // заголовок + кадр 1514 с запасом
#define VNET_RX_BUFS        64
#define VNET_TX_BUFS        128           // слоты возвращаются лениво — берём с запасом
#define VRING_AVAIL_F_NO_INTERRUPT 1

typedef struct {
    bool            ready;
    virtio_dev_t    dev;
    virtq_t         rxq, txq;
    net_interface_t iface;
    uint8_t         mac[6];
    uint16_t        rx_n;                 // сколько буферов приёма реально положено
    uint8_t         tx_free[VNET_TX_BUFS];
    int             tx_nfree;
    volatile uint64_t rx_frames, tx_frames, irqs, tx_drops;
    uint8_t         rx_buf[VNET_RX_BUFS][VNET_BUF_SIZE] __attribute__((aligned(16)));
    uint8_t         tx_buf[VNET_TX_BUFS][VNET_BUF_SIZE] __attribute__((aligned(16)));
} vnet_t;

static vnet_t g;

bool           virtio_net_is_ready(void)  { return g.ready; }
const uint8_t* virtio_net_get_mac(void)   { return g.ready ? g.mac : 0; }
uint64_t       virtio_net_rx_frames(void) { return g.rx_frames; }
uint64_t       virtio_net_tx_frames(void) { return g.tx_frames; }
uint64_t       virtio_net_irqs(void)      { return g.irqs; }
uint64_t       virtio_net_tx_drops(void)  { return g.tx_drops; }

// Разобрать принятые кадры. Только из обработчиков прерываний (MSI-X или
// таймер): они не вложены друг в друга, блокировок не нужно.
static void vnet_rx_drain(void) {
    if (!g.ready) return;
    // Защита от повторного входа. Сюда ведут два пути — прерывание MSI-X и
    // таймер, — а стек внутри netif_receive может разрешить прерывания
    // (ожидание ARP-ответа делает `sti; hlt`). Тогда таймер вклинился бы в
    // разбор кадра и начал разбирать следующий: TCP получал сегменты
    // вперемешку и поток вставал. Вложенный вызов просто выходит — кадры
    // доберёт внешний цикл.
    static volatile bool busy = false;
    if (busy) return;
    busy = true;
    virtq_t* q = &g.rxq;
    bool any = false;
    while (q->last_used_idx != q->used->idx) {
        virtq_used_elem_t el = q->used->ring[q->last_used_idx % q->num];
        q->last_used_idx = (uint16_t)(q->last_used_idx + 1);
        uint32_t id = el.id;
        if (id >= g.rx_n) continue;
        if (el.len > VNET_HDR_LEN && el.len <= VNET_BUF_SIZE) {
            g.rx_frames++;
            netif_receive(g.rx_buf[id] + VNET_HDR_LEN, el.len - VNET_HDR_LEN);
        }
        q->avail->ring[q->avail->idx % q->num] = (uint16_t)id;
        __asm__ __volatile__("sfence" ::: "memory");
        q->avail->idx = (uint16_t)(q->avail->idx + 1);
        any = true;
    }
    if (any) {
        __asm__ __volatile__("mfence" ::: "memory");
        virtq_notify(&g.dev, q);
    }
    busy = false;
}

static void vnet_irq(registers64_t* r) {
    (void)r;
    g.irqs++;
    vnet_rx_drain();
    lapic_eoi();
}

static void vnet_tx_reclaim(void) {
    virtq_t* q = &g.txq;
    while (q->last_used_idx != q->used->idx) {
        uint32_t id = q->used->ring[q->last_used_idx % q->num].id;
        q->last_used_idx = (uint16_t)(q->last_used_idx + 1);
        if (id < VNET_TX_BUFS && g.tx_nfree < VNET_TX_BUFS) g.tx_free[g.tx_nfree++] = (uint8_t)id;
    }
}

static size_t vnet_send(const void* data, size_t len) {
    if (!g.ready || !data || len == 0 || len > VNET_BUF_SIZE - VNET_HDR_LEN) return 0;
    // Запретить прерывания на время работы с очередью передачи и вернуть
    // прежнее состояние флага IF (нас могли позвать из обработчика).
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl));

    vnet_tx_reclaim();
    size_t sent = 0;
    if (g.tx_nfree > 0) {
        uint8_t i = g.tx_free[--g.tx_nfree];
        uint8_t* b = g.tx_buf[i];
        for (int k = 0; k < VNET_HDR_LEN; k++) b[k] = 0;
        const uint8_t* s = (const uint8_t*)data;
        for (size_t k = 0; k < len; k++) b[VNET_HDR_LEN + k] = s[k];

        virtq_t* q = &g.txq;
        q->desc[i].addr  = virt_to_phys(b);
        q->desc[i].len   = (uint32_t)(VNET_HDR_LEN + len);
        q->desc[i].flags = 0;
        q->desc[i].next  = 0;
        q->avail->ring[q->avail->idx % q->num] = i;
        __asm__ __volatile__("sfence" ::: "memory");
        q->avail->idx = (uint16_t)(q->avail->idx + 1);
        __asm__ __volatile__("mfence" ::: "memory");
        virtq_notify(&g.dev, q);
        g.tx_frames++;
        sent = len;
    } else {
        g.tx_drops++;          // устройство не успело вернуть слоты — кадр потерян, TCP повторит
    }

    if (fl & 0x200) __asm__ __volatile__("sti");
    return sent;
}

bool virtio_net_init(void) {
    g.ready = false;
    virtio_pci_probe(&g.dev, VIRTIO_DEV_NET_MODERN);
    if (!g.dev.present) virtio_pci_probe(&g.dev, VIRTIO_DEV_NET_TRANS);
    if (!g.dev.present) return false;
    if (!g.dev.is_modern) {
        serial_puts("[vnet] device without modern capabilities - skipped\n");
        return false;
    }
    if (!virtio_pci_init_begin(&g.dev)) return false;
    uint64_t feat = virtio_pci_negotiate(&g.dev, 1ULL << VNET_F_MAC);

    // Запись 0 таблицы MSI-X — очередь приёма. Передаче прерывание не нужно.
    uint16_t rx_entry = VIRTIO_MSI_NO_VECTOR;
    if (virtio_pci_msix_setup(&g.dev) && virtio_pci_msix_route(&g.dev, 0, vnet_irq)) rx_entry = 0;
    if (!virtq_init_irq(&g.dev, &g.rxq, 0, 0, rx_entry) ||
        !virtq_init_irq(&g.dev, &g.txq, 1, 0, VIRTIO_MSI_NO_VECTOR)) {
        serial_puts("[vnet] queue init failed\n");
        return false;
    }
    // Очередями управляем сами (постоянные слоты), в hlt на них не ждём.
    g.rxq.free_count = 0; g.txq.free_count = 0;
    bool rx_irq = g.rxq.irq;
    g.rxq.irq = false;
    g.txq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    // Буферы приёма — устройству.
    g.rx_n = g.rxq.num < VNET_RX_BUFS ? g.rxq.num : VNET_RX_BUFS;
    for (uint16_t i = 0; i < g.rx_n; i++) {
        g.rxq.desc[i].addr  = virt_to_phys(g.rx_buf[i]);
        g.rxq.desc[i].len   = VNET_BUF_SIZE;
        g.rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        g.rxq.desc[i].next  = 0;
        g.rxq.avail->ring[i] = i;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    g.rxq.avail->idx = g.rx_n;

    g.tx_nfree = 0;
    int tx_slots = g.txq.num < VNET_TX_BUFS ? g.txq.num : VNET_TX_BUFS;
    for (int i = 0; i < tx_slots; i++) g.tx_free[g.tx_nfree++] = (uint8_t)i;

    // MAC — первые 6 байт конфигурации (если устройство его сообщает).
    if (feat & (1ULL << VNET_F_MAC)) {
        for (int i = 0; i < 6; i++) g.mac[i] = virtio_dev_cfg_read8(&g.dev, (uint16_t)i);
    } else {
        g.mac[0] = 0x52; g.mac[1] = 0x54; g.mac[2] = 0x00; g.mac[3] = 0x12; g.mac[4] = 0x34; g.mac[5] = 0x57;
    }

    const char* name = "eth0";
    for (int i = 0; i < NETIF_NAME_LEN && name[i]; i++) g.iface.name[i] = name[i];
    g.iface.name[NETIF_NAME_LEN - 1] = 0;
    for (int i = 0; i < NETIF_MAC_LEN; i++) g.iface.mac[i] = g.mac[i];
    g.iface.send = vnet_send;
    g.iface.up   = false;

    g.rx_frames = g.tx_frames = g.irqs = g.tx_drops = 0;
    g.ready = true;
    virtio_pci_init_end(&g.dev);            // DRIVER_OK — после него можно уведомлять
    virtq_notify(&g.dev, &g.rxq);

    netif_register(&g.iface);
    // Страховка приёма раз в 10 мс (см. шапку файла).
    timer_register_callback(vnet_rx_drain, 10);

    serial_puts("[vnet] virtio-net up, MAC="); netif_dump_mac(g.mac);
    serial_puts(rx_irq ? ", rx irq=MSI-X\n" : ", rx polled (10 ms)\n");
    return true;
}
