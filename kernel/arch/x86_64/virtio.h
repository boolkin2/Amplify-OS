// ============================================================================
// virtio.h — общий transport-слой VIRTIO PCI для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 41: ПОЛНЫЙ VIRTIO 1.0 modern transport через MMIO BAR'ы и PCI
// capability list (4.1.4). Legacy IO-путь оставлен fallback'ом, но
// virtio-gpu в QEMU работает строго modern: BAR0 — MMIO, BAR4 — MSI-X,
// и device НЕ имеет legacy IO BAR.
//
// Для AMPLIFY-OS этого достаточно. Когда понадобится legacy (на каких-то
// древних ARM-virt-машинах или старых VMware), просто сделаем второй probe.
//
// Спецификация:
//   https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html
//   §4.1.4 — virtio over PCI bus, modern interface
//   §2.6   — virtqueue layout
// ============================================================================
#ifndef AOS_X86_64_VIRTIO_H
#define AOS_X86_64_VIRTIO_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "idt.h"

// ----------------------------------------------------------------------------
// Vendor / device IDs.
// ----------------------------------------------------------------------------
#define VIRTIO_PCI_VENDOR_ID            0x1AF4

// Transitional/modern device IDs.
#define VIRTIO_DEV_NET_TRANS            0x1000
#define VIRTIO_DEV_BLK_TRANS            0x1001
#define VIRTIO_DEV_GPU_TRANS            0x1050
// [41.2] Modern-only устройства: PCI device id = 0x1040 + тип устройства.
#define VIRTIO_DEV_NET_MODERN           0x1041
#define VIRTIO_DEV_BLK_MODERN           0x1042
#define VIRTIO_DEV_INPUT                0x1052

// ----------------------------------------------------------------------------
// PCI capability ID для virtio: 0x09 (Vendor Specific). Каждая такая
// capability содержит cfg_type, BAR index, offset и length. (4.1.4.1)
// ----------------------------------------------------------------------------
#define PCI_CAP_ID_VENDOR               0x09

// virtio_pci_cap.cfg_type значения.
#define VIRTIO_PCI_CAP_COMMON_CFG       1   // Common configuration
#define VIRTIO_PCI_CAP_NOTIFY_CFG       2   // Notifications
#define VIRTIO_PCI_CAP_ISR_CFG          3   // ISR Status
#define VIRTIO_PCI_CAP_DEVICE_CFG       4   // Device specific configuration
#define VIRTIO_PCI_CAP_PCI_CFG          5   // PCI config access (не используем)

// Структура virtio_pci_cap (4.1.4.1).
typedef struct __attribute__((packed)) {
    uint8_t  cap_vndr;     // 0x09
    uint8_t  cap_next;
    uint8_t  cap_len;      // sizeof(virtio_pci_cap_t)
    uint8_t  cfg_type;
    uint8_t  bar;
    uint8_t  padding[3];
    uint32_t offset;
    uint32_t length;
} virtio_pci_cap_t;

// notify cap имеет дополнительное поле notify_off_multiplier.
typedef struct __attribute__((packed)) {
    virtio_pci_cap_t cap;
    uint32_t         notify_off_multiplier;
} virtio_pci_notify_cap_t;

// ----------------------------------------------------------------------------
// Layout common configuration (BAR + offset из COMMON_CFG cap, см. 4.1.4.3).
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    // about whole device
    uint32_t device_feature_select;     // RW
    uint32_t device_feature;            // R
    uint32_t driver_feature_select;     // RW
    uint32_t driver_feature;            // RW
    uint16_t msix_config;               // RW
    uint16_t num_queues;                // R
    uint8_t  device_status;             // RW
    uint8_t  config_generation;         // R
    // about a specific virtqueue
    uint16_t queue_select;              // RW
    uint16_t queue_size;                // RW
    uint16_t queue_msix_vector;         // RW
    uint16_t queue_enable;              // RW
    uint16_t queue_notify_off;          // R
    uint64_t queue_desc;                // RW
    uint64_t queue_driver;              // RW (avail)
    uint64_t queue_device;              // RW (used)
} virtio_pci_common_cfg_t;

// Device Status (4.1.4.3).
#define VIRTIO_STATUS_RESET                 0x00
#define VIRTIO_STATUS_ACKNOWLEDGE           0x01
#define VIRTIO_STATUS_DRIVER                0x02
#define VIRTIO_STATUS_DRIVER_OK             0x04
#define VIRTIO_STATUS_FEATURES_OK           0x08
#define VIRTIO_STATUS_FAILED                0x80

// Общие feature биты.
#define VIRTIO_F_RING_INDIRECT_DESC         28
#define VIRTIO_F_RING_EVENT_IDX             29
#define VIRTIO_F_VERSION_1                  32   // bit 32 — обязателен для modern

// ----------------------------------------------------------------------------
// virtqueue (2.6) — layout не изменился между legacy и modern.
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2
#define VIRTQ_DESC_F_INDIRECT   4

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

// ----------------------------------------------------------------------------
// virtq_t — софтверный handle.
// ----------------------------------------------------------------------------
typedef struct {
    uint16_t       index;
    uint16_t       num;
    virtq_desc_t*  desc;
    virtq_avail_t* avail;
    virtq_used_t*  used;
    uint64_t       desc_phys;
    uint64_t       avail_phys;
    uint64_t       used_phys;
    uint16_t       free_head;
    uint16_t       free_count;
    uint16_t       last_used_idx;
    uint8_t*       backing;
    uint16_t       notify_off;       // queue_notify_off для modern (умножается на multiplier)
    bool           irq;              // [41.2] очереди назначен вектор MSI-X
} virtq_t;

// ----------------------------------------------------------------------------
// virtio_dev_t — handle устройства.
// ----------------------------------------------------------------------------
typedef struct {
    bool         present;
    bool         is_modern;          // modern (MMIO+capabilities) или legacy (IO)
    pci_device_t pci;
    uint64_t     features;

    // Modern transport state.
    volatile virtio_pci_common_cfg_t* cfg;   // virt = phys (kernel paging)
    volatile uint8_t*                 isr;
    volatile uint8_t*                 notify_base;
    uint32_t                          notify_off_multiplier;
    volatile uint8_t*                 device_cfg;   // device-specific

    // Legacy transport state (для совместимости).
    uint16_t io_base;

    // [41.2] MSI-X: таблица векторов устройства (BAR + смещение из PCI
    // capability 0x11). msix_on — MSI-X включён, INTx устройства отключён.
    bool               msix_on;
    uint16_t           msix_count;
    volatile uint32_t* msix_table;
} virtio_dev_t;

// ----------------------------------------------------------------------------
// Public API. Назначение и сигнатуры неизменны: virtio_gpu.c использует
// тот же набор функций и не знает modern/legacy.
//
// ВАЖНО: probe заполняет struct через указатель, а не возвращает по value.
// Возврат by-value через `volatile T*`-поля ломал struct-copy у GCC: код
// после `return d;` копировал struct ПО ЧАСТЯМ, и volatile-pointer-fields
// (cfg/isr/notify_base/device_cfg) теряли значения по дороге (видно как
// нули в копии). Через указатель проблема отсутствует — struct никогда
// не копируется.
// ----------------------------------------------------------------------------
void         virtio_pci_probe(virtio_dev_t* dev, uint16_t device_id);
// [41.2] n-е по счёту устройство с таким id (0 — первое): у virtio-input
// клавиатура и планшет — два устройства с одним и тем же 0x1052.
void         virtio_pci_probe_nth(virtio_dev_t* dev, uint16_t device_id, int n);
bool         virtio_pci_init_begin(virtio_dev_t* dev);
uint64_t     virtio_pci_negotiate(virtio_dev_t* dev, uint64_t allowed);
void         virtio_pci_init_end(virtio_dev_t* dev);

uint8_t  virtio_dev_cfg_read8 (virtio_dev_t* dev, uint16_t off);
uint16_t virtio_dev_cfg_read16(virtio_dev_t* dev, uint16_t off);
uint32_t virtio_dev_cfg_read32(virtio_dev_t* dev, uint16_t off);
uint8_t  virtio_pci_read_isr  (virtio_dev_t* dev);
void     virtio_dev_cfg_write8(virtio_dev_t* dev, uint16_t off, uint8_t v);

// ----------------------------------------------------------------------------
// [41.2] Прерывания MSI-X вместо опроса.
//   virtio_pci_msix_setup — найти capability, отобразить таблицу, включить
//       MSI-X (все записи замаскированы). false — у устройства MSI-X нет.
//   virtio_pci_msix_route — привязать запись таблицы к новому вектору IDT с
//       обработчиком h (доставка на BSP). false — кончились векторы.
//   virtq_init_irq        — как virtq_init, но очередь шлёт прерывание через
//       запись msix_entry (VIRTIO_MSI_NO_VECTOR — без прерываний).
// Звать между virtio_pci_negotiate и virtio_pci_init_end. Обработчик обязан
// закончиться lapic_eoi().
// ----------------------------------------------------------------------------
#define VIRTIO_MSI_NO_VECTOR  0xFFFF
bool     virtio_pci_msix_setup(virtio_dev_t* dev);
bool     virtio_pci_msix_route(virtio_dev_t* dev, uint16_t entry, int_handler_t h);

bool     virtq_init(virtio_dev_t* dev, virtq_t* vq, uint16_t index, uint16_t num);
bool     virtq_init_irq(virtio_dev_t* dev, virtq_t* vq, uint16_t index, uint16_t num,
                        uint16_t msix_entry);
// Уведомить устройство о новых буферах в avail (без добавления буфера).
void     virtq_notify(virtio_dev_t* dev, virtq_t* vq);
void     virtq_free(virtq_t* vq);
uint16_t virtq_alloc_chain(virtq_t* vq, uint16_t n);
void     virtq_free_chain (virtq_t* vq, uint16_t head);
void     virtq_submit_kick(virtio_dev_t* dev, virtq_t* vq, uint16_t head);
bool     virtq_wait_used  (virtq_t* vq, uint16_t head, uint64_t timeout_ticks,
                           uint32_t* out_len);
uint16_t virtq_used_count (const virtq_t* vq);
uint64_t virtq_wait_spins (void);   // [41.2] обороты busy-wait за всё время

#endif
