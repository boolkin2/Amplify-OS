// ============================================================================
// ahci.c — драйвер SATA AHCI. Контракт и мотивация — в ahci.h.
// ----------------------------------------------------------------------------
// Регистры HBA (AHCI 1.3.1, §3): глобальные в начале ABAR, у каждого порта —
// окно 0x80 байт с 0x100. Для команды нужны три структуры в памяти:
//   * Command List (1 КБ, выравнивание 1 КБ) — 32 заголовка по 32 байта;
//   * Received FIS (256 байт, выравнивание 256) — сюда HBA кладёт ответы;
//   * Command Table (выравнивание 128) — FIS команды, ATAPI CDB, PRDT.
// Всё это для порта — одна страница 4 КБ: CLB @0, FIS @1024, CT @2048.
// Используем единственный слот 0 и опрос PxCI: ядро однопоточное на BSP,
// очередь (NCQ) и прерывания здесь ничего бы не дали.
//
// Данные идут через общий bounce-буфер 64 КБ из pmm: PRDT требует чётного
// физического адреса, а буфер вызывающего может быть любым (в т.ч. .bss).
// disk_io и так режет запросы на куски по 128 секторов = 64 КБ.
//
// Ядро и pmm отображены 1:1 (физический адрес == виртуальный), поэтому
// адреса структур пишутся в регистры HBA как есть. ABAR отображаем
// некэшируемым (PCD|PWT), как virtio.c — иначе записи в регистры могут
// застрять в кэше.
// ============================================================================
#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "memory.h"
#include "serial.h"
#include "timer.h"

// ---- глобальные регистры HBA ------------------------------------------------
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28
#define GHC_AE     (1u << 31)
#define CAP_SSS    (1u << 27)

// ---- регистры порта ---------------------------------------------------------
#define PORT_BASE(p) (0x100u + (uint32_t)(p) * 0x80u)
#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SERR   0x30
#define PX_CI     0x38

#define CMD_ST    (1u << 0)
#define CMD_SUD   (1u << 1)
#define CMD_POD   (1u << 2)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)
#define IS_TFES   (1u << 30)
#define TFD_BSY   0x80u
#define TFD_DRQ   0x08u
#define TFD_ERR   0x01u

#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u

#define BOUNCE_BYTES (64u * 1024u)

typedef struct {
    bool     used;
    int      port;
    bool     atapi;
    uint8_t* mem;          // страница: CLB @0, FIS @1024, CT @2048
} aport_t;

static volatile uint8_t* g_abar;
static aport_t           g_ports[AHCI_MAX_SLOTS];
static uint8_t*          g_bounce;
static bool              g_early = true;     // до sti uptime не растёт

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t*)(g_abar + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_abar + off) = v; }

static void bcopy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void bzero(void* d, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (uint32_t i = 0; i < n; i++) dd[i] = 0;
}

// Ожидание по часам, а до первого sti — по числу опросов (каждое чтение
// MMIO под KVM — VM-exit, ~1–2 мкс, поэтому 2 млн опросов ~ пара секунд).
static bool wait_reg(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeout_ms) {
    uint64_t start = timer_get_uptime_ms();
    for (uint32_t spin = 0; ; spin++) {
        if ((rd(off) & mask) == want) return true;
        if (g_early && spin > 2000000u) return false;
        if ((spin & 0xFF) == 0 && !g_early && timer_get_uptime_ms() - start > timeout_ms) return false;
        if (spin > 400000000u) return false;
    }
}

static void map_uc(uint64_t phys, uint64_t len) {
    uint64_t* pml4 = paging_kernel_pml4();
    uint64_t start = phys & ~PAGE_MASK;
    uint64_t end = (phys + len + PAGE_MASK) & ~PAGE_MASK;
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        paging_map_page_in(pml4, p, p, PG_PRESENT | PG_WRITE | PG_PCD | PG_PWT);
        paging_invlpg(p);
    }
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static void port_stop(uint32_t pb) {
    wr(pb + PX_CMD, rd(pb + PX_CMD) & ~CMD_ST);
    (void)wait_reg(pb + PX_CMD, CMD_CR, 0, 500);
    wr(pb + PX_CMD, rd(pb + PX_CMD) & ~CMD_FRE);
    (void)wait_reg(pb + PX_CMD, CMD_FR, 0, 500);
}

static void port_start(uint32_t pb) {
    (void)wait_reg(pb + PX_CMD, CMD_CR, 0, 500);
    wr(pb + PX_CMD, rd(pb + PX_CMD) | CMD_FRE);
    wr(pb + PX_CMD, rd(pb + PX_CMD) | CMD_ST);
}

// После ошибки (TFES) порт перестаёт принимать команды, пока его не
// перезапустить с очисткой SERR/IS (AHCI §6.2.2.1).
static void port_recover(aport_t* ap) {
    uint32_t pb = PORT_BASE(ap->port);
    port_stop(pb);
    wr(pb + PX_SERR, 0xFFFFFFFFu);
    wr(pb + PX_IS, 0xFFFFFFFFu);
    port_start(pb);
}

// Одна команда через слот 0. bytes — объём данных (через bounce-буфер);
// write — направление; cdb — ATAPI-пакет (12 байт) или 0.
static bool port_cmd(aport_t* ap, uint8_t command, uint64_t lba, uint32_t count,
                     uint32_t bytes, bool write, const uint8_t* cdb, uint32_t timeout_ms) {
    uint32_t pb = PORT_BASE(ap->port);
    if (!wait_reg(pb + PX_TFD, TFD_BSY | TFD_DRQ, 0, timeout_ms)) {
        port_recover(ap);
        if (!wait_reg(pb + PX_TFD, TFD_BSY | TFD_DRQ, 0, timeout_ms)) return false;
    }
    uint32_t* hdr = (uint32_t*)ap->mem;
    uint8_t*  ct  = ap->mem + 2048;

    uint32_t flags = 5u;                          // CFL: 5 dword = 20 байт FIS
    if (cdb) flags |= 1u << 5;                    // A: ATAPI
    if (write) flags |= 1u << 6;                  // W: память -> устройство
    hdr[0] = flags | ((bytes ? 1u : 0u) << 16);   // PRDTL
    hdr[1] = 0;                                   // PRDBC — заполнит HBA
    hdr[2] = (uint32_t)virt_to_phys(ct);
    hdr[3] = (uint32_t)(virt_to_phys(ct) >> 32);
    hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;

    bzero(ct, 0x80 + 16);
    ct[0] = 0x27;                                 // H2D Register FIS
    ct[1] = 0x80;                                 // C: это команда
    ct[2] = command;
    ct[3] = cdb ? 0x01 : 0x00;                    // FEATURES: ATAPI через DMA
    ct[4] = (uint8_t)lba;
    ct[5] = (uint8_t)(lba >> 8);
    ct[6] = (uint8_t)(lba >> 16);
    ct[7] = cdb ? 0x00 : 0x40;                    // DEVICE: LBA-адресация
    ct[8] = (uint8_t)(lba >> 24);
    ct[9] = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)count;
    ct[13] = (uint8_t)(count >> 8);
    if (cdb) bcopy(ct + 0x40, cdb, 12);
    if (bytes) {
        uint32_t* prd = (uint32_t*)(ct + 0x80);
        prd[0] = (uint32_t)virt_to_phys(g_bounce);
        prd[1] = (uint32_t)(virt_to_phys(g_bounce) >> 32);
        prd[2] = 0;
        prd[3] = bytes - 1;                       // DBC: байтов минус один
    }
    __asm__ __volatile__("" ::: "memory");       // структуры записаны до CI

    wr(pb + PX_IS, 0xFFFFFFFFu);
    wr(pb + PX_CI, 1u);

    uint64_t start = timer_get_uptime_ms();
    for (uint32_t spin = 0; ; spin++) {
        if (!(rd(pb + PX_CI) & 1u)) break;
        if (rd(pb + PX_IS) & IS_TFES) { port_recover(ap); return false; }
        if (g_early && spin > 20000000u) { port_recover(ap); return false; }
        if ((spin & 0xFF) == 0 && !g_early && timer_get_uptime_ms() - start > timeout_ms) {
            port_recover(ap);
            return false;
        }
    }
    if (rd(pb + PX_TFD) & TFD_ERR) { port_recover(ap); return false; }
    return true;
}

static void id_string(char* dst, const uint16_t* src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i * 2] = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = 0;
    int n = words * 2;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

static void print_dec(uint64_t v) { serial_print_dec(v); }

int ahci_probe(ata_info_t* out, int max) {
    if (max > AHCI_MAX_SLOTS) max = AHCI_MAX_SLOTS;
    for (int i = 0; i < max; i++) { out[i].present = false; out[i].atapi = false; }
    g_early = true;

    pci_device_t dev = pci_find_by_class(0x01, 0x06);
    if (!dev.found || dev.prog_if != 0x01) return 0;
    uint32_t bar5 = pci_read_bar(&dev, 5);
    uint64_t abar = bar5 & ~0xFULL;
    if (!abar || (bar5 & 1)) {
        serial_puts("[ahci] controller without an MMIO ABAR - skipped\n");
        return 0;
    }
    pci_enable_bus_master(&dev);
    map_uc(abar, 0x1100);
    g_abar = (volatile uint8_t*)(uintptr_t)abar;

    // BIOS/OS handoff: прошивка может держать контроллер за собой.
    if (rd(HBA_CAP2) & 1u) {
        wr(HBA_BOHC, rd(HBA_BOHC) | (1u << 1));
        (void)wait_reg(HBA_BOHC, 1u, 0, 100);
    }
    wr(HBA_GHC, rd(HBA_GHC) | GHC_AE);

    if (!g_bounce) {
        uint64_t phys = pmm_alloc_pages(BOUNCE_BYTES / PAGE_SIZE);
        if (!phys) { serial_puts("[ahci] no memory for the DMA buffer\n"); return 0; }
        g_bounce = (uint8_t*)(uintptr_t)phys;
    }

    uint32_t cap = rd(HBA_CAP), pi = rd(HBA_PI), vs = rd(HBA_VS);
    serial_puts("[ahci] controller ");
    serial_print_hex(dev.vendor, 4);
    serial_puts(":");
    serial_print_hex(dev.device_id, 4);
    serial_puts(" AHCI ");
    print_dec(vs >> 16);
    serial_puts(".");
    print_dec((vs >> 8) & 0xFF);
    serial_puts(" ports=0x");
    serial_print_hex(pi, 8);
    serial_puts(" abar=0x");
    serial_print_hex(abar, 8);
    serial_puts("\n");

    int found = 0;
    for (int p = 0; p < 32 && found < max; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t pb = PORT_BASE(p);
        uint32_t ssts = rd(pb + PX_SSTS);
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;   // нет устройства / не активно

        port_stop(pb);
        uint64_t mem = pmm_alloc_page();
        if (!mem) break;
        aport_t* ap = &g_ports[found];
        ap->used = true;
        ap->port = p;
        ap->mem = (uint8_t*)(uintptr_t)mem;
        bzero(ap->mem, PAGE_SIZE);
        wr(pb + PX_CLB, (uint32_t)mem);
        wr(pb + PX_CLBU, (uint32_t)(mem >> 32));
        wr(pb + PX_FB, (uint32_t)(mem + 1024));
        wr(pb + PX_FBU, (uint32_t)((mem + 1024) >> 32));
        wr(pb + PX_SERR, 0xFFFFFFFFu);
        wr(pb + PX_IS, 0xFFFFFFFFu);
        wr(pb + PX_IE, 0);                        // только опрос
        if (cap & CAP_SSS) wr(pb + PX_CMD, rd(pb + PX_CMD) | CMD_SUD | CMD_POD);
        port_start(pb);

        uint32_t sig = rd(pb + PX_SIG);
        if (sig != SIG_ATA && sig != SIG_ATAPI) { ap->used = false; continue; }
        ap->atapi = (sig == SIG_ATAPI);

        ata_info_t* d = &out[found];
        if (!port_cmd(ap, ap->atapi ? 0xA1 : 0xEC, 0, 0, 512, false, 0, 2000)) {
            serial_puts("[ahci] port ");
            print_dec((uint64_t)p);
            serial_puts(": IDENTIFY failed\n");
            ap->used = false;
            continue;
        }
        const uint16_t* id = (const uint16_t*)g_bounce;
        d->present = true;
        d->atapi = ap->atapi;
        d->lba_supported = true;
        d->lba48 = (id[83] & (1u << 10)) != 0;
        d->io_base = 0;
        d->ctrl_base = 0;
        d->slave = 0;
        d->kind = ATA_KIND_AHCI;
        d->port = (uint8_t)p;
        if (ap->atapi) d->total_sectors = 0;
        else {
            d->total_sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                               ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
            if (!d->lba48 || !d->total_sectors)
                d->total_sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
        }
        d->size_mb = d->total_sectors / 2048;
        id_string(d->model, &id[27], 20);
        id_string(d->serial, &id[10], 10);
        serial_puts("[ahci] port ");
        print_dec((uint64_t)p);
        serial_puts(": ");
        serial_puts(d->model);
        if (ap->atapi) serial_puts(" (ATAPI)\n");
        else {
            serial_puts(" - ");
            print_dec(d->size_mb);
            serial_puts(" MB\n");
        }
        found++;
    }
    g_early = false;
    return found;
}

static aport_t* slot_port(int slot) {
    if (slot < 0 || slot >= AHCI_MAX_SLOTS || !g_ports[slot].used) return 0;
    return &g_ports[slot];
}

bool ahci_read(int slot, uint64_t lba, uint32_t count, void* buf) {
    aport_t* ap = slot_port(slot);
    if (!ap || ap->atapi || count == 0 || count > BOUNCE_BYTES / 512) return false;
    g_early = false;
    if (!port_cmd(ap, 0x25, lba, count, count * 512u, false, 0, 10000)) return false;   // READ DMA EXT
    bcopy(buf, g_bounce, count * 512u);
    return true;
}

bool ahci_write(int slot, uint64_t lba, uint32_t count, const void* buf) {
    aport_t* ap = slot_port(slot);
    if (!ap || ap->atapi || count == 0 || count > BOUNCE_BYTES / 512) return false;
    g_early = false;
    bcopy(g_bounce, buf, count * 512u);
    return port_cmd(ap, 0x35, lba, count, count * 512u, true, 0, 10000);               // WRITE DMA EXT
}

bool ahci_flush(int slot) {
    aport_t* ap = slot_port(slot);
    if (!ap || ap->atapi) return false;
    g_early = false;
    return port_cmd(ap, 0xEA, 0, 0, 0, false, 0, 30000);                               // FLUSH CACHE EXT
}

bool ahci_atapi_read(int slot, uint32_t lba, uint32_t count, void* buf) {
    aport_t* ap = slot_port(slot);
    if (!ap || !ap->atapi || count == 0 || count > BOUNCE_BYTES / 2048) return false;
    g_early = false;
    uint8_t cdb[12] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                        (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0, 0, 0 };
    // Первая команда после сброса часто отвечает UNIT ATTENTION — повторяем.
    for (int attempt = 0; attempt < 3; attempt++) {
        if (port_cmd(ap, 0xA0, 0, 0, count * 2048u, false, cdb, 10000)) {
            bcopy(buf, g_bounce, count * 2048u);
            return true;
        }
    }
    return false;
}
