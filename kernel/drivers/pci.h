// ============================================================================
// pci.h — минимальный PCI-сканер для MyOS
// ----------------------------------------------------------------------------
// PCI-устройства обнаруживаются путём последовательного опроса
// bus/device/function через порты CONFIG_ADDRESS (0xCF8) и CONFIG_DATA (0xCFC).
//
// Этот драйвер умеет ровно то, что нужно Этапам 24–28:
//   • найти устройство по vendor+device ID или по class+subclass;
//   • прочитать/записать конфигурационное пространство (32-бит слово);
//   • прочитать BAR'ы;
//   • включить Bus Master Enable (для DMA-устройств).
//
// 64-bit MMIO BAR'ы не поддерживаются — в QEMU AC97/RTL8139/E1000 их не
// используют (всё в IO space + 32-bit MMIO).
// ============================================================================

#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

// Порты Configuration Mechanism #1 (самый распространённый).
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Поля в Header Type 0x00.
#define PCI_VENDOR_ID       0x00  // 16 bit
#define PCI_DEVICE_ID       0x02  // 16 bit
#define PCI_COMMAND         0x04  // 16 bit
#define PCI_STATUS          0x06  // 16 bit
#define PCI_REVISION_ID     0x08  // 8  bit
#define PCI_PROG_IF         0x09  // 8  bit
#define PCI_SUBCLASS        0x0A  // 8  bit
#define PCI_CLASS           0x0B  // 8  bit
#define PCI_HEADER_TYPE     0x0E  // 8  bit
#define PCI_BAR0            0x10  // 32 bit
#define PCI_BAR1            0x14  // 32 bit
#define PCI_BAR2            0x18  // 32 bit
#define PCI_BAR3            0x1C  // 32 bit
#define PCI_BAR4            0x20  // 32 bit
#define PCI_BAR5            0x24  // 32 bit
#define PCI_INTERRUPT_LINE  0x3C  // 8  bit — номер IRQ на legacy PIC

// Флаги PCI_COMMAND.
#define PCI_CMD_IO_SPACE       (1 << 0)
#define PCI_CMD_MEMORY_SPACE   (1 << 1)
#define PCI_CMD_BUS_MASTER     (1 << 2)

// Описание найденного устройства. "found" = false, если поиск не удался.
typedef struct {
    bool     found;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
} pci_device_t;

// Низкоуровневый доступ к конфигурационному пространству (32-бит выравнивание).
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);

// Удобные обёртки для 8/16 бит.
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val);

// Поиск по vendor + device. Возвращает заполненный pci_device_t (found=false
// при неудаче). Сканирует все 256 bus × 32 dev × 8 fn — на учебной ОС это быстро.
pci_device_t pci_find_device(uint16_t vendor, uint16_t device);

// Поиск по class + subclass (удобно для "любая ethernet-карта", class=0x02).
pci_device_t pci_find_by_class(uint8_t class_code, uint8_t subclass);

// Читает BAR (0..5). Если это IO-BAR — возвращает base port, если MMIO —
// физический адрес. Младший бит/флаги замаскированы.
uint32_t pci_read_bar(const pci_device_t* dev, int bar_idx);

// true, если BAR — IO (младший бит в BAR = 1).
bool pci_bar_is_io(const pci_device_t* dev, int bar_idx);

// Включает Bus Master + IO/Memory Space в Command Register.
void pci_enable_bus_master(const pci_device_t* dev);

// Для отладки: печатает в serial-лог список всех устройств.
void pci_scan_dump(void);

#endif // PCI_H
