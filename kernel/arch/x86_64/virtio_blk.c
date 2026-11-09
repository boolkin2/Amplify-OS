// ============================================================================
// virtio_blk.c — драйвер virtio-blk (см. virtio_blk.h).
// ----------------------------------------------------------------------------
// Запрос (virtio 1.x §5.2.6) — цепочка из трёх дескрипторов:
//   [0] заголовок { le32 type; le32 reserved; le64 sector }  — читает устройство
//   [1] данные (count * 512 байт)   — читает (запись) или пишет (чтение) устройство
//   [2] байт статуса                — пишет устройство: 0 OK, 1 IOERR, 2 UNSUPP
// FLUSH — заголовок и статус, без данных.
//
// Буфер данных передаётся устройству КАК ЕСТЬ: память ядра отображена
// тождественно (и ниже, и выше 4 ГБ), поэтому гостевой физический адрес равен
// указателю, а блок, выделенный одним куском, непрерывен физически. Копии
// через bounce-буфер, как у AHCI, нет.
//
// Запросы строго по одному и синхронно: дисковый стек ядра однопоточный
// (CLAUDE.md §5.11). Ожидание ответа — virtq_wait_used: сон в hlt до
// прерывания MSI-X, либо опрос, если прерываний ещё/вообще нет.
// ============================================================================
#include "virtio_blk.h"
#include "memory.h"
#include "virtio.h"
#include "lapic.h"
#include "serial.h"

#define VBLK_T_IN      0        // чтение
#define VBLK_T_OUT     1        // запись
#define VBLK_T_FLUSH   4
#define VBLK_S_OK      0

#define VBLK_F_FLUSH   9        // устройство умеет FLUSH (кэш записи)
#define VBLK_F_RO      5

// Не больше 1 МиБ за запрос: размер одного сегмента у QEMU не ограничен, но
// так таймаут ожидания остаётся осмысленным и на медленном хранилище.
#define VBLK_MAX_SECTORS   2048
#define VBLK_TIMEOUT_TICKS 3000      // 30 с (тики PIT по 10 мс)

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} vblk_req_hdr_t;

typedef struct {
    bool         ready;
    bool         has_flush;
    bool         read_only;
    virtio_dev_t dev;
    virtq_t      q;
    uint64_t     sectors;
    volatile uint64_t irq_count;
    // Заголовок и статус — не на стеке: устройство пишет статус, когда хочет,
    // а кадр стека к тому времени мог бы уже смениться при таймауте.
    vblk_req_hdr_t hdr;
    volatile uint8_t status;
} vblk_t;

static vblk_t g_disk[VIRTIO_BLK_MAX_DISKS];
static int    g_ndisk = 0;

// Вектор у каждого диска свой, обработчик общий: он только будит BSP.
static void vblk_irq(registers64_t* r) {
    (void)r;
    for (int i = 0; i < g_ndisk; i++) g_disk[i].irq_count++;
    lapic_eoi();
}

int      virtio_blk_count(void) { return g_ndisk; }
uint64_t virtio_blk_sectors(int disk) {
    return (disk >= 0 && disk < g_ndisk && g_disk[disk].ready) ? g_disk[disk].sectors : 0;
}

static bool request(vblk_t* d, uint32_t type, uint64_t lba, uint32_t count, void* buf) {
    bool has_data = (type != VBLK_T_FLUSH);
    uint16_t n = has_data ? 3 : 2;
    uint16_t head = virtq_alloc_chain(&d->q, n);
    if (head == 0xFFFF) { serial_puts("[vblk] no free descriptors\n"); return false; }

    d->hdr.type = type; d->hdr.reserved = 0; d->hdr.sector = lba;
    d->status = 0xFF;

    virtq_desc_t* ds = d->q.desc;
    uint16_t i0 = head, i1 = ds[i0].next, i2 = has_data ? ds[i1].next : i1;
    ds[i0].addr = virt_to_phys(&d->hdr); ds[i0].len = sizeof(d->hdr);
    ds[i0].flags = VIRTQ_DESC_F_NEXT; ds[i0].next = i1;
    if (has_data) {
        ds[i1].addr = virt_to_phys(buf); ds[i1].len = count * 512u;
        ds[i1].flags = (uint16_t)(VIRTQ_DESC_F_NEXT | (type == VBLK_T_IN ? VIRTQ_DESC_F_WRITE : 0));
        ds[i1].next = i2;
    }
    ds[i2].addr = virt_to_phys((const void*)&d->status); ds[i2].len = 1;
    ds[i2].flags = VIRTQ_DESC_F_WRITE; ds[i2].next = 0;

    virtq_submit_kick(&d->dev, &d->q, head);
    bool ok = virtq_wait_used(&d->q, head, VBLK_TIMEOUT_TICKS, 0);
    virtq_free_chain(&d->q, head);
    if (!ok) { serial_puts("[vblk] request timeout\n"); return false; }
    if (d->status != VBLK_S_OK) {
        serial_puts("[vblk] request failed, status="); serial_print_dec(d->status);
        serial_puts(" lba="); serial_print_dec(lba); serial_puts("\n");
        return false;
    }
    return true;
}

static bool rw(int disk, uint64_t lba, uint32_t count, void* buf, bool write) {
    if (disk < 0 || disk >= g_ndisk || !buf || count == 0) return false;
    vblk_t* d = &g_disk[disk];
    if (!d->ready || lba + count > d->sectors) return false;
    if (write && d->read_only) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count > 0) {
        uint32_t n = count > VBLK_MAX_SECTORS ? VBLK_MAX_SECTORS : count;
        if (!request(d, write ? VBLK_T_OUT : VBLK_T_IN, lba, n, p)) return false;
        lba += n; p += (uint64_t)n * 512; count -= n;
    }
    return true;
}

bool virtio_blk_read (int disk, uint64_t lba, uint32_t count, void* buf)       { return rw(disk, lba, count, buf, false); }
bool virtio_blk_write(int disk, uint64_t lba, uint32_t count, const void* buf) { return rw(disk, lba, count, (void*)buf, true); }

bool virtio_blk_flush(int disk) {
    if (disk < 0 || disk >= g_ndisk || !g_disk[disk].ready) return false;
    if (!g_disk[disk].has_flush) return true;      // кэша записи нет — сбрасывать нечего
    return request(&g_disk[disk], VBLK_T_FLUSH, 0, 0, 0);
}

static bool init_one(vblk_t* d, uint16_t device_id, int nth) {
    virtio_pci_probe_nth(&d->dev, device_id, nth);
    if (!d->dev.present) return false;
    if (!d->dev.is_modern) {
        serial_puts("[vblk] device without modern capabilities - skipped\n");
        return false;
    }
    if (!virtio_pci_init_begin(&d->dev)) return false;
    uint64_t feat = virtio_pci_negotiate(&d->dev, (1ULL << VBLK_F_FLUSH) | (1ULL << VBLK_F_RO));
    d->has_flush = (feat >> VBLK_F_FLUSH) & 1;
    d->read_only = (feat >> VBLK_F_RO) & 1;

    // Запись 0 таблицы MSI-X — очередь запросов (она у нас одна).
    uint16_t entry = VIRTIO_MSI_NO_VECTOR;
    if (virtio_pci_msix_setup(&d->dev) && virtio_pci_msix_route(&d->dev, 0, vblk_irq)) entry = 0;
    if (!virtq_init_irq(&d->dev, &d->q, 0, 0, entry)) {
        serial_puts("[vblk] request queue init failed\n");
        return false;
    }
    virtio_pci_init_end(&d->dev);

    // capacity — первые 8 байт конфигурации, в секторах по 512 байт.
    d->sectors = (uint64_t)virtio_dev_cfg_read32(&d->dev, 0) |
                 ((uint64_t)virtio_dev_cfg_read32(&d->dev, 4) << 32);
    d->irq_count = 0;
    d->ready = d->sectors != 0;
    serial_puts("[vblk] disk "); serial_print_dec((uint64_t)(d - g_disk));
    serial_puts(": "); serial_print_dec(d->sectors / 2048); serial_puts(" MB");
    serial_puts(d->read_only ? ", read-only" : "");
    serial_puts(d->has_flush ? ", flush" : "");
    serial_puts(d->q.irq ? ", irq=MSI-X\n" : ", polled\n");
    return d->ready;
}

int virtio_blk_init(void) {
    g_ndisk = 0;
    // Сначала modern-only id (0x1042), затем переходный (0x1001): Proxmox
    // на i440fx создаёт переходное устройство, но modern-возможности у него
    // тоже есть — ими и пользуемся.
    static const uint16_t ids[2] = { VIRTIO_DEV_BLK_MODERN, VIRTIO_DEV_BLK_TRANS };
    for (int k = 0; k < 2; k++) {
        for (int nth = 0; nth < VIRTIO_BLK_MAX_DISKS && g_ndisk < VIRTIO_BLK_MAX_DISKS; nth++) {
            vblk_t* d = &g_disk[g_ndisk];
            d->ready = false;
            g_ndisk++;                               // обработчик смотрит g_disk[0..g_ndisk)
            if (!init_one(d, ids[k], nth)) {
                g_ndisk--;
                if (!d->dev.pci.found) break;        // устройств с этим id больше нет
            }
        }
    }
    return g_ndisk;
}

void virtio_blk_irq_selftest(void) {
    static uint8_t sector[512];
    for (int i = 0; i < g_ndisk; i++) {
        vblk_t* d = &g_disk[i];
        if (!d->ready || !d->q.irq) continue;
        // Один запрос на чтение: если счётчик прерываний не вырос, MSI-X до
        // нас не доходит и каждое ожидание кончалось бы по тику таймера.
        uint64_t before = d->irq_count;
        (void)request(d, VBLK_T_IN, 0, 1, sector);
        for (int k = 0; k < 2 && d->irq_count == before; k++) __asm__ __volatile__("sti; hlt");
        if (d->irq_count == before) {
            d->q.irq = false;
            serial_puts("[vblk] MSI-X interrupt is not delivered -> polling\n");
        } else {
            serial_puts("[vblk] completion by MSI-X interrupt: OK\n");
        }
    }
}
