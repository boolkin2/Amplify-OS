// ============================================================================
// pci.c — реализация минимального PCI-сканера
// ============================================================================

#include "pci.h"
#include "../io.h"
#include "../debug_serial.h"

// ----------------------------------------------------------------------------
// Формирование 32-битного адреса для порта 0xCF8:
//   bit 31    = enable (всегда 1)
//   bit 30-24 = reserved (0)
//   bit 23-16 = bus
//   bit 15-11 = device
//   bit 10-8  = function
//   bit 7-2   = register offset (DWORD-выровнен)
//   bit 1-0   = 0
// ----------------------------------------------------------------------------
static inline uint32_t pci_make_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (uint32_t)0x80000000
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
    uint32_t dword = pci_config_read32(bus, dev, fn, off & 0xFC);
    return (uint16_t)((dword >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t dword = pci_config_read32(bus, dev, fn, off & 0xFC);
    return (uint8_t)((dword >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val) {
    // Read-modify-write в 32-битной гранулярности, чтобы не задеть соседнее слово.
    uint8_t  shift = (off & 2) * 8;
    uint32_t dword = pci_config_read32(bus, dev, fn, off & 0xFC);
    dword = (dword & ~((uint32_t)0xFFFF << shift)) | ((uint32_t)val << shift);
    pci_config_write32(bus, dev, fn, off & 0xFC, dword);
}

// ----------------------------------------------------------------------------
// Помощник: читает все нужные поля устройства в pci_device_t
// ----------------------------------------------------------------------------
static void fill_device(pci_device_t* out, uint8_t b, uint8_t d, uint8_t f) {
    out->found      = true;
    out->bus        = b;
    out->device     = d;
    out->function   = f;
    out->vendor     = pci_config_read16(b, d, f, PCI_VENDOR_ID);
    out->device_id  = pci_config_read16(b, d, f, PCI_DEVICE_ID);
    out->class_code = pci_config_read8 (b, d, f, PCI_CLASS);
    out->subclass   = pci_config_read8 (b, d, f, PCI_SUBCLASS);
    out->prog_if    = pci_config_read8 (b, d, f, PCI_PROG_IF);
    out->irq_line   = pci_config_read8 (b, d, f, PCI_INTERRUPT_LINE);
}

// ----------------------------------------------------------------------------
// Поиск устройств. Структура QEMU PCI-шины: один root bus, несколько dev.
// Полный перебор 256×32×8 = 65536 запросов — для учебной ОС мгновенно.
// ----------------------------------------------------------------------------
pci_device_t pci_find_device(uint16_t vendor, uint16_t device) {
    pci_device_t result = {0};
    for (uint32_t b = 0; b < 256; b++) {
        for (uint32_t d = 0; d < 32; d++) {
            for (uint32_t f = 0; f < 8; f++) {
                uint16_t v = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
                if (v == 0xFFFF) continue;  // Нет устройства на этой позиции
                uint16_t dv = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_DEVICE_ID);
                if (v == vendor && dv == device) {
                    fill_device(&result, (uint8_t)b, (uint8_t)d, (uint8_t)f);
                    return result;
                }
            }
        }
    }
    return result;  // found=false
}

pci_device_t pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    pci_device_t result = {0};
    for (uint32_t b = 0; b < 256; b++) {
        for (uint32_t d = 0; d < 32; d++) {
            for (uint32_t f = 0; f < 8; f++) {
                uint16_t v = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
                if (v == 0xFFFF) continue;
                uint8_t c  = pci_config_read8((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_CLASS);
                uint8_t sc = pci_config_read8((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_SUBCLASS);
                if (c == class_code && sc == subclass) {
                    fill_device(&result, (uint8_t)b, (uint8_t)d, (uint8_t)f);
                    return result;
                }
            }
        }
    }
    return result;
}

uint32_t pci_read_bar(const pci_device_t* dev, int bar_idx) {
    if (!dev || !dev->found || bar_idx < 0 || bar_idx > 5) return 0;
    uint8_t off = (uint8_t)(PCI_BAR0 + bar_idx * 4);
    uint32_t raw = pci_config_read32(dev->bus, dev->device, dev->function, off);
    if (raw & 1) {
        // IO space: маскируем младшие 2 бита.
        return raw & ~0x3u;
    }
    // Memory space: маскируем младшие 4 бита (info-биты).
    return raw & ~0xFu;
}

bool pci_bar_is_io(const pci_device_t* dev, int bar_idx) {
    if (!dev || !dev->found || bar_idx < 0 || bar_idx > 5) return false;
    uint8_t off = (uint8_t)(PCI_BAR0 + bar_idx * 4);
    uint32_t raw = pci_config_read32(dev->bus, dev->device, dev->function, off);
    return (raw & 1) != 0;
}

void pci_enable_bus_master(const pci_device_t* dev) {
    if (!dev || !dev->found) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_scan_dump(void) {
    dbg_puts("[PCI] scan begin\n");
    for (uint32_t b = 0; b < 256; b++) {
        for (uint32_t d = 0; d < 32; d++) {
            for (uint32_t f = 0; f < 8; f++) {
                uint16_t v = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
                if (v == 0xFFFF) continue;
                uint16_t dv = pci_config_read16((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_DEVICE_ID);
                uint8_t  cl = pci_config_read8 ((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_CLASS);
                uint8_t  sc = pci_config_read8 ((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_SUBCLASS);
                dbg_puts("  ");
                dbg_hex(((uint32_t)b<<16) | ((uint32_t)d<<8) | f);
                dbg_puts(" ven=");
                dbg_hex(v);
                dbg_puts(" dev=");
                dbg_hex(dv);
                dbg_puts(" cl=");
                dbg_hex(cl);
                dbg_puts(" sc=");
                dbg_hex(sc);
                dbg_puts("\n");
            }
        }
    }
    dbg_puts("[PCI] scan end\n");
}
