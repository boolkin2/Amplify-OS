// ============================================================================
// rtl8139.c — драйвер Realtek RTL8139
// ----------------------------------------------------------------------------
// Последовательность инициализации (строго по Realtek Programming Guide):
//
//   1. Найти устройство 0x10EC:0x8139 через PCI.
//   2. Включить Bus Master + IO Space в Command Register.
//   3. Разбудить чип: CONFIG1 = 0x00 (убрать LWAKE + power-down).
//   4. Software reset: CR = 0x10, подождать пока бит сбросится.
//   5. Выделить непрерывный RX-буфер (8K+16+1500), записать его физ.адрес
//      в RBSTART (4-байтный регистр).
//   6. IMR = интересующие биты (ROK | TOK | RER | TER | RXOVW).
//   7. RCR = AAP|APM|AM|AB + MXDMA=1024 + RBLEN=8K.
//   8. TCR = IFG=standard + MXDMA=2048.
//   9. CR = RE | TE.
//  10. Прочитать MAC (MAC0..MAC5).
//  11. Зарегистрировать IRQ.
//  12. Передать netif_register() с коллбэком rtl8139_send.
//
// IRQ handler:
//   • Прочитать ISR, записать его обратно (write-to-clear).
//   • Если ROK — слить пакеты из RX-буфера.
//   • Если TOK/TER — обновить статистику.
//
// RX: пакеты лежат подряд в кольце вида
//     [hdr:2][len:2][packet:len-4 байт: eth + IP + ... + FCS:4]
//   hdr — статус (ROK/BAD), len — длина вместе с FCS.
//   После обработки drivers обновляет CAPR (write = my_read_offset - 16).
//
// TX: 4 дескриптора по 2 KiB. Копируем фрейм в ближайший свободный буфер,
//   пишем TSAD (физ.адрес) и TSD (размер + start bit).
// ============================================================================

#include "rtl8139.h"
#include "pci.h"
#include "../io.h"
#include "../memory/heap.h"
#include "../arch/i386/isr.h"
#include "../net/netif.h"
#include "../net/ethernet.h"
#include "../debug_serial.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Состояние драйвера
// ----------------------------------------------------------------------------
typedef struct {
    bool          ready;
    pci_device_t  pci;
    uint16_t      iobase;      // BAR0, младшая часть

    uint8_t*      rx_buffer;   // выровнен по 4 байта, размер RTL_RX_BUF_TOTAL
    uint16_t      rx_offset;   // текущий read-указатель (в пределах 8K+16)

    uint8_t*      tx_buffer[RTL_TX_BUF_COUNT]; // по 2 KiB каждый
    uint8_t       tx_current;  // индекс следующего дескриптора для записи

    uint8_t       mac[6];

    rtl8139_stats_t stats;

    net_interface_t iface;     // объект, регистрируемый в netif
} rtl8139_state_t;

static rtl8139_state_t rtl = {0};

// ----------------------------------------------------------------------------
// Удобные helpers
// ----------------------------------------------------------------------------
static inline uint8_t  rd8 (uint16_t off) { return inb (rtl.iobase + off); }
static inline uint16_t rd16(uint16_t off) { return inw (rtl.iobase + off); }
static inline uint32_t rd32(uint16_t off) { return inl (rtl.iobase + off); }
static inline void     wr8 (uint16_t off, uint8_t v)  { outb(rtl.iobase + off, v); }
static inline void     wr16(uint16_t off, uint16_t v) { outw(rtl.iobase + off, v); }
static inline void     wr32(uint16_t off, uint32_t v) { outl(rtl.iobase + off, v); }

// ----------------------------------------------------------------------------
// Forward-деклoрации
// ----------------------------------------------------------------------------
static uint32_t rtl8139_irq_handler(registers_t* regs);
static void     rtl8139_process_rx(void);
static size_t   rtl8139_send_impl(const void* data, size_t len);

// ----------------------------------------------------------------------------
// API: статус/MAC/stats
// ----------------------------------------------------------------------------
bool rtl8139_is_ready(void) { return rtl.ready; }

const uint8_t* rtl8139_get_mac(void) {
    return rtl.ready ? rtl.mac : NULL;
}

const rtl8139_stats_t* rtl8139_get_stats(void) {
    return &rtl.stats;
}

size_t rtl8139_send(const void* data, size_t len) {
    return rtl8139_send_impl(data, len);
}

// ============================================================================
// rtl8139_init
// ============================================================================
bool rtl8139_init(void) {
    dbg_puts("[rtl8139] probing PCI...\n");

    rtl.pci = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!rtl.pci.found) {
        // Fallback: ищем любую карту класса Network Controller Ethernet (0x02/0x00).
        // Пригодится, если QEMU стоит другая модель (пропустим её).
        dbg_puts("[rtl8139] not found by VID/DID\n");
        return false;
    }

    dbg_puts("[rtl8139] found at bus=");
    dbg_hex(rtl.pci.bus);
    dbg_puts(" dev=");
    dbg_hex(rtl.pci.device);
    dbg_puts(" fn=");
    dbg_hex(rtl.pci.function);
    dbg_puts(" IRQ=");
    dbg_hex(rtl.pci.irq_line);
    dbg_puts("\n");

    // --- Шаг 2: Включить Bus Master + IO Space ---
    pci_enable_bus_master(&rtl.pci);

    // --- BAR0 → IO base ---
    uint32_t bar0 = pci_read_bar(&rtl.pci, 0);
    if (!pci_bar_is_io(&rtl.pci, 0)) {
        dbg_puts("[rtl8139] BAR0 is not IO — unsupported\n");
        return false;
    }
    rtl.iobase = (uint16_t)(bar0 & 0xFFFC);
    dbg_puts("[rtl8139] iobase=0x");
    dbg_hex(rtl.iobase);
    dbg_puts("\n");

    // --- Шаг 3: Power-on (CONFIG1 = 0) ---
    wr8(RTL_REG_CONFIG1, 0x00);

    // --- Шаг 4: Software reset ---
    wr8(RTL_REG_CR, RTL_CR_RST);
    // Ждём, пока карта сбросит бит RST. Ограничиваем число попыток.
    for (int i = 0; i < 100000; i++) {
        if ((rd8(RTL_REG_CR) & RTL_CR_RST) == 0) break;
        io_wait();
    }
    if (rd8(RTL_REG_CR) & RTL_CR_RST) {
        dbg_puts("[rtl8139] reset timeout\n");
        return false;
    }
    dbg_puts("[rtl8139] reset ok\n");

    // --- Шаг 5: RX буфер ---
    // Выделяем выровненную память — важно для RBSTART (физ.адрес).
    // Identity-mapping первых 64 MiB сохраняется, так что phys==virt.
    rtl.rx_buffer = (uint8_t*)kmalloc_aligned(RTL_RX_BUF_TOTAL, 16);
    if (!rtl.rx_buffer) {
        dbg_puts("[rtl8139] kmalloc rx_buffer failed\n");
        return false;
    }
    // Нулируем — чтобы старые байты не выглядели как пакеты при первом чтении.
    for (size_t i = 0; i < RTL_RX_BUF_TOTAL; i++) rtl.rx_buffer[i] = 0;
    rtl.rx_offset = 0;
    wr32(RTL_REG_RBSTART, (uint32_t)(uintptr_t)rtl.rx_buffer);
    dbg_puts("[rtl8139] rx_buffer at 0x");
    dbg_hex((uint32_t)(uintptr_t)rtl.rx_buffer);
    dbg_puts("\n");

    // --- TX буферы ---
    for (int i = 0; i < RTL_TX_BUF_COUNT; i++) {
        rtl.tx_buffer[i] = (uint8_t*)kmalloc_aligned(RTL_TX_BUF_SIZE, 16);
        if (!rtl.tx_buffer[i]) {
            dbg_puts("[rtl8139] kmalloc tx_buffer failed\n");
            return false;
        }
    }
    rtl.tx_current = 0;

    // --- Шаг 6: IMR — разрешаем интересующие прерывания ---
    wr16(RTL_REG_IMR,
         RTL_INT_ROK | RTL_INT_RER | RTL_INT_TOK | RTL_INT_TER | RTL_INT_RXOVW);

    // --- Шаг 7: RCR — принимать broadcast/multicast/our-mac ---
    // Без WRAP → карта может писать до +1500 байт за конец буфера
    // (мы специально оставили хвост под это).
    wr32(RTL_REG_RCR,
         RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_AM |
         RTL_RCR_MXDMA_1024 | RTL_RCR_RBLEN_8K);

    // --- Шаг 8: TCR ---
    wr32(RTL_REG_TCR, RTL_TCR_IFG_STD | RTL_TCR_MXDMA_2048);

    // --- Шаг 9: Включить RX и TX ---
    wr8(RTL_REG_CR, RTL_CR_RE | RTL_CR_TE);

    // --- Шаг 10: Прочитать MAC ---
    for (int i = 0; i < 6; i++) rtl.mac[i] = rd8((uint16_t)(RTL_REG_MAC0 + i));
    dbg_puts("[rtl8139] MAC=");
    netif_dump_mac(rtl.mac);
    dbg_puts("\n");

    // --- Шаг 11: IRQ ---
    // PCI INT pin мапится на PIC-IRQ согласно PCI Interrupt Line.
    uint8_t irq = rtl.pci.irq_line;
    if (irq >= 16) {
        dbg_puts("[rtl8139] invalid IRQ line — skip IRQ setup\n");
    } else {
        isr_register_handler((uint8_t)(32 + irq), rtl8139_irq_handler);
        dbg_puts("[rtl8139] IRQ ");
        dbg_hex(irq);
        dbg_puts(" registered\n");
    }

    // --- Шаг 12: Регистрируем netif ---
    // Заполняем структуру один раз.
    const char* name = "eth0";
    for (int i = 0; i < NETIF_NAME_LEN && name[i]; i++) rtl.iface.name[i] = name[i];
    rtl.iface.name[NETIF_NAME_LEN - 1] = 0;
    for (int i = 0; i < NETIF_MAC_LEN; i++) rtl.iface.mac[i] = rtl.mac[i];
    rtl.iface.send = rtl8139_send_impl;
    rtl.iface.ip4_addr    = 0;
    rtl.iface.ip4_netmask = 0;
    rtl.iface.ip4_gateway = 0;
    rtl.iface.up = false;
    netif_register(&rtl.iface);

    rtl.ready = true;
    dbg_puts("[rtl8139] init OK\n");
    return true;
}

// ============================================================================
// IRQ handler
// ============================================================================
static uint32_t rtl8139_irq_handler(registers_t* regs) {
    (void)regs;
    uint16_t isr = rd16(RTL_REG_ISR);
    if (isr == 0) {
        // Не наше прерывание — shared IRQ line.
        return 0;
    }
    // Write-1-to-clear: записываем обратно, чтобы сбросить биты.
    wr16(RTL_REG_ISR, isr);

    if (isr & RTL_INT_ROK) {
        rtl8139_process_rx();
    }
    if (isr & RTL_INT_RER) {
        rtl.stats.rx_errors++;
        dbg_puts("[rtl8139] RX error\n");
    }
    if (isr & RTL_INT_TOK) {
        // Пробегаем TSD'ы и суммируем bytes/packets у завершённых.
        for (int i = 0; i < RTL_TX_BUF_COUNT; i++) {
            uint32_t tsd = rd32((uint16_t)(RTL_REG_TSD0 + i * 4));
            if (tsd & RTL_TSD_TOK) {
                rtl.stats.tx_packets++;
                rtl.stats.tx_bytes += (tsd & 0x1FFF);
            }
        }
    }
    if (isr & RTL_INT_TER) {
        rtl.stats.tx_errors++;
        dbg_puts("[rtl8139] TX error\n");
    }
    if (isr & RTL_INT_RXOVW) {
        dbg_puts("[rtl8139] RX overflow — flush\n");
        rtl.rx_offset = rd16(RTL_REG_CBR) % RTL_RX_BUF_SIZE;
        wr16(RTL_REG_CAPR, (uint16_t)(rtl.rx_offset - 16));
    }
    return 0;
}

// ============================================================================
// RX: вычитать все накопленные пакеты
// ============================================================================
static void rtl8139_process_rx(void) {
    // Пока BUFE=0 — есть что читать.
    while (!(rd8(RTL_REG_CR) & RTL_CR_BUFE)) {
        // Заголовок пакета в RX-буфере.
        uint8_t* p = rtl.rx_buffer + rtl.rx_offset;
        uint16_t hdr = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));

        if (!(hdr & 0x0001)) {
            // Пакет плохой — ROK bit не установлен. Сбрасываем RX.
            dbg_puts("[rtl8139] bad packet, reset RX\n");
            uint8_t cr = rd8(RTL_REG_CR);
            wr8(RTL_REG_CR, cr & ~RTL_CR_RE);
            wr8(RTL_REG_CR, cr);
            rtl.rx_offset = 0;
            wr16(RTL_REG_CAPR, 0xFFF0);
            rtl.stats.rx_errors++;
            return;
        }

        if (len < 14 || len > ETH_MAX_LEN + 4 + 4) {
            // len включает 4 байта FCS; отбрасываем явный мусор.
            rtl.stats.rx_dropped++;
        } else {
            // Передаём вверх без FCS (последние 4 байта).
            netif_receive(p + 4, (size_t)(len - 4));
            rtl.stats.rx_packets++;
            rtl.stats.rx_bytes += (len - 4);
        }

        // Следующий пакет выровнен по 4 байта.
        rtl.rx_offset = (uint16_t)((rtl.rx_offset + len + 4 + 3) & ~3u);
        // Кольцо 8 KiB.
        if (rtl.rx_offset >= RTL_RX_BUF_SIZE) {
            rtl.rx_offset -= RTL_RX_BUF_SIZE;
        }
        // Обновляем CAPR: (my_offset - 16) по модулю буфера.
        wr16(RTL_REG_CAPR, (uint16_t)(rtl.rx_offset - 16));
    }
}

// ============================================================================
// TX
// ============================================================================
static size_t rtl8139_send_impl(const void* data, size_t len) {
    if (!rtl.ready || !data) return 0;
    if (len == 0 || len > RTL_TX_BUF_SIZE) return 0;

    // Берём текущий дескриптор; ждём, пока NIC его освободит.
    uint8_t idx = rtl.tx_current;
    uint16_t tsd_off = (uint16_t)(RTL_REG_TSD0 + idx * 4);

    // Ждём бит OWN=1 (признак: NIC закончил с этим дескриптором).
    // OWN=0 означает "NIC ещё работает". Timeout — 1 млн итераций.
    for (int i = 0; i < 1000000; i++) {
        uint32_t s = rd32(tsd_off);
        if (s & RTL_TSD_OWN) break;
        io_wait();
    }

    // Копируем фрейм в TX-буфер.
    uint8_t* buf = rtl.tx_buffer[idx];
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) buf[i] = src[i];

    // Физический адрес буфера.
    wr32((uint16_t)(RTL_REG_TSAD0 + idx * 4), (uint32_t)(uintptr_t)buf);

    // Размер + старт (бит OWN сбрасывается в 0 — NIC берёт пакет).
    // Биты 0..12 — длина. Биты 13..15 — status (мы пишем 0).
    uint32_t tsd = (uint32_t)(len & 0x1FFF);
    wr32(tsd_off, tsd);

    // Следующий дескриптор round-robin.
    rtl.tx_current = (uint8_t)((rtl.tx_current + 1) & (RTL_TX_BUF_COUNT - 1));

    return len;
}
