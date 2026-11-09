// ============================================================================
// pci.c — PCI scanner (Configuration Mechanism #1).
// ----------------------------------------------------------------------------
// HOTFIX 19.05.2026 (KVM-nested): прежний код делал full-scan 256×32×8 BDF
// на КАЖДЫЙ вызов pci_find_device/pci_find_by_class. Под KVM каждый порт-IO
// = VMExit, на вложенной virt (KVM-внутри-VMware) каждый VMExit ~50-200 µs.
// 6 full-scan'ов × 65536 BDF × 2 port-IO = ~80 секунд только на PCI-init.
// Теперь: при первом pci_scan_dump() сохраняем все найденные устройства в
// кеш (g_pci_cache), а find_*() ищут в нём. Один скан вместо шести.
// ============================================================================
#include "pci.h"
#include "io.h"
#include "serial.h"

#define PCI_CACHE_MAX 64

static pci_device_t g_pci_cache[PCI_CACHE_MAX];
static int          g_pci_cache_count = 0;
static bool         g_pci_cache_valid = false;

static inline uint32_t pci_make_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn  & 0x07) << 8)
         | ((uint32_t)(off & 0xFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, pci_make_addr(bus, dev, fn, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDRESS, pci_make_addr(bus, dev, fn, off));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t d = pci_config_read32(bus, dev, fn, off & 0xFC);
    return (uint16_t)((d >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t d = pci_config_read32(bus, dev, fn, off & 0xFC);
    return (uint8_t)((d >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val) {
    uint8_t  shift = (off & 2) * 8;
    uint32_t d = pci_config_read32(bus, dev, fn, off & 0xFC);
    d = (d & ~((uint32_t)0xFFFF << shift)) | ((uint32_t)val << shift);
    pci_config_write32(bus, dev, fn, off & 0xFC, d);
}

static void fill_device(pci_device_t* o, uint8_t b, uint8_t d, uint8_t f) {
    o->found      = true;
    o->bus        = b;
    o->device     = d;
    o->function   = f;
    o->vendor     = pci_config_read16(b, d, f, PCI_VENDOR_ID);
    o->device_id  = pci_config_read16(b, d, f, PCI_DEVICE_ID);
    o->class_code = pci_config_read8 (b, d, f, PCI_CLASS);
    o->subclass   = pci_config_read8 (b, d, f, PCI_SUBCLASS);
    o->prog_if    = pci_config_read8 (b, d, f, PCI_PROG_IF);
    o->irq_line   = pci_config_read8 (b, d, f, PCI_INTERRUPT_LINE);
}

// ----------------------------------------------------------------------------
// Полный скан с заполнением кеша. Вызывается из pci_scan_dump() и при
// необходимости из pci_find_*() (если кеш ещё не построен). Если функция 0
// устройства отсутствует — пропускаем все 8 функций (Intel spec: function 0
// обязательно есть у любого реального устройства). Это даёт 8× меньше
// port-IO при пустых слотах.
// ----------------------------------------------------------------------------
static void pci_build_cache(void) {
    g_pci_cache_count = 0;
    for (uint32_t b = 0; b < 256; b++)
    for (uint32_t d = 0; d < 32; d++) {
        uint16_t v0 = pci_config_read16((uint8_t)b, (uint8_t)d, 0, PCI_VENDOR_ID);
        if (v0 == 0xFFFF) continue;                  // device absent, skip 8 fns
        // function 0 уже валидна, читаем header type — bit 7 = multi-function
        uint8_t  hdr = pci_config_read8((uint8_t)b, (uint8_t)d, 0, 0x0E);
        uint8_t  fmax = (hdr & 0x80) ? 8 : 1;
        for (uint32_t f = 0; f < fmax; f++) {
            uint16_t v = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
            if (v == 0xFFFF) continue;
            if (g_pci_cache_count >= PCI_CACHE_MAX) break;
            fill_device(&g_pci_cache[g_pci_cache_count++],
                        (uint8_t)b, (uint8_t)d, (uint8_t)f);
        }
    }
    g_pci_cache_valid = true;
}

static inline void pci_ensure_cache(void) {
    if (!g_pci_cache_valid) pci_build_cache();
}

pci_device_t pci_find_device(uint16_t vendor, uint16_t device) {
    pci_device_t r = {0};
    pci_ensure_cache();
    for (int i = 0; i < g_pci_cache_count; i++) {
        const pci_device_t* e = &g_pci_cache[i];
        if (e->vendor == vendor && e->device_id == device) { r = *e; return r; }
    }
    return r;
}

pci_device_t pci_find_device_nth(uint16_t vendor, uint16_t device, int n) {
    pci_device_t r = {0};
    pci_ensure_cache();
    for (int i = 0; i < g_pci_cache_count; i++) {
        const pci_device_t* e = &g_pci_cache[i];
        if (e->vendor != vendor || e->device_id != device) continue;
        if (n-- == 0) { r = *e; return r; }
    }
    return r;
}

pci_device_t pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    pci_device_t r = {0};
    pci_ensure_cache();
    for (int i = 0; i < g_pci_cache_count; i++) {
        const pci_device_t* e = &g_pci_cache[i];
        if (e->class_code == class_code && e->subclass == subclass) { r = *e; return r; }
    }
    return r;
}

uint32_t pci_read_bar(const pci_device_t* dev, int bar_idx) {
    if (!dev || !dev->found || bar_idx < 0 || bar_idx > 5) return 0;
    uint8_t off = (uint8_t)(PCI_BAR0 + bar_idx * 4);
    uint32_t raw = pci_config_read32(dev->bus, dev->device, dev->function, off);
    return (raw & 1) ? (raw & ~0x3u) : (raw & ~0xFu);
}

bool pci_bar_is_io(const pci_device_t* dev, int bar_idx) {
    if (!dev || !dev->found || bar_idx < 0 || bar_idx > 5) return false;
    uint8_t off = (uint8_t)(PCI_BAR0 + bar_idx * 4);
    uint32_t raw = pci_config_read32(dev->bus, dev->device, dev->function, off);
    return (raw & 1) != 0;
}

// ----------------------------------------------------------------------------
// Этап 41: 64-bit BAR. Биты [2:1] BAR-низа = type:
//   00 = 32-bit, 10 = 64-bit (тогда верх в следующем BAR-слоте), 01 = (зарезерв.)
// Bit 0: 0=MMIO, 1=IO. Bit 3 — prefetchable.
// ----------------------------------------------------------------------------
uint64_t pci_read_bar64(const pci_device_t* dev, int bar_idx) {
    if (!dev || !dev->found || bar_idx < 0 || bar_idx > 5) return 0;
    uint8_t  off = (uint8_t)(PCI_BAR0 + bar_idx * 4);
    uint32_t lo  = pci_config_read32(dev->bus, dev->device, dev->function, off);
    if (lo & 1) return 0;                          // IO BAR — не наш случай
    uint8_t  type = (uint8_t)((lo >> 1) & 0x3);
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if (type == 0x2 && bar_idx < 5) {              // 64-bit
        uint32_t hi = pci_config_read32(dev->bus, dev->device, dev->function,
                                        (uint8_t)(off + 4));
        base |= ((uint64_t)hi) << 32;
    }
    return base;
}

// ----------------------------------------------------------------------------
// PCI capability traversal: PCI_STATUS bit 4 → есть list, голова в PCI_CAP_PTR.
// Каждая capability: 0=cap_id, 1=next_off (terminating 0). Длина зависит от cap.
// ----------------------------------------------------------------------------
uint8_t pci_find_capability(const pci_device_t* dev, uint8_t cap_id) {
    if (!dev || !dev->found) return 0;
    uint16_t st = pci_config_read16(dev->bus, dev->device, dev->function, PCI_STATUS);
    if (!(st & PCI_STATUS_CAP_LIST)) return 0;
    uint8_t p = pci_config_read8(dev->bus, dev->device, dev->function, PCI_CAP_PTR) & 0xFCu;
    int guard = 64;                          // защита от закольцованного списка
    while (p && guard-- > 0) {
        uint8_t id   = pci_config_read8(dev->bus, dev->device, dev->function, p);
        uint8_t next = pci_config_read8(dev->bus, dev->device, dev->function,
                                        (uint8_t)(p + 1));
        if (id == cap_id) return p;
        p = next & 0xFCu;
    }
    return 0;
}

uint8_t pci_next_capability(const pci_device_t* dev, uint8_t cur_off, uint8_t cap_id) {
    if (!dev || !dev->found || cur_off == 0) return 0;
    uint8_t p = pci_config_read8(dev->bus, dev->device, dev->function,
                                 (uint8_t)(cur_off + 1)) & 0xFCu;
    int guard = 64;
    while (p && guard-- > 0) {
        uint8_t id   = pci_config_read8(dev->bus, dev->device, dev->function, p);
        uint8_t next = pci_config_read8(dev->bus, dev->device, dev->function,
                                        (uint8_t)(p + 1));
        if (id == cap_id) return p;
        p = next & 0xFCu;
    }
    return 0;
}

void pci_enable_bus_master(const pci_device_t* dev) {
    if (!dev || !dev->found) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_scan_dump(void) {
    serial_puts("[pci] scan begin\n");
    pci_ensure_cache();
    for (int i = 0; i < g_pci_cache_count; i++) {
        const pci_device_t* e = &g_pci_cache[i];
        serial_puts("  bdf=");
        serial_print_dec(e->bus); serial_puts("/");
        serial_print_dec(e->device); serial_puts("/");
        serial_print_dec(e->function);
        serial_puts(" vid=0x"); serial_print_hex(e->vendor, 4);
        serial_puts(" did=0x"); serial_print_hex(e->device_id, 4);
        serial_puts(" class=0x"); serial_print_hex(e->class_code, 2);
        serial_puts(" sub=0x"); serial_print_hex(e->subclass, 2);
        serial_puts("\n");
    }
    serial_puts("[pci] scan end (");
    serial_print_dec(g_pci_cache_count);
    serial_puts(" devices)\n");
}
