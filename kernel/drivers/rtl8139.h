// ============================================================================
// rtl8139.h — драйвер сетевой карты Realtek RTL8139 (Этап 25)
// ----------------------------------------------------------------------------
// RTL8139 — классическая 10/100 Mbit PCI Ethernet-карта эпохи 2000-х.
// QEMU идеально её эмулирует (-device rtl8139), спецификация открыта,
// программная модель максимально простая — поэтому это первый выбор для
// любой учебной ОС.
//
// Архитектура:
//   • PCI VENDOR=0x10EC DEVICE=0x8139
//   • BAR0 = IO ports (мы используем IO-режим, не MMIO — так проще
//     на 32-битном x86 без mapping helper'ов).
//   • Один непрерывный RX-буфер (ring), 8192+16+1500 = 9708 байт,
//     карта пишет в него подряд новые пакеты, драйвер читает по CAPR/CBR.
//   • TX — 4 дескриптора, round-robin. Для каждого — свой 1.5 KiB буфер.
//   • IRQ — IRQ_LINE берётся из PCI config 0x3C (в QEMU обычно 10 или 11).
//
// Подключается через абстракцию netif.h: rtl8139_init() регистрирует
// объект net_interface_t с коллбэками send/stats.
// ============================================================================

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- PCI ID ---
#define RTL8139_VENDOR_ID   0x10EC
#define RTL8139_DEVICE_ID   0x8139

// --- Регистры (смещения от BAR0, IO-порты) ---
// Всё по Realtek RTL8139 Programming Guide.
#define RTL_REG_MAC0        0x00  // 6 байт MAC
#define RTL_REG_MAR0        0x08  // 8 байт multicast filter
#define RTL_REG_TSD0        0x10  // TX Status Descriptor 0..3 (по 4 байта)
#define RTL_REG_TSAD0       0x20  // TX Start Address of Descriptor 0..3
#define RTL_REG_RBSTART     0x30  // RX Buffer Start address (phys)
#define RTL_REG_ERBCR       0x34  // Early RX byte count
#define RTL_REG_ERSR        0x36  // Early RX status
#define RTL_REG_CR          0x37  // Command Register (byte)
#define RTL_REG_CAPR        0x38  // Current Address of Packet Read
#define RTL_REG_CBR         0x3A  // Current Buffer Address (RX write ptr)
#define RTL_REG_IMR         0x3C  // Interrupt Mask (word)
#define RTL_REG_ISR         0x3E  // Interrupt Status (word)
#define RTL_REG_TCR         0x40  // TX Configuration
#define RTL_REG_RCR         0x44  // RX Configuration
#define RTL_REG_TCTR        0x48  // Timer count
#define RTL_REG_MPC         0x4C  // Missed Packet Counter
#define RTL_REG_9346CR      0x50  // 93C46 EEPROM command register
#define RTL_REG_CONFIG0     0x51
#define RTL_REG_CONFIG1     0x52
#define RTL_REG_MSR         0x58  // Media Status
#define RTL_REG_CONFIG3     0x59
#define RTL_REG_CONFIG4     0x5A
#define RTL_REG_MULINT      0x5C
#define RTL_REG_BMCR        0x62  // Basic Mode Control
#define RTL_REG_BMSR        0x64  // Basic Mode Status

// --- CR (Command Register) биты ---
#define RTL_CR_BUFE         (1 << 0)  // Buffer Empty (RO)
#define RTL_CR_TE           (1 << 2)  // Transmitter Enable
#define RTL_CR_RE           (1 << 3)  // Receiver Enable
#define RTL_CR_RST          (1 << 4)  // Reset

// --- IMR/ISR биты (совпадают) ---
#define RTL_INT_ROK         (1 << 0)  // Receive OK
#define RTL_INT_RER         (1 << 1)  // Receive Error
#define RTL_INT_TOK         (1 << 2)  // Transmit OK
#define RTL_INT_TER         (1 << 3)  // Transmit Error
#define RTL_INT_RXOVW       (1 << 4)  // RX buffer overflow
#define RTL_INT_LINKCHG     (1 << 5)  // Link change
#define RTL_INT_FIFOOVW     (1 << 6)  // RX FIFO overflow
#define RTL_INT_LENCHG      (1 << 13) // Cable length change
#define RTL_INT_TIMEOUT     (1 << 14)
#define RTL_INT_SERR        (1 << 15) // System error

// --- RCR (RX Configuration) биты ---
#define RTL_RCR_AAP         (1 << 0)  // Accept All Packets (promisc)
#define RTL_RCR_APM         (1 << 1)  // Accept Physical Match (наш MAC)
#define RTL_RCR_AM          (1 << 2)  // Accept Multicast
#define RTL_RCR_AB          (1 << 3)  // Accept Broadcast
#define RTL_RCR_AR          (1 << 4)  // Accept Runt
#define RTL_RCR_AER         (1 << 5)  // Accept Error
#define RTL_RCR_WRAP        (1 << 7)  // Wrap (карта не wrap'ит — пишет за границу)
// MXDMA: биты 8..10 — макс burst DMA. 0b110 = 1024 байт.
#define RTL_RCR_MXDMA_1024  (6 << 8)
// RBLEN: биты 11..12 — размер RX буфера. 0 = 8 KiB+16, 1 = 16K+16, 2 = 32K+16.
#define RTL_RCR_RBLEN_8K    (0 << 11)

// --- TCR (TX Configuration) биты ---
#define RTL_TCR_MXDMA_2048  (7 << 8)
// IFG: биты 24..25 — inter frame gap. По умолчанию (3<<24) = стандартный.
#define RTL_TCR_IFG_STD     (3 << 24)

// --- TSD (TX Status Descriptor) биты ---
#define RTL_TSD_OWN         (1 << 13) // driver→NIC: 0 = NIC владеет
#define RTL_TSD_TUN         (1 << 14) // TX FIFO underrun
#define RTL_TSD_TOK         (1 << 15) // TX OK

// --- Размеры буферов ---
#define RTL_RX_BUF_SIZE     8192
#define RTL_RX_BUF_PAD      16      // spec: +16 байт защита от overrun
#define RTL_RX_BUF_WRAP     1500    // +MTU, т.к. WRAP=0 и пакет может торчать за границей
#define RTL_RX_BUF_TOTAL    (RTL_RX_BUF_SIZE + RTL_RX_BUF_PAD + RTL_RX_BUF_WRAP)
#define RTL_TX_BUF_COUNT    4
#define RTL_TX_BUF_SIZE     2048    // с запасом под Ethernet MTU+заголовки

// --- Публичное API ---
// Инициализация. Возвращает true, если RTL8139 найдено и поднято.
// Регистрирует объект netif с именем "eth0".
bool rtl8139_init(void);

// MAC-адрес текущей карты (NULL, если не поднята).
const uint8_t* rtl8139_get_mac(void);

// Отправить сырой Ethernet-фрейм (с уже сформированными dst/src/type).
// Возвращает число переданных байт либо 0 при ошибке.
size_t rtl8139_send(const void* data, size_t len);

// Статистика.
typedef struct {
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_errors;
    uint32_t rx_dropped;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_errors;
} rtl8139_stats_t;
const rtl8139_stats_t* rtl8139_get_stats(void);

// Доступен ли драйвер (после успешного init).
bool rtl8139_is_ready(void);

#endif // RTL8139_H
