// ============================================================================
// pci.h — PCI Configuration Space scanner для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Configuration Mechanism #1 (порты 0xCF8 / 0xCFC) — единственный практически
// используемый на нашем железе. ECAM (MMIO config) добавим если/когда
// появится PCIe-устройство, требующее >256 байт config space.
// ============================================================================
#ifndef AOS_X86_64_PCI_H
#define AOS_X86_64_PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Поля Header Type 0x00.
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_CAP_PTR         0x34   // указатель на начало capability list
#define PCI_INTERRUPT_LINE  0x3C

// PCI_STATUS бит 4 = Capability List доступен.
#define PCI_STATUS_CAP_LIST 0x10

// PCI_COMMAND флаги.
#define PCI_CMD_IO_SPACE      (1u << 0)
#define PCI_CMD_MEMORY_SPACE  (1u << 1)
#define PCI_CMD_BUS_MASTER    (1u << 2)
#define PCI_CMD_INTX_DISABLE  (1u << 10)  // [41.2] при MSI-X линию INTx глушим
#define PCI_CAP_ID_MSIX       0x11

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

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val);

pci_device_t pci_find_device(uint16_t vendor, uint16_t device);
// [41.2] n-е устройство с такими vendor/device (0 — первое); found=false, если их меньше.
pci_device_t pci_find_device_nth(uint16_t vendor, uint16_t device, int n);
pci_device_t pci_find_by_class(uint8_t class_code, uint8_t subclass);

uint32_t pci_read_bar(const pci_device_t* dev, int bar_idx);
bool     pci_bar_is_io(const pci_device_t* dev, int bar_idx);

// Этап 41: 64-битный BAR (MMIO с PrefetchableType bit set).
// Возвращает физический адрес BAR-окна (для 64-bit MMIO BAR'а склеивает
// два 32-битных слота). 0 если BAR не доступен / не MMIO.
uint64_t pci_read_bar64(const pci_device_t* dev, int bar_idx);

// Этап 41: ходим по PCI capability list, ищем capability нужного ID.
// Возвращает offset в config space (8..255) либо 0.
uint8_t  pci_find_capability(const pci_device_t* dev, uint8_t cap_id);

// Этап 41: следующая capability того же ID (для virtio modern, у которой
// несколько VENDOR_SPECIFIC capabilities в списке).
uint8_t  pci_next_capability(const pci_device_t* dev, uint8_t cur_off, uint8_t cap_id);

void     pci_enable_bus_master(const pci_device_t* dev);
void     pci_scan_dump(void);

#endif
