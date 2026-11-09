// ============================================================================
// e1000.c — Intel 82540EM/82545EM Gigabit Ethernet driver (этап 47.8).
// ----------------------------------------------------------------------------
// Минимальный driver чтобы работала сеть в VMware Workstation / VirtualBox /
// QEMU `-device e1000` / на реальном Intel NIC.
//
// Архитектура:
//   1. PCI probe — ищем Intel vendor 0x8086 и один из supported device IDs.
//   2. BAR0 — MMIO для регистров (НЕ I/O ports, в отличие от RTL8139).
//      Первые 4 GB identity-mapped в boot.asm через 2MB-страницы, поэтому
//      адрес BAR0 (обычно 0xFEBC0000 на QEMU) уже доступен напрямую.
//   3. Reset: CTRL.RST → подождать → SLU + FD.
//   4. MAC: читаем RAL0/RAH0 — на reset'е чипа значение уже загружено
//      из EEPROM.
//   5. Multicast Table — обнулить (128 регистров × 32 бита).
//   6. RX: alloc 32 descriptor + 32 × 2048 byte буферов, заполнить addr,
//      RDBAL/RDBAH/RDLEN/RDH=0/RDT=count-1.  RCTL = EN + BAM + SECRC + 2K bufsize.
//   7. TX: alloc 32 descriptor + 32 × 2048 byte буферов, обнулить.
//      TDBAL/H/LEN, RDH=0, RDT=0.  TCTL = EN + PSP + CT(0x10) + COLD(0x40).
//      TIPG = legacy IEEE 802.3 values (10/8/6).
//   8. IRQ: установить handler через idt64_set_handler, route через IOAPIC.
//      IMS = RXT0 | RXDMT0 | LSC.
//   9. netif_register с send=e1000_send.
//
// TX path:
//   * Берём текущий tx_tail дескриптор.
//   * Копируем data в tx_buffers[tail].
//   * Заполняем addr, length, cmd = EOP|IFCS|RS.
//   * Двигаем TDT.
//   * Не блокируем — DD bit устанавливается асинхронно (по interrupt'у).
//
// RX path (в IRQ handler'е):
//   * Walk дескрипторы от текущего rx_tail вперёд, пока status.DD == 1.
//   * Каждый ready frame отдать в netif_receive.
//   * Сбросить status descriptor'а, продвинуть RDT (= tail).
// ============================================================================
#include "e1000.h"
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
#include "timer.h"

typedef struct {
    bool             ready;
    pci_device_t     pci;

    volatile uint8_t* mmio;     // BAR0 base, identity-mapped

    e1000_rx_desc_t* rx_ring;   // 32 entries, 16-byte aligned
    e1000_tx_desc_t* tx_ring;
    uint8_t**        rx_buffers; // массив указателей на 32 × 2048-байт буферы
    uint8_t**        tx_buffers;

    uint32_t         rx_tail;   // следующий desc, который мы будем читать
    uint32_t         tx_tail;   // следующий desc для записи
    uint32_t         tx_pristine_count;  // сколько ещё свежих TX-desc

    uint8_t          mac[6];

    net_interface_t  iface;
} e1000_state_t;

static e1000_state_t g_e1000 = {0};

// ---- MMIO accessors --------------------------------------------------------
static inline uint32_t e1000_rd32(uint32_t off) {
    return *(volatile uint32_t*)(g_e1000.mmio + off);
}
static inline void e1000_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_e1000.mmio + off) = v;
}

static void e1000_delay_us(int us) {
    // ~грубая задержка для PHY reset.  io_wait() = 1 µs на PIC IO port.
    for (int i = 0; i < us; i++) io_wait();
}

// ---- Forward declarations --------------------------------------------------
static void   e1000_irq(registers64_t* r);
static void   e1000_process_rx(void);
static size_t e1000_send_impl(const void* data, size_t len);
static bool   e1000_setup_rx(void);
static bool   e1000_setup_tx(void);

// ---- Public API ------------------------------------------------------------
bool           e1000_is_ready(void) { return g_e1000.ready; }
const uint8_t* e1000_get_mac(void)  { return g_e1000.ready ? g_e1000.mac : 0; }
size_t         e1000_send(const void* d, size_t l) { return e1000_send_impl(d, l); }
void           e1000_poll_rx(void)  { if (g_e1000.ready) e1000_process_rx(); }

// ---- Init ------------------------------------------------------------------
bool e1000_init(void) {
    serial_puts("[e1000] probing PCI...\n");

    // Ищем по очереди наши supported device IDs.  Хотим первое попавшееся.
    static const uint16_t supported[] = {
        E1000_DEV_82540EM,    // QEMU/VMware standard
        E1000_DEV_82545EM,
        E1000_DEV_82571,
        E1000_DEV_82574L,
        E1000_DEV_82577LM,
        E1000_DEV_I217,
    };
    pci_device_t dev = {0};
    for (size_t i = 0; i < sizeof(supported)/sizeof(supported[0]); i++) {
        dev = pci_find_device(E1000_VENDOR_ID, supported[i]);
        if (dev.found) {
            serial_puts("[e1000] found device 0x");
            serial_print_hex(supported[i], 4);
            serial_puts("\n");
            break;
        }
    }
    if (!dev.found) { serial_puts("[e1000] no supported Intel NIC found\n"); return false; }

    g_e1000.pci = dev;
    serial_puts("[e1000] bdf=");
    serial_print_dec(dev.bus); serial_puts("/");
    serial_print_dec(dev.device); serial_puts("/");
    serial_print_dec(dev.function);
    serial_puts(" IRQ="); serial_print_dec(dev.irq_line); serial_puts("\n");

    pci_enable_bus_master(&dev);

    // Снять PCI Interrupt Disable bit (bit 10 в Command Register).
    // При reset может быть = 1, блокирует INTx# доставку — IRQ ни разу
    // не выстрелит.  pci_enable_bus_master() не трогает этот бит.
    uint16_t pci_cmd = pci_config_read16(dev.bus, dev.device, dev.function, PCI_COMMAND);
    if (pci_cmd & (1u << 10)) {
        pci_cmd &= ~(1u << 10);
        pci_config_write16(dev.bus, dev.device, dev.function, PCI_COMMAND, pci_cmd);
        serial_puts("[e1000] cleared PCI INTx disable\n");
    }

    // BAR0 — 64-bit MMIO.  На QEMU обычно 0xFEBC0000-ish, на VMware похоже.
    uint64_t bar0 = pci_read_bar64(&dev, 0);
    if (!bar0) {
        // Fallback на 32-bit BAR (старые real Intel NICs).
        uint32_t bar0_32 = pci_read_bar(&dev, 0);
        if (pci_bar_is_io(&dev, 0) || bar0_32 == 0) {
            serial_puts("[e1000] BAR0 not MMIO or empty\n");
            return false;
        }
        bar0 = (uint64_t)(bar0_32 & 0xFFFFFFF0u);
    }
    serial_puts("[e1000] mmio bar0=0x"); serial_print_hex(bar0, 16); serial_puts("\n");

    if (bar0 >= (1ULL << 32)) {
        // boot.asm identity-мапит только первые 4 GB.  Для 64-bit MMIO нужно
        // дополнительно отобразить — пока не поддерживаем (на QEMU/VMware
        // MMIO всегда в нижних 4 GB).
        serial_puts("[e1000] FATAL: BAR0 above 4 GB (not supported)\n");
        return false;
    }
    g_e1000.mmio = (volatile uint8_t*)(uintptr_t)bar0;

    // ---- Soft reset ----
    // Disable interrupts first.
    e1000_wr32(E1000_REG_IMC, 0xFFFFFFFFu);

    // CTRL.RST = 1.  Чип должен очистить bit в течение нескольких µs.
    uint32_t ctrl = e1000_rd32(E1000_REG_CTRL);
    e1000_wr32(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    e1000_delay_us(20);
    // Wait until RST clears (max 1ms).
    int timeout = 1000;
    while ((e1000_rd32(E1000_REG_CTRL) & E1000_CTRL_RST) && timeout--) {
        e1000_delay_us(1);
    }
    if (timeout <= 0) {
        serial_puts("[e1000] reset timeout\n");
        return false;
    }

    // Disable interrupts again (reset мог их сбросить, но повторим).
    e1000_wr32(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_rd32(E1000_REG_ICR);   // clear pending

    // ---- Setup link ----
    ctrl = e1000_rd32(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_FD | E1000_CTRL_ASDE;
    ctrl &= ~(1u << 3);   // clear LRST (Link Reset)
    ctrl &= ~(1u << 7);   // clear ILOS (Invert Loss of Signal)
    ctrl &= ~(3u << 30);  // clear VME (VLAN mode) + PHY_RST
    e1000_wr32(E1000_REG_CTRL, ctrl);

    // ---- MAC ----
    // После reset RAL0/RAH0 уже содержат MAC, прочитанный из EEPROM.
    uint32_t ral = e1000_rd32(E1000_REG_RAL0);
    uint32_t rah = e1000_rd32(E1000_REG_RAH0);
    g_e1000.mac[0] = (uint8_t)(ral >> 0);
    g_e1000.mac[1] = (uint8_t)(ral >> 8);
    g_e1000.mac[2] = (uint8_t)(ral >> 16);
    g_e1000.mac[3] = (uint8_t)(ral >> 24);
    g_e1000.mac[4] = (uint8_t)(rah >> 0);
    g_e1000.mac[5] = (uint8_t)(rah >> 8);
    // Проверка sanity — все нули значит EEPROM не загрузил, плохо.
    {
        uint8_t z = 0;
        for (int i = 0; i < 6; i++) z |= g_e1000.mac[i];
        if (z == 0) {
            serial_puts("[e1000] WARN: MAC all zero, EEPROM not loaded\n");
            // Возьмём дефолтный QEMU/locally-administered.
            g_e1000.mac[0] = 0x52; g_e1000.mac[1] = 0x54; g_e1000.mac[2] = 0x00;
            g_e1000.mac[3] = 0x12; g_e1000.mac[4] = 0x34; g_e1000.mac[5] = 0x56;
            uint32_t l = ((uint32_t)g_e1000.mac[0]) |
                         ((uint32_t)g_e1000.mac[1] << 8) |
                         ((uint32_t)g_e1000.mac[2] << 16) |
                         ((uint32_t)g_e1000.mac[3] << 24);
            uint32_t h = ((uint32_t)g_e1000.mac[4]) |
                         ((uint32_t)g_e1000.mac[5] << 8) |
                         E1000_RAH_AV;
            e1000_wr32(E1000_REG_RAL0, l);
            e1000_wr32(E1000_REG_RAH0, h);
        } else {
            // Поставим Address Valid если ещё не стоит.
            e1000_wr32(E1000_REG_RAH0, rah | E1000_RAH_AV);
        }
    }
    serial_puts("[e1000] MAC="); netif_dump_mac(g_e1000.mac); serial_puts("\n");

    // ---- Clear Multicast Table Array (128 регистров × 32 бита) ----
    for (int i = 0; i < 128; i++) {
        e1000_wr32(E1000_REG_MTA_BASE + i * 4, 0);
    }

    // ---- RX setup ----
    if (!e1000_setup_rx()) {
        serial_puts("[e1000] RX setup failed\n");
        return false;
    }
    // ---- TX setup ----
    if (!e1000_setup_tx()) {
        serial_puts("[e1000] TX setup failed\n");
        return false;
    }

    // ---- IRQ ----
    uint8_t irq = dev.irq_line;
    if (irq < 16) {
        idt64_set_handler((uint8_t)(32 + irq), e1000_irq);
        pic_clear_mask(irq);
        if (irq >= 8) pic_clear_mask(2);
        if (ioapic_present()) {
            uint8_t gsi = acpi_irq_to_gsi[irq];
            ioapic_route_irq(gsi, (uint8_t)(32 + irq),
                             (uint8_t)acpi_bsp_lapic_id, false);
            serial_puts("[e1000] IOAPIC: GSI ");
            serial_print_dec(gsi);
            serial_puts(" -> vector ");
            serial_print_dec(32 + irq);
            serial_puts("\n");
        }
        serial_puts("[e1000] IRQ "); serial_print_dec(irq); serial_puts(" registered\n");
    } else {
        serial_puts("[e1000] invalid IRQ line\n");
        return false;
    }

    // ---- Enable interrupts ----
    // RXT0 (RX timer / RX packet arrived) + RXDMT0 (low RX descriptors) + LSC.
    // TXDW тоже включаем (но мы не блокируем на TX, так что не критично).
    e1000_wr32(E1000_REG_IMS,
               E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_LSC | E1000_INT_RXO);
    (void)e1000_rd32(E1000_REG_ICR);   // clear pending

    // ---- Регистрируем netif ----
    const char* name = "eth0";
    for (int i = 0; i < NETIF_NAME_LEN && name[i]; i++) g_e1000.iface.name[i] = name[i];
    g_e1000.iface.name[NETIF_NAME_LEN - 1] = 0;
    for (int i = 0; i < NETIF_MAC_LEN; i++) g_e1000.iface.mac[i] = g_e1000.mac[i];
    g_e1000.iface.send = e1000_send_impl;
    g_e1000.iface.up = false;
    netif_register(&g_e1000.iface);

    g_e1000.ready = true;

    // ---- Регистрируем polling timer (этап 47.8) ----
    // Если PCI INTx routing где-то не работает (часто на VMware/Hyper-V
    // когда machine type не "pc"), IRQ может не доходить.  Polling
    // backup каждые 10 мс гарантирует обработку RX.  На реальном
    // железе это micro-overhead (10 ms × 1 register read).
    timer_register_callback(e1000_process_rx, 10);
    serial_puts("[e1000] RX polling backup registered (10 ms)\n");

    // ---- Print link status ----
    uint32_t st = e1000_rd32(E1000_REG_STATUS);
    serial_puts("[e1000] link=");
    serial_puts((st & E1000_STATUS_LU) ? "UP" : "DOWN");
    serial_puts(" init OK\n");

    return true;
}

// ---- RX setup --------------------------------------------------------------
static bool e1000_setup_rx(void) {
    // Descriptor ring: 32 × 16 байт = 512 байт.  HW требует:
    //   * 16-byte aligned address (RDBAL bits 0..3 hardwired 0)
    //   * ring size в RDLEN — кратно 128 байт
    //   * ХОТЯ spec говорит 16-byte alignment, на практике многим NIC
    //     нужно 128 byte для надёжной работы — выделим с 128.
    g_e1000.rx_ring = (e1000_rx_desc_t*)kmalloc_aligned(
        E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t), 128);
    if (!g_e1000.rx_ring) return false;
    for (int i = 0; i < E1000_RX_DESC_COUNT; i++) {
        // Будут перезаписаны ниже.
        g_e1000.rx_ring[i].addr     = 0;
        g_e1000.rx_ring[i].length   = 0;
        g_e1000.rx_ring[i].checksum = 0;
        g_e1000.rx_ring[i].status   = 0;
        g_e1000.rx_ring[i].errors   = 0;
        g_e1000.rx_ring[i].special  = 0;
    }

    // Массив указателей на буферы.
    g_e1000.rx_buffers = (uint8_t**)kmalloc(sizeof(uint8_t*) * E1000_RX_DESC_COUNT);
    if (!g_e1000.rx_buffers) return false;

    for (int i = 0; i < E1000_RX_DESC_COUNT; i++) {
        g_e1000.rx_buffers[i] = (uint8_t*)kmalloc_aligned(E1000_RX_BUF_SIZE, 16);
        if (!g_e1000.rx_buffers[i]) return false;
        uint64_t phys = virt_to_phys(g_e1000.rx_buffers[i]);
        if (phys >> 32) {
            serial_puts("[e1000] rx buffer > 4 GB\n");
            return false;
        }
        g_e1000.rx_ring[i].addr   = phys;
        g_e1000.rx_ring[i].status = 0;
    }

    uint64_t ring_phys = virt_to_phys(g_e1000.rx_ring);
    if (ring_phys >> 32) {
        serial_puts("[e1000] rx_ring > 4 GB\n");
        return false;
    }

    // RDBAL/RDBAH = physical address ring'а.  RDLEN = размер в БАЙТАХ.
    e1000_wr32(E1000_REG_RDBAL, (uint32_t)ring_phys);
    e1000_wr32(E1000_REG_RDBAH, (uint32_t)(ring_phys >> 32));
    e1000_wr32(E1000_REG_RDLEN, E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t));

    // RDH = 0 (hardware будет записывать сюда), RDT = COUNT-1 (мы говорим
    // "все 32 дескриптора готовы принять frame").
    e1000_wr32(E1000_REG_RDH, 0);
    e1000_wr32(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1);
    g_e1000.rx_tail = 0;   // мы будем читать с этого индекса вперёд

    // RCTL: enable + broadcast accept + strip CRC + 2K buffer.
    uint32_t rctl = E1000_RCTL_EN
                  | E1000_RCTL_BAM
                  | E1000_RCTL_SECRC
                  | E1000_RCTL_BSIZE_2048
                  | E1000_RCTL_LBM_NONE;
    e1000_wr32(E1000_REG_RCTL, rctl);

    return true;
}

// ---- TX setup --------------------------------------------------------------
static bool e1000_setup_tx(void) {
    g_e1000.tx_ring = (e1000_tx_desc_t*)kmalloc_aligned(
        E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t), 128);
    if (!g_e1000.tx_ring) return false;
    for (int i = 0; i < E1000_TX_DESC_COUNT; i++) {
        g_e1000.tx_ring[i].addr    = 0;
        g_e1000.tx_ring[i].length  = 0;
        g_e1000.tx_ring[i].cso     = 0;
        g_e1000.tx_ring[i].cmd     = 0;
        // ВАЖНО: status = 0 (НЕ ставим DD).  Если поставить DD=1, hardware
        // подумает что descriptor уже обработан и может пропустить наш первый
        // TX (или передать мусор из не-init'ленного буфера).  Spec говорит:
        // "Software should write 0 to status field; hardware writes DD after TX".
        // Для первого TX каждого descriptor'а используем tx_pristine_count, чтобы
        // не ждать DD (его никогда не будет, потому что HW ещё не передавал).
        g_e1000.tx_ring[i].status  = 0;
        g_e1000.tx_ring[i].css     = 0;
        g_e1000.tx_ring[i].special = 0;
    }
    g_e1000.tx_pristine_count = E1000_TX_DESC_COUNT;

    g_e1000.tx_buffers = (uint8_t**)kmalloc(sizeof(uint8_t*) * E1000_TX_DESC_COUNT);
    if (!g_e1000.tx_buffers) return false;
    for (int i = 0; i < E1000_TX_DESC_COUNT; i++) {
        g_e1000.tx_buffers[i] = (uint8_t*)kmalloc_aligned(E1000_TX_BUF_SIZE, 16);
        if (!g_e1000.tx_buffers[i]) return false;
        uint64_t phys = virt_to_phys(g_e1000.tx_buffers[i]);
        if (phys >> 32) return false;
    }

    uint64_t ring_phys = virt_to_phys(g_e1000.tx_ring);
    if (ring_phys >> 32) return false;

    e1000_wr32(E1000_REG_TDBAL, (uint32_t)ring_phys);
    e1000_wr32(E1000_REG_TDBAH, (uint32_t)(ring_phys >> 32));
    e1000_wr32(E1000_REG_TDLEN, E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t));

    e1000_wr32(E1000_REG_TDH, 0);
    e1000_wr32(E1000_REG_TDT, 0);
    g_e1000.tx_tail = 0;

    // TCTL: enable + pad short + standard collision threshold.
    uint32_t tctl = E1000_TCTL_EN
                  | E1000_TCTL_PSP
                  | (0x10u << E1000_TCTL_CT_SHIFT)
                  | (0x40u << E1000_TCTL_COLD_SHIFT);
    e1000_wr32(E1000_REG_TCTL, tctl);

    // TIPG: legacy IEEE 802.3 inter-packet gap.
    // bits 0-9   IPGT = 10 (default for copper)
    // bits 10-19 IPGR1 = 8
    // bits 20-29 IPGR2 = 6
    e1000_wr32(E1000_REG_TIPG, 10 | (8u << 10) | (6u << 20));

    return true;
}

// ---- IRQ handler -----------------------------------------------------------
static void e1000_irq(registers64_t* r) {
    (void)r;
    uint32_t icr = e1000_rd32(E1000_REG_ICR);   // read clears
    if (icr == 0) {
        lapic_eoi();
        pic_send_eoi(g_e1000.pci.irq_line);
        return;
    }

    if (icr & (E1000_INT_RXT0 | E1000_INT_RXDMT0)) {
        e1000_process_rx();
    }
    if (icr & E1000_INT_RXO) {
        serial_puts("[e1000] RX overrun\n");
        // Просто продвинем tail если возможно — RXO значит hardware догнал
        // нас и сбросил packets.  Не критично — попросим reset RDT.
        e1000_process_rx();
    }
    if (icr & E1000_INT_LSC) {
        uint32_t st = e1000_rd32(E1000_REG_STATUS);
        serial_puts("[e1000] LSC link=");
        serial_puts((st & E1000_STATUS_LU) ? "UP\n" : "DOWN\n");
    }

    lapic_eoi();
    pic_send_eoi(g_e1000.pci.irq_line);
}

// ---- RX process ------------------------------------------------------------
// Walk дескрипторы от rx_tail вперёд, пока status.DD set.  Каждый ready
// packet отдать в netif_receive.  Затем продвинуть RDT.
static void e1000_process_rx(void) {
    while (1) {
        e1000_rx_desc_t* d = &g_e1000.rx_ring[g_e1000.rx_tail];
        if (!(d->status & E1000_RXD_STAT_DD)) break;

        uint16_t len = d->length;
        if (len > 0 && len <= ETH_MAX_LEN + 4) {
            // SECRC уже срезал CRC, len = полезная нагрузка ethernet frame.
            netif_receive(g_e1000.rx_buffers[g_e1000.rx_tail], len);
        }

        // Reset descriptor — hardware будет писать в него снова.
        d->status = 0;
        d->length = 0;
        d->checksum = 0;
        d->errors = 0;
        d->special = 0;

        // RDT (tail) = последний "available" descriptor, который мы вернули
        // hardware'у.  Двигаем РОВНО до текущего минус 1 (HW wraps).
        uint32_t old_tail = g_e1000.rx_tail;
        g_e1000.rx_tail = (g_e1000.rx_tail + 1) % E1000_RX_DESC_COUNT;
        e1000_wr32(E1000_REG_RDT, old_tail);
    }
}

// ---- TX path ---------------------------------------------------------------
// Берём текущий tx_tail дескриптор, ждём пока его DD bit не будет 1 (HW
// освободил его после предыдущей записи), копируем data в tx_buffers[tail],
// заполняем cmd, двигаем TDT.
static size_t e1000_send_impl(const void* data, size_t len) {
    if (!g_e1000.ready || !data || len == 0 || len > E1000_TX_BUF_SIZE) return 0;

    uint32_t idx = g_e1000.tx_tail;
    // volatile — гарантирует что компилятор не закеширует записи в descriptor.
    // HW читает descriptor через DMA, ему важна точная последовательность.
    volatile e1000_tx_desc_t* d = (volatile e1000_tx_desc_t*)&g_e1000.tx_ring[idx];

    // Ждём DD bit (предыдущий transmit с этого дескриптора завершился).
    // Для свежих (pristine) descriptors, которые HW ни разу не трогал,
    // DD никогда не будет — wait пропускаем.
    if (g_e1000.tx_pristine_count > 0) {
        g_e1000.tx_pristine_count--;
    } else {
        int spins = 1000000;
        while (!(d->status & E1000_TXD_STAT_DD) && spins--) {
            io_wait();
        }
        if (!(d->status & E1000_TXD_STAT_DD)) {
            serial_puts("[e1000] TX timeout\n");
            return 0;
        }
    }

    // Copy data в наш TX buffer.
    uint8_t* buf = g_e1000.tx_buffers[idx];
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) buf[i] = src[i];


    uint64_t buf_phys = virt_to_phys(buf);
    if (buf_phys >> 32) return 0;

    d->addr    = buf_phys;
    d->length  = (uint16_t)len;
    d->cso     = 0;
    d->cmd     = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->status  = 0;     // hardware установит DD после отправки
    d->css     = 0;
    d->special = 0;

    // Memory barrier: гарантируем что все наши writes в descriptor + buffer
    // видны HW DMA ДО bump'а TDT.  Без этого HW может прочитать stale
    // descriptor и/или половину buffer'а.
    __asm__ __volatile__("sfence" ::: "memory");

    g_e1000.tx_tail = (idx + 1) % E1000_TX_DESC_COUNT;
    e1000_wr32(E1000_REG_TDT, g_e1000.tx_tail);   // notify HW

    return len;
}
