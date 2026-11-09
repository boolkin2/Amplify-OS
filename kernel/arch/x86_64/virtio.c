// ============================================================================
// virtio.c — общий transport-слой VIRTIO PCI (modern + legacy fallback).
// ----------------------------------------------------------------------------
// Modern (1.0): обнаружение через PCI capability list — 4 vendor-specific
// capabilities (COMMON_CFG / NOTIFY_CFG / ISR_CFG / DEVICE_CFG), каждая
// указывает BAR + offset. Все доступы — обычные MMIO, наша kernel-paging
// уже identity-мапит первые 4 GB, поэтому BAR-адрес = виртуальный адрес.
//
// QEMU virtio-gpu обычно сидит на BAR4 (4 KB) для всего набора capabilities.
// ============================================================================
#include "virtio.h"
#include "io.h"
#include "heap.h"
#include "memory.h"
#include "paging.h"
#include "pit.h"
#include "serial.h"
#include "acpi.h"
#include "lapic.h"

static inline void mb_full(void) { __asm__ __volatile__("mfence" ::: "memory"); }
static inline void mb_wmb (void) { __asm__ __volatile__("sfence" ::: "memory"); }

// MMIO-доступ строго через volatile-указатели + барьеры. Все обёртки
// принудительно делают mfence до и после, чтобы исключить переупорядочивание
// и устранить stale-read'ы при последовательных записях queue_select/чтениях
// queue_size, queue_notify_off (наблюдалось на QEMU 7.x: запись select без
// барьера приводила к чтению "0xFFFF" вместо реального queue_size).
static inline uint8_t  mmio_r8 (volatile uint8_t*  p) { mb_full(); uint8_t  v = *p; mb_full(); return v; }
static inline uint16_t mmio_r16(volatile uint16_t* p) { mb_full(); uint16_t v = *p; mb_full(); return v; }
static inline uint32_t mmio_r32(volatile uint32_t* p) { mb_full(); uint32_t v = *p; mb_full(); return v; }
static inline void     mmio_w8 (volatile uint8_t*  p, uint8_t  v) { mb_full(); *p = v; mb_full(); }
static inline void     mmio_w16(volatile uint16_t* p, uint16_t v) { mb_full(); *p = v; mb_full(); }
static inline void     mmio_w32(volatile uint32_t* p, uint32_t v) { mb_full(); *p = v; mb_full(); }
static inline void     mmio_w64(volatile uint64_t* p, uint64_t v) { mb_full(); *p = v; mb_full(); }

// ----------------------------------------------------------------------------
// Прямой доступ по смещениям к virtio_pci_common_cfg.
// Обращение по `&packed->member` даёт компилятору повод оптимизировать
// (наблюдалось: queue_size читался stale). Поэтому строго через смещения.
// Layout (4.1.4.3): см. virtio.h struct virtio_pci_common_cfg_t.
// ----------------------------------------------------------------------------
#define VPCI_DFEAT_SEL          0x00   // device_feature_select   (uint32)
#define VPCI_DFEAT              0x04   // device_feature          (uint32)
#define VPCI_DRVFEAT_SEL        0x08
#define VPCI_DRVFEAT            0x0C
#define VPCI_MSIX_CONFIG        0x10
#define VPCI_NUM_QUEUES         0x12
#define VPCI_DEV_STATUS         0x14
#define VPCI_CONFIG_GEN         0x15
#define VPCI_QSEL               0x16
#define VPCI_QSIZE              0x18
#define VPCI_Q_MSIX             0x1A
#define VPCI_Q_ENABLE           0x1C
#define VPCI_Q_NOTIFY_OFF       0x1E
#define VPCI_QDESC              0x20
#define VPCI_QDRIVER            0x28   // avail
#define VPCI_QDEVICE            0x30   // used

static inline volatile uint8_t*  cfg8 (virtio_dev_t* d, uint16_t off) {
    return (volatile uint8_t*)((volatile uint8_t*)d->cfg + off);
}
static inline volatile uint16_t* cfg16(virtio_dev_t* d, uint16_t off) {
    return (volatile uint16_t*)((volatile uint8_t*)d->cfg + off);
}
static inline volatile uint32_t* cfg32(virtio_dev_t* d, uint16_t off) {
    return (volatile uint32_t*)((volatile uint8_t*)d->cfg + off);
}
static inline volatile uint64_t* cfg64(virtio_dev_t* d, uint16_t off) {
    return (volatile uint64_t*)((volatile uint8_t*)d->cfg + off);
}
// ============================================================================
// MMIO mapping: BAR-страницы из boot.asm initial-mapping все маркированы как
// WB cached (потому что 2-MB pages без PCD/PWT). Для virtio это смертельно:
// записи теряются в L1, чтения возвращают stale-данные. На устройство нужны
// uncached страницы (PG_PCD|PG_PWT). Перемапим окно 4 KB-страницами с UC.
// После — WBINVD, чтобы сбросить уже закэшированные строки.
// ============================================================================
// Полный сброс TLB: CR3 reload. Нужен, потому что некоторые CPU (особенно
// при demote'е 2-MB → 4-KB) держат в TLB старый super-page entry, и обычный
// invlpg по одному 4-KB-адресу не валит супер-страничный кэш.
static inline void tlb_flush_all(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static void map_mmio_uc(uint64_t phys_base, uint64_t length) {
    uint64_t* pml4 = paging_kernel_pml4();
    uint64_t start = phys_base & ~PAGE_MASK;
    uint64_t end   = (phys_base + length + PAGE_MASK) & ~PAGE_MASK;
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        if (!paging_map_page_in(pml4, p, p,
                                PG_PRESENT | PG_WRITE | PG_PCD | PG_PWT)) {
            serial_puts("[virtio] WARN: map_mmio_uc failed @ 0x");
            serial_print_hex(p, 16); serial_puts("\n");
        }
        paging_invlpg(p);
    }
    // Сбрасываем все кэш-строки CPU. Без этого записи в страницы, которые
    // были WB до перемапа, продолжают сидеть в L1/L2 и не доходят до MMIO.
    __asm__ __volatile__("wbinvd" ::: "memory");
    // Полная инвалидация TLB — invlpg может не сбросить кэш super-page TLB
    // entry'я, оставшийся от 2-MB-маппинга до demote'а. CR3-reload гарантирует
    // что все будущие walks пройдут через новые 4-KB PTE.
    tlb_flush_all();
}


// ============================================================================
// Modern detection: ходим по capability list, ищем 4 vendor-specific cap'а.
// ============================================================================
static bool parse_modern_caps(virtio_dev_t* d) {
    uint8_t off = pci_find_capability(&d->pci, PCI_CAP_ID_VENDOR);
    while (off) {
        // Прочитаем cap-структуру по 4-байтовым словам.
        uint32_t w0 = pci_config_read32(d->pci.bus, d->pci.device, d->pci.function, off);
        uint32_t w1 = pci_config_read32(d->pci.bus, d->pci.device, d->pci.function, (uint8_t)(off + 4));
        uint32_t w2 = pci_config_read32(d->pci.bus, d->pci.device, d->pci.function, (uint8_t)(off + 8));
        uint32_t w3 = pci_config_read32(d->pci.bus, d->pci.device, d->pci.function, (uint8_t)(off + 12));
        // w0: cap_vndr | cap_next | cap_len | cfg_type
        uint8_t cfg_type = (uint8_t)((w0 >> 24) & 0xFF);
        // w1: bar | padding[3]
        uint8_t bar      = (uint8_t)(w1 & 0xFF);
        // w2: offset
        uint32_t cap_off = w2;
        // w3: length
        uint32_t cap_len = w3;

        if (bar > 5) goto next;
        uint64_t bar_base = pci_read_bar64(&d->pci, bar);
        if (bar_base == 0) goto next;

        // Перемапить регион capability'и как UC. Без этого записи в MMIO
        // теряются в L1 (boot.asm создал 2-MB WB-mapping для всего 0..4 GB).
        map_mmio_uc(bar_base + cap_off, cap_len ? cap_len : 0x1000);

        volatile uint8_t* mmio = (volatile uint8_t*)(uintptr_t)(bar_base + cap_off);

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            d->cfg = (volatile virtio_pci_common_cfg_t*)mmio;
            serial_puts("[virtio] common_cfg @ 0x"); serial_print_hex((uint64_t)(uintptr_t)mmio, 16); serial_puts("\n");
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG: {
            d->notify_base = mmio;
            // notify_off_multiplier лежит сразу за virtio_pci_cap (offset 16).
            uint32_t mult = pci_config_read32(d->pci.bus, d->pci.device, d->pci.function,
                                              (uint8_t)(off + 16));
            d->notify_off_multiplier = mult;
            serial_puts("[virtio] notify_cfg @ 0x"); serial_print_hex((uint64_t)(uintptr_t)mmio, 16);
            serial_puts(", mult=");                serial_print_dec(mult);
            serial_puts("\n");
            break;
        }
        case VIRTIO_PCI_CAP_ISR_CFG:
            d->isr = mmio;
            serial_puts("[virtio] isr_cfg    @ 0x"); serial_print_hex((uint64_t)(uintptr_t)mmio, 16); serial_puts("\n");
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            d->device_cfg = mmio;
            serial_puts("[virtio] device_cfg @ 0x"); serial_print_hex((uint64_t)(uintptr_t)mmio, 16); serial_puts("\n");
            break;
        default:
            break;
        }

next:
        off = pci_next_capability(&d->pci, off, PCI_CAP_ID_VENDOR);
    }
    return d->cfg != 0 && d->notify_base != 0 && d->isr != 0;
}

// ============================================================================
// Probe — заполняет dev через указатель (НЕ возвращает by-value, см. virtio.h).
// ============================================================================
void virtio_pci_probe(virtio_dev_t* dev, uint16_t device_id) {
    virtio_pci_probe_nth(dev, device_id, 0);
}

void virtio_pci_probe_nth(virtio_dev_t* dev, uint16_t device_id, int n) {
    if (!dev) return;

    // Явный zero-init (volatile-поля не любят {0}).
    dev->present              = false;
    dev->is_modern            = false;
    dev->features             = 0;
    dev->cfg                  = 0;
    dev->isr                  = 0;
    dev->notify_base          = 0;
    dev->notify_off_multiplier= 0;
    dev->device_cfg           = 0;
    dev->io_base              = 0;
    dev->msix_on              = false;
    dev->msix_count           = 0;
    dev->msix_table           = 0;

    dev->pci = pci_find_device_nth(VIRTIO_PCI_VENDOR_ID, device_id, n);
    if (!dev->pci.found) {
        // Отсутствие второго/третьего экземпляра — норма, не шумим.
        if (n == 0) {
            serial_puts("[virtio] device 0x"); serial_print_hex(device_id, 4); serial_puts(" NOT found\n");
        }
        return;
    }

    serial_puts("[virtio] found virtio device id=0x"); serial_print_hex(device_id, 4);
    serial_puts(" at bdf="); serial_print_dec(dev->pci.bus); serial_puts("/");
    serial_print_dec(dev->pci.device); serial_puts("/"); serial_print_dec(dev->pci.function);
    serial_puts(" IRQ="); serial_print_dec(dev->pci.irq_line); serial_puts("\n");

    pci_enable_bus_master(&dev->pci);

    // Сначала пробуем modern (PCI capability list).
    if (parse_modern_caps(dev)) {
        dev->is_modern = true;
        dev->present   = true;
        serial_puts("[virtio] modern transport OK\n");
        return;
    }

    // Фолбэк на legacy IO BAR0.
    if (pci_bar_is_io(&dev->pci, 0)) {
        uint32_t bar0 = pci_read_bar(&dev->pci, 0);
        dev->io_base = (uint16_t)(bar0 & 0xFFFC);
        dev->is_modern = false;
        dev->present   = true;
        serial_puts("[virtio] legacy IO BAR0 @ 0x"); serial_print_hex(dev->io_base, 4); serial_puts("\n");
        return;
    }

    serial_puts("[virtio] no modern caps, no legacy IO BAR — unsupported\n");
}

// ============================================================================
// Status machine — единая для modern и legacy.
// ============================================================================
static void set_status(virtio_dev_t* d, uint8_t s) {
    if (d->is_modern) mmio_w8(cfg8(d, VPCI_DEV_STATUS), s);
    else              outb((uint16_t)(d->io_base + 0x12), s);
}
static uint8_t get_status(virtio_dev_t* d) {
    if (d->is_modern) return mmio_r8(cfg8(d, VPCI_DEV_STATUS));
    return inb((uint16_t)(d->io_base + 0x12));
}

bool virtio_pci_init_begin(virtio_dev_t* d) {
    if (!d || !d->present) return false;
    set_status(d, VIRTIO_STATUS_RESET);
    for (int i = 0; i < 1000 && get_status(d) != 0; i++) io_wait();
    set_status(d, VIRTIO_STATUS_ACKNOWLEDGE);
    set_status(d, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    return true;
}

uint64_t virtio_pci_negotiate(virtio_dev_t* d, uint64_t allowed) {
    if (!d || !d->present) return 0;
    if (d->is_modern) {
        // Читаем feature_low / feature_high через select.
        mmio_w32(cfg32(d, VPCI_DFEAT_SEL), 0);
        uint32_t feat_lo = mmio_r32(cfg32(d, VPCI_DFEAT));
        mmio_w32(cfg32(d, VPCI_DFEAT_SEL), 1);
        uint32_t feat_hi = mmio_r32(cfg32(d, VPCI_DFEAT));
        uint64_t dev_feat = ((uint64_t)feat_hi << 32) | feat_lo;

        // Modern требует VIRTIO_F_VERSION_1 — bit 32.
        uint64_t want = (allowed | (1ULL << VIRTIO_F_VERSION_1)) & dev_feat;

        mmio_w32(cfg32(d, VPCI_DRVFEAT_SEL), 0);
        mmio_w32(cfg32(d, VPCI_DRVFEAT), (uint32_t)(want & 0xFFFFFFFF));
        mmio_w32(cfg32(d, VPCI_DRVFEAT_SEL), 1);
        mmio_w32(cfg32(d, VPCI_DRVFEAT), (uint32_t)(want >> 32));

        d->features = want;

        // FEATURES_OK handshake.
        set_status(d, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        if (!(get_status(d) & VIRTIO_STATUS_FEATURES_OK)) {
            serial_puts("[virtio] FEATURES_OK rejected by device\n");
            return 0;
        }

        serial_puts("[virtio] features dev=0x"); serial_print_hex(dev_feat, 16);
        serial_puts(" allowed=0x"); serial_print_hex(allowed, 16);
        serial_puts(" negotiated=0x"); serial_print_hex(want, 16); serial_puts("\n");
        return want;
    } else {
        uint32_t dev_feat = inl((uint16_t)(d->io_base + 0x00));
        uint32_t neg = dev_feat & (uint32_t)allowed;
        outl((uint16_t)(d->io_base + 0x04), neg);
        d->features = neg;
        return neg;
    }
}

void virtio_pci_init_end(virtio_dev_t* d) {
    if (!d || !d->present) return;
    set_status(d, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

// Device-specific config reads. На modern это MMIO в device_cfg-окно.
uint8_t virtio_dev_cfg_read8(virtio_dev_t* d, uint16_t off) {
    if (d->is_modern) return d->device_cfg ? mmio_r8 (d->device_cfg + off) : 0;
    return inb((uint16_t)(d->io_base + 0x14 + off));
}
uint16_t virtio_dev_cfg_read16(virtio_dev_t* d, uint16_t off) {
    if (d->is_modern) return d->device_cfg ? mmio_r16((volatile uint16_t*)(d->device_cfg + off)) : 0;
    return inw((uint16_t)(d->io_base + 0x14 + off));
}
uint32_t virtio_dev_cfg_read32(virtio_dev_t* d, uint16_t off) {
    if (d->is_modern) return d->device_cfg ? mmio_r32((volatile uint32_t*)(d->device_cfg + off)) : 0;
    return inl((uint16_t)(d->io_base + 0x14 + off));
}

void virtio_dev_cfg_write8(virtio_dev_t* d, uint16_t off, uint8_t v) {
    if (d->is_modern) { if (d->device_cfg) mmio_w8(d->device_cfg + off, v); }
    else outb((uint16_t)(d->io_base + 0x14 + off), v);
}

// ============================================================================
// [41.2] MSI-X. Почему не INTx: линия INTx идёт через PIC/IOAPIC, и чтобы
// знать её GSI в режиме APIC, нужно разбирать ACPI _PRT (AML) — у нас этого
// нет. MSI-X пишет вектор прямо в LAPIC: адрес 0xFEE00000 | (apic_id << 12),
// данные = номер вектора. Таблица векторов лежит в BAR устройства
// (capability 0x11: +2 message control, +4 table offset | BIR).
// ============================================================================
bool virtio_pci_msix_setup(virtio_dev_t* d) {
    if (!d || !d->present || !d->is_modern) return false;
    uint8_t off = pci_find_capability(&d->pci, PCI_CAP_ID_MSIX);
    if (!off) return false;
    uint8_t b = d->pci.bus, dv = d->pci.device, f = d->pci.function;
    uint16_t ctrl  = pci_config_read16(b, dv, f, (uint8_t)(off + 2));
    uint32_t tbl   = pci_config_read32(b, dv, f, (uint8_t)(off + 4));
    uint16_t count = (uint16_t)((ctrl & 0x7FF) + 1);
    uint64_t bar   = pci_read_bar64(&d->pci, (int)(tbl & 7));
    if (!bar) return false;
    uint64_t table = bar + (tbl & ~7u);
    map_mmio_uc(table, (uint64_t)count * 16);
    d->msix_table = (volatile uint32_t*)(uintptr_t)table;
    d->msix_count = count;
    // Сначала замаскировать все записи, потом включить MSI-X: до настройки
    // маршрутов устройство не должно никуда стрелять.
    for (uint16_t i = 0; i < count; i++) mmio_w32(&d->msix_table[i * 4 + 3], 1);
    pci_config_write16(b, dv, f, (uint8_t)(off + 2), (uint16_t)((ctrl | 0x8000) & ~0x4000));
    uint16_t cmd = pci_config_read16(b, dv, f, PCI_COMMAND);
    pci_config_write16(b, dv, f, PCI_COMMAND, (uint16_t)(cmd | PCI_CMD_INTX_DISABLE));
    d->msix_on = true;
    // Вектор изменения конфигурации не используем (NO_VECTOR): события
    // конфигурации драйверы читают сами, когда им нужно.
    mmio_w16(cfg16(d, VPCI_MSIX_CONFIG), VIRTIO_MSI_NO_VECTOR);
    serial_puts("[virtio] MSI-X enabled, table entries=");
    serial_print_dec(count); serial_puts("\n");
    return true;
}

bool virtio_pci_msix_route(virtio_dev_t* d, uint16_t entry, int_handler_t h) {
    if (!d || !d->msix_on || entry >= d->msix_count) return false;
    uint8_t vec = irq_alloc_vector(h);
    if (!vec) {
        serial_puts("[virtio] MSI-X: no free interrupt vectors\n");
        return false;
    }
    volatile uint32_t* e = &d->msix_table[entry * 4];
    // Получатель — LAPIC текущего ядра (инициализация идёт на BSP). Берём id
    // из самого LAPIC, а не из ACPI: virtio-blk поднимается раньше acpi_init.
    mmio_w32(&e[0], 0xFEE00000u | (lapic_get_id() << 12));
    mmio_w32(&e[1], 0);
    mmio_w32(&e[2], vec);                 // edge, fixed delivery
    mmio_w32(&e[3], 0);                   // unmask
    serial_puts("[virtio] MSI-X entry "); serial_print_dec(entry);
    serial_puts(" -> vector ");           serial_print_dec(vec); serial_puts("\n");
    return true;
}

uint8_t virtio_pci_read_isr(virtio_dev_t* d) {
    if (d->is_modern) return d->isr ? mmio_r8(d->isr) : 0;
    return inb((uint16_t)(d->io_base + 0x13));
}

// ============================================================================
// virtq init/setup.
// ============================================================================
static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

bool virtq_init(virtio_dev_t* d, virtq_t* vq, uint16_t index, uint16_t num_request) {
    return virtq_init_irq(d, vq, index, num_request, VIRTIO_MSI_NO_VECTOR);
}

bool virtq_init_irq(virtio_dev_t* d, virtq_t* vq, uint16_t index, uint16_t num_request,
                    uint16_t msix_entry) {
    if (!d || !d->present || !vq) return false;

    uint16_t qsize = 0;
    if (d->is_modern) {
        mmio_w16(cfg16(d, VPCI_QSEL), index);
        qsize = mmio_r16(cfg16(d, VPCI_QSIZE));
        // Защита: на edge-cases queue_size возвращает мусор для несуществующих
        // queue. virtio 1.0 запрещает queue_size > 32768 (степень двойки до 2^15).
        if (qsize == 0 || qsize > 32768) qsize = 0;
    } else {
        outw((uint16_t)(d->io_base + 0x0E), index);
        qsize = inw((uint16_t)(d->io_base + 0x0C));
    }
    if (qsize == 0) {
        serial_puts("[virtq] queue "); serial_print_dec(index); serial_puts(" not present\n");
        return false;
    }
    uint16_t num = (num_request > 0 && num_request < qsize) ? num_request : qsize;

    // Modern: avail и used не обязательно непрерывные с desc, но мы держим
    // их в одном blob'е (выравнивание: desc 16, avail 2, used 4 — все
    // страничные, чтобы было проще).
    uint64_t desc_size  = (uint64_t)num * sizeof(virtq_desc_t);
    uint64_t avail_size = 6 + 2 * (uint64_t)num + 2;
    uint64_t used_size  = 6 + 8 * (uint64_t)num + 2;

    uint64_t avail_off = desc_size;                        // 16-aligned после desc
    uint64_t used_off  = align_up(avail_off + avail_size, 4096);
    uint64_t total     = used_off + align_up(used_size, 4096);

    uint8_t* mem = (uint8_t*)kmalloc_aligned((size_t)total, 4096);
    if (!mem) return false;
    for (uint64_t i = 0; i < total; i++) mem[i] = 0;

    vq->index         = index;
    vq->num           = num;
    vq->desc          = (virtq_desc_t*)mem;
    vq->avail         = (virtq_avail_t*)(mem + avail_off);
    vq->used          = (virtq_used_t*)(mem + used_off);
    vq->desc_phys     = virt_to_phys(mem);
    vq->avail_phys    = virt_to_phys((mem + avail_off));
    vq->used_phys     = virt_to_phys((mem + used_off));
    vq->backing       = mem;
    vq->free_head     = 0;
    vq->free_count    = num;
    vq->last_used_idx = 0;
    vq->notify_off    = 0;
    vq->irq           = false;

    for (uint16_t i = 0; i < num; i++) {
        vq->desc[i].addr  = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next  = (uint16_t)((i + 1) % num);
    }

    if (d->is_modern) {
        mmio_w16(cfg16(d, VPCI_QSEL), index);
        // Не пишем queue_size — host задал максимум, мы его уже прочитали выше
        // как `qsize` и используем `num <= qsize`. Запись меньшего значения
        // допустима, но необязательна; при некоторых версиях QEMU запись
        // вызывает reset очереди и теряет наши queue_desc/driver/device.
        mmio_w64(cfg64(d, VPCI_QDESC),    vq->desc_phys);
        mmio_w64(cfg64(d, VPCI_QDRIVER),  vq->avail_phys);
        mmio_w64(cfg64(d, VPCI_QDEVICE),  vq->used_phys);
        vq->notify_off = mmio_r16(cfg16(d, VPCI_Q_NOTIFY_OFF));
        // [41.2] Вектор очереди — строго ДО queue_enable. Устройство может
        // отказать (вернёт NO_VECTOR при чтении) — тогда очередь на опросе.
        if (d->msix_on) {
            mmio_w16(cfg16(d, VPCI_Q_MSIX), msix_entry);
            vq->irq = (msix_entry != VIRTIO_MSI_NO_VECTOR) &&
                      (mmio_r16(cfg16(d, VPCI_Q_MSIX)) == msix_entry);
        }
        mmio_w16(cfg16(d, VPCI_Q_ENABLE), 1);
    } else {
        if (vq->desc_phys >> 32) { kfree(mem); return false; }
        outw((uint16_t)(d->io_base + 0x0E), index);
        outl((uint16_t)(d->io_base + 0x08), (uint32_t)(vq->desc_phys >> 12));
    }

    serial_puts("[virtq] queue "); serial_print_dec(index);
    serial_puts(": num=");          serial_print_dec(num);
    serial_puts(", desc=0x");       serial_print_hex(vq->desc_phys, 16);
    serial_puts(", notify_off=");   serial_print_dec(vq->notify_off);
    serial_puts("\n");
    return true;
}

void virtq_free(virtq_t* vq) {
    if (vq && vq->backing) { kfree(vq->backing); vq->backing = 0; }
}

uint16_t virtq_alloc_chain(virtq_t* vq, uint16_t n) {
    if (!vq || n == 0 || n > vq->free_count) return 0xFFFF;
    uint16_t head = vq->free_head;
    uint16_t cur  = head;
    for (uint16_t i = 0; i + 1 < n; i++) cur = vq->desc[cur].next;
    vq->free_head  = vq->desc[cur].next;
    vq->free_count = (uint16_t)(vq->free_count - n);
    return head;
}

void virtq_free_chain(virtq_t* vq, uint16_t head) {
    if (!vq || head == 0xFFFF) return;
    uint16_t cur = head;
    uint16_t cnt = 1;
    while (vq->desc[cur].flags & VIRTQ_DESC_F_NEXT) {
        cur = vq->desc[cur].next;
        cnt++;
        if (cnt > vq->num) { serial_puts("[virtq] corrupt chain\n"); return; }
    }
    for (uint16_t i = head, k = 0; k < cnt; k++) {
        vq->desc[i].flags = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].addr  = 0;
        if (k + 1 < cnt) i = vq->desc[i].next;
    }
    vq->desc[cur].next = vq->free_head;
    vq->free_head      = head;
    vq->free_count     = (uint16_t)(vq->free_count + cnt);
}

void virtq_notify(virtio_dev_t* d, virtq_t* vq) {
    if (d->is_modern) {
        // Notification: пишем queue_index в notify_base + queue_notify_off * multiplier.
        uint64_t off = (uint64_t)vq->notify_off * (uint64_t)d->notify_off_multiplier;
        volatile uint16_t* slot = (volatile uint16_t*)(d->notify_base + off);
        mmio_w16(slot, vq->index);
    } else {
        outw((uint16_t)(d->io_base + 0x10), vq->index);
    }
}

void virtq_submit_kick(virtio_dev_t* d, virtq_t* vq, uint16_t head) {
    uint16_t pos = vq->avail->idx % vq->num;
    vq->avail->ring[pos] = head;
    mb_wmb();
    vq->avail->idx = (uint16_t)(vq->avail->idx + 1);
    mb_full();
    virtq_notify(d, vq);
}

// [41.2] Сколько оборотов busy-wait сделано в ожидании ответов устройства.
// Это прямой показатель цены опроса: каждый оборот — mfence + pause на BSP
// вместо сна до прерывания. Читает framestat.
static uint64_t g_wait_spins = 0;
uint64_t virtq_wait_spins(void) { return g_wait_spins; }

static inline bool irqs_enabled(void) {
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0" : "=r"(fl));
    return (fl & 0x200) != 0;
}

bool virtq_wait_used(virtq_t* vq, uint16_t head, uint64_t timeout_ticks,
                     uint32_t* out_len) {
    uint64_t t0 = pit_ticks();
    while (1) {
        mb_full();
        uint16_t cur = vq->used->idx;
        while (vq->last_used_idx != cur) {
            uint16_t pos = vq->last_used_idx % vq->num;
            virtq_used_elem_t e = vq->used->ring[pos];
            vq->last_used_idx = (uint16_t)(vq->last_used_idx + 1);
            if ((uint16_t)e.id == head) {
                if (out_len) *out_len = e.len;
                return true;
            }
        }
        if (pit_ticks() - t0 > timeout_ticks) {
            serial_puts("[virtq] wait_used TIMEOUT (head=");
            serial_print_dec(head); serial_puts(", q=");
            serial_print_dec(vq->index); serial_puts(")\n");
            return false;
        }
        // [41.2] Есть прерывание очереди и прерывания разрешены — спим до него
        // вместо busy-wait. Проверка used и hlt должны быть неразрывны: если
        // прерывание придёт между ними, hlt проспит до тика таймера (10 мс).
        // `sti; hlt` разрешает прерывания только после следующей команды,
        // поэтому: cli -> проверить -> sti;hlt.
        if (vq->irq && irqs_enabled()) {
            __asm__ __volatile__("cli");
            if (vq->used->idx == vq->last_used_idx)
                __asm__ __volatile__("sti; hlt" ::: "memory");
            else
                __asm__ __volatile__("sti");
            continue;
        }
        g_wait_spins++;
        __asm__ __volatile__("pause" ::: "memory");
    }
}

uint16_t virtq_used_count(const virtq_t* vq) {
    return vq ? (uint16_t)(vq->num - vq->free_count) : 0;
}
