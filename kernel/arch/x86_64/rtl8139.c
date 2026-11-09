// ============================================================================
// rtl8139.c — Realtek RTL8139 driver, 64-битная адаптация.
// ----------------------------------------------------------------------------
// Отличия от i386:
//   * IRQ регистрируется через idt64_set_handler(32+irq, isr) — наш тонкий
//     ISR-stub передаст registers64_t* в C-обработчик
//   * адреса 64-битные, но RBSTART и TSAD — 32-битные регистры; буферы должны
//     лежать ниже 4 GB. Наш PMM пока работает только в первых 4 GB,
//     поэтому проблем нет
//   * перед регистрацией IRQ снимаем маску PIC: pic_clear_mask(irq) и (если
//     irq >= 8) pic_clear_mask(2) для каскада slave→master
// ============================================================================
#include "rtl8139.h"
#include "memory.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "idt.h"
#include "pic.h"
#include "ioapic.h"
#include "acpi.h"
#include "lapic.h"
#include "netif.h"
#include "ethernet.h"
#include "serial.h"

typedef struct {
    bool          ready;
    pci_device_t  pci;
    uint16_t      iobase;

    uint8_t*      rx_buffer;
    uint16_t      rx_offset;

    uint8_t*      tx_buffer[RTL_TX_BUF_COUNT];
    uint8_t       tx_current;

    uint8_t       mac[6];

    net_interface_t iface;
} rtl8139_state_t;

static rtl8139_state_t rtl = {0};

static inline uint8_t  rd8 (uint16_t off) { return inb (rtl.iobase + off); }
static inline uint16_t rd16(uint16_t off) { return inw (rtl.iobase + off); }
static inline uint32_t rd32(uint16_t off) { return inl (rtl.iobase + off); }
static inline void     wr8 (uint16_t off, uint8_t  v) { outb(rtl.iobase + off, v); }
static inline void     wr16(uint16_t off, uint16_t v) { outw(rtl.iobase + off, v); }
static inline void     wr32(uint16_t off, uint32_t v) { outl(rtl.iobase + off, v); }

static void   rtl8139_irq(registers64_t* r);
static void   rtl8139_process_rx(void);
static size_t rtl8139_send_impl(const void* data, size_t len);

bool           rtl8139_is_ready(void) { return rtl.ready; }
const uint8_t* rtl8139_get_mac(void)  { return rtl.ready ? rtl.mac : 0; }
size_t         rtl8139_send(const void* d, size_t l) { return rtl8139_send_impl(d, l); }

bool rtl8139_init(void) {
    serial_puts("[rtl8139] probing PCI...\n");
    rtl.pci = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!rtl.pci.found) { serial_puts("[rtl8139] not found\n"); return false; }

    serial_puts("[rtl8139] found bdf=");
    serial_print_dec(rtl.pci.bus); serial_puts("/");
    serial_print_dec(rtl.pci.device); serial_puts("/");
    serial_print_dec(rtl.pci.function);
    serial_puts(" IRQ="); serial_print_dec(rtl.pci.irq_line); serial_puts("\n");

    pci_enable_bus_master(&rtl.pci);

    uint32_t bar0 = pci_read_bar(&rtl.pci, 0);
    if (!pci_bar_is_io(&rtl.pci, 0)) { serial_puts("[rtl8139] BAR0 not IO\n"); return false; }
    rtl.iobase = (uint16_t)(bar0 & 0xFFFC);
    serial_puts("[rtl8139] iobase=0x"); serial_print_hex(rtl.iobase, 4); serial_puts("\n");

    // Power-on.
    wr8(RTL_REG_CONFIG1, 0x00);

    // Soft reset.
    wr8(RTL_REG_CR, RTL_CR_RST);
    for (int i = 0; i < 100000; i++) {
        if ((rd8(RTL_REG_CR) & RTL_CR_RST) == 0) break;
        io_wait();
    }
    if (rd8(RTL_REG_CR) & RTL_CR_RST) { serial_puts("[rtl8139] reset timeout\n"); return false; }

    // RX buffer.
    rtl.rx_buffer = (uint8_t*)kmalloc_aligned(RTL_RX_BUF_TOTAL, 16);
    if (!rtl.rx_buffer) { serial_puts("[rtl8139] OOM rx\n"); return false; }
    for (size_t i = 0; i < RTL_RX_BUF_TOTAL; i++) rtl.rx_buffer[i] = 0;
    rtl.rx_offset = 0;
    // RBSTART — 32-битный регистр. Наш PMM выдаёт страницы из < 4 GB.
    uint64_t rx_phys = virt_to_phys(rtl.rx_buffer);
    if (rx_phys >> 32) {
        serial_puts("[rtl8139] FATAL: rx buffer above 4 GB\n");
        return false;
    }
    wr32(RTL_REG_RBSTART, (uint32_t)rx_phys);

    // TX buffers.
    for (int i = 0; i < RTL_TX_BUF_COUNT; i++) {
        rtl.tx_buffer[i] = (uint8_t*)kmalloc_aligned(RTL_TX_BUF_SIZE, 16);
        if (!rtl.tx_buffer[i]) { serial_puts("[rtl8139] OOM tx\n"); return false; }
    }
    rtl.tx_current = 0;

    // IMR + RCR + TCR.
    wr16(RTL_REG_IMR, RTL_INT_ROK | RTL_INT_RER | RTL_INT_TOK | RTL_INT_TER | RTL_INT_RXOVW);
    wr32(RTL_REG_RCR, RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_AM
                    | RTL_RCR_MXDMA_1024 | RTL_RCR_RBLEN_8K);
    wr32(RTL_REG_TCR, RTL_TCR_IFG_STD | RTL_TCR_MXDMA_2048);

    // Enable RX/TX.
    wr8(RTL_REG_CR, RTL_CR_RE | RTL_CR_TE);

    // КРИТИЧНО: после reset все TSD = 0 (включая бит OWN).  Наш spin в
    // send_impl ждёт OWN=1 ("NIC закончил") — но для свежих дескрипторов
    // он никогда не выставится без первого TX.  Принудительно ставим OWN=1
    // на старте, чтобы первая запись каждого дескриптора не зависала.
    for (int i = 0; i < RTL_TX_BUF_COUNT; i++) {
        wr32((uint16_t)(RTL_REG_TSD0 + i * 4), RTL_TSD_OWN);
    }

    // MAC.
    for (int i = 0; i < 6; i++) rtl.mac[i] = rd8((uint16_t)(RTL_REG_MAC0 + i));
    serial_puts("[rtl8139] MAC="); netif_dump_mac(rtl.mac); serial_puts("\n");

    // IRQ. PCI Interrupt Line — номер legacy IRQ. На современной QEMU pc/q35
    // фактическая доставка идёт через IOAPIC (PIIX3 LNK# → IOAPIC GSI), а PIC
    // — лишь legacy fallback. Поэтому одновременно:
    //   1) unmask PIC mask (на случай если IOAPIC выключен)
    //   2) ioapic_route_irq() — главный путь
    uint8_t irq = rtl.pci.irq_line;
    if (irq < 16) {
        idt64_set_handler((uint8_t)(32 + irq), rtl8139_irq);

        // PIC fallback.
        pic_clear_mask(irq);
        if (irq >= 8) pic_clear_mask(2);

        // IOAPIC routing: GSI = irq, либо переопределение из MADT
        // INT_OVERRIDE (acpi_irq_to_gsi[]). vector = 32 + irq.
        if (ioapic_present()) {
            uint8_t gsi = acpi_irq_to_gsi[irq];
            ioapic_route_irq(gsi, (uint8_t)(32 + irq),
                             (uint8_t)acpi_bsp_lapic_id, false);
            serial_puts("[rtl8139] IOAPIC route: GSI ");
            serial_print_dec(gsi);
            serial_puts(" -> vector ");
            serial_print_dec(32 + irq);
            serial_puts(" -> LAPIC ");
            serial_print_dec(acpi_bsp_lapic_id);
            serial_puts("\n");
        }
        serial_puts("[rtl8139] IRQ "); serial_print_dec(irq); serial_puts(" registered\n");
    } else {
        serial_puts("[rtl8139] invalid IRQ line — skip\n");
    }

    // Регистрируем netif.
    const char* name = "eth0";
    for (int i = 0; i < NETIF_NAME_LEN && name[i]; i++) rtl.iface.name[i] = name[i];
    rtl.iface.name[NETIF_NAME_LEN - 1] = 0;
    for (int i = 0; i < NETIF_MAC_LEN; i++) rtl.iface.mac[i] = rtl.mac[i];
    rtl.iface.send = rtl8139_send_impl;
    rtl.iface.ip4_addr = 0;
    rtl.iface.ip4_netmask = 0;
    rtl.iface.ip4_gateway = 0;
    rtl.iface.up = false;
    netif_register(&rtl.iface);

    rtl.ready = true;
    serial_puts("[rtl8139] init OK\n");
    return true;
}

static void rtl8139_irq(registers64_t* r) {
    (void)r;
    uint16_t isr = rd16(RTL_REG_ISR);
    if (isr == 0) {
        // Spurious — всё равно квитируем EOI обоим контроллерам.
        lapic_eoi();
        pic_send_eoi(rtl.pci.irq_line);
        return;
    }
    wr16(RTL_REG_ISR, isr);   // write-1-to-clear

    if (isr & RTL_INT_ROK) rtl8139_process_rx();
    if (isr & RTL_INT_RER) serial_puts("[rtl8139] RX error\n");
    if (isr & RTL_INT_TER) serial_puts("[rtl8139] TX error\n");
    if (isr & RTL_INT_RXOVW) {
        serial_puts("[rtl8139] RX overflow — flush\n");
        rtl.rx_offset = rd16(RTL_REG_CBR) % RTL_RX_BUF_SIZE;
        wr16(RTL_REG_CAPR, (uint16_t)(rtl.rx_offset - 16));
    }

    // ВАЖНО: PCI IRQ роутится через IOAPIC → LAPIC, поэтому EOI идёт в LAPIC.
    // PIC EOI оставляем для совместимости с конфигами без IOAPIC.
    lapic_eoi();
    pic_send_eoi(rtl.pci.irq_line);
}

static void rtl8139_process_rx(void) {
    while (!(rd8(RTL_REG_CR) & RTL_CR_BUFE)) {
        uint8_t* p = rtl.rx_buffer + rtl.rx_offset;
        uint16_t hdr = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));

        if (!(hdr & 0x0001)) {
            serial_puts("[rtl8139] bad packet, reset RX\n");
            uint8_t cr = rd8(RTL_REG_CR);
            wr8(RTL_REG_CR, cr & ~RTL_CR_RE);
            wr8(RTL_REG_CR, cr);
            rtl.rx_offset = 0;
            wr16(RTL_REG_CAPR, 0xFFF0);
            return;
        }
        if (len >= 14 && len <= ETH_MAX_LEN + 4 + 4) {
            netif_receive(p + 4, (size_t)(len - 4));    // -4 = FCS
        }
        rtl.rx_offset = (uint16_t)((rtl.rx_offset + len + 4 + 3) & ~3u);
        if (rtl.rx_offset >= RTL_RX_BUF_SIZE) rtl.rx_offset -= RTL_RX_BUF_SIZE;
        wr16(RTL_REG_CAPR, (uint16_t)(rtl.rx_offset - 16));
    }
}

static size_t rtl8139_send_impl(const void* data, size_t len) {
    if (!rtl.ready || !data || len == 0 || len > RTL_TX_BUF_SIZE) return 0;

    uint8_t idx = rtl.tx_current;
    uint16_t tsd_off = (uint16_t)(RTL_REG_TSD0 + idx * 4);
    for (int i = 0; i < 1000000; i++) {
        if (rd32(tsd_off) & RTL_TSD_OWN) break;
        io_wait();
    }
    uint8_t* buf = rtl.tx_buffer[idx];
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) buf[i] = src[i];

    uint64_t buf_phys = virt_to_phys(buf);
    if (buf_phys >> 32) return 0;
    wr32((uint16_t)(RTL_REG_TSAD0 + idx * 4), (uint32_t)buf_phys);
    wr32(tsd_off, (uint32_t)(len & 0x1FFF));
    rtl.tx_current = (uint8_t)((rtl.tx_current + 1) & (RTL_TX_BUF_COUNT - 1));
    return len;
}
