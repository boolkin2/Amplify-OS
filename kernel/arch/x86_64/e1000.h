// ============================================================================
// e1000.h — Intel 82540EM / 82545EM / 82574L Gigabit Ethernet driver (этап 47.8).
// ----------------------------------------------------------------------------
// Зачем: VMware Workstation / Player эмулирует Intel e1000 как стандартный
// virtual NIC, а наш RTL8139 driver там не работает (VMware его не эмулирует).
// На реальном железе e1000 чипы тоже распространены.  QEMU поддерживает e1000
// через `-device e1000` (и это стандарт для VirtualBox/Xen guest).
//
// Что реализуем (минимум для DHCP+DNS+ICMP+HTTPS):
//   * PCI probe для 0x8086:0x100E (82540EM), 0x100F (82545EM), 0x10D3 (82574L)
//   * MMIO BAR0 (нет I/O port доступа, в отличие от RTL8139)
//   * MAC из RAL0/RAH0 (уже загружен из EEPROM при reset'е)
//   * RX ring: 32 дескриптора + 2048-байт буферы (один frame)
//   * TX ring: 32 дескриптора + 2048-байт буферы
//   * Legacy IRQ через IOAPIC (как rtl8139)
//   * Промискуитет НЕ включаем — принимаем unicast (MAC match) + broadcast
//
// Что НЕ реализуем (отложено):
//   * MSI / MSI-X
//   * TCP/UDP/IP checksum offload (TSO, RX checksum)
//   * Jumbo frames (>1518)
//   * Multi-queue (RSS)
//   * Power management
//   * Wake-on-LAN
//   * Flow control
//   * VLAN tagging
// ============================================================================
#ifndef AOS_X86_64_E1000_H
#define AOS_X86_64_E1000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// PCI Vendor + Devices.  Intel 0x8086.
#define E1000_VENDOR_ID         0x8086
#define E1000_DEV_82540EM       0x100E   // QEMU/VMware standard
#define E1000_DEV_82545EM       0x100F
#define E1000_DEV_82574L        0x10D3   // e1000e — другой layout, support частичный
#define E1000_DEV_82571         0x105E
#define E1000_DEV_I217          0x153A
#define E1000_DEV_82577LM       0x10EA

// ---- MMIO регистры (offset от BAR0) ----------------------------------------
#define E1000_REG_CTRL          0x00000   // Device Control
#define E1000_REG_STATUS        0x00008   // Device Status
#define E1000_REG_EECD          0x00010   // EEPROM/Flash Control
#define E1000_REG_EERD          0x00014   // EEPROM Read
#define E1000_REG_CTRL_EXT      0x00018   // Extended Device Control
#define E1000_REG_ICR           0x000C0   // Interrupt Cause Read
#define E1000_REG_ITR           0x000C4   // Interrupt Throttling
#define E1000_REG_ICS           0x000C8   // Interrupt Cause Set
#define E1000_REG_IMS           0x000D0   // Interrupt Mask Set
#define E1000_REG_IMC           0x000D8   // Interrupt Mask Clear
#define E1000_REG_RCTL          0x00100   // Receive Control
#define E1000_REG_TCTL          0x00400   // Transmit Control
#define E1000_REG_TIPG          0x00410   // Transmit IPG

// RX ring registers
#define E1000_REG_RDBAL         0x02800   // RX Desc Base Address Low
#define E1000_REG_RDBAH         0x02804   // RX Desc Base Address High
#define E1000_REG_RDLEN         0x02808   // RX Desc Length (bytes)
#define E1000_REG_RDH           0x02810   // RX Desc Head
#define E1000_REG_RDT           0x02818   // RX Desc Tail

// TX ring registers
#define E1000_REG_TDBAL         0x03800
#define E1000_REG_TDBAH         0x03804
#define E1000_REG_TDLEN         0x03808
#define E1000_REG_TDH           0x03810
#define E1000_REG_TDT           0x03818

// Multicast Table Array — 128 регистров по 32 бита (обнуляем при init).
#define E1000_REG_MTA_BASE      0x05200

// Receive Address Low/High — MAC адрес (16 пар).  Используем только [0].
#define E1000_REG_RAL0          0x05400
#define E1000_REG_RAH0          0x05404
#define E1000_RAH_AV            (1u << 31)   // Address Valid

// ---- CTRL bits -------------------------------------------------------------
#define E1000_CTRL_FD           (1u << 0)    // Full Duplex
#define E1000_CTRL_ASDE         (1u << 5)    // Auto-Speed Detection Enable
#define E1000_CTRL_SLU          (1u << 6)    // Set Link Up
#define E1000_CTRL_RST          (1u << 26)   // Device Reset
#define E1000_CTRL_PHY_RST      (1u << 31)   // PHY Reset

// ---- STATUS bits -----------------------------------------------------------
#define E1000_STATUS_LU         (1u << 1)    // Link Up

// ---- EECD bits -------------------------------------------------------------
#define E1000_EECD_PRES         (1u << 8)    // EEPROM Present

// ---- EERD bits -------------------------------------------------------------
#define E1000_EERD_START        (1u << 0)
#define E1000_EERD_DONE_82540   (1u << 4)    // на 82540EM
#define E1000_EERD_DONE_82574   (1u << 1)    // на 82574L

// ---- ICR / IMS bits --------------------------------------------------------
#define E1000_INT_TXDW          (1u << 0)    // TX Desc Written Back
#define E1000_INT_TXQE          (1u << 1)    // TX Queue Empty
#define E1000_INT_LSC           (1u << 2)    // Link Status Change
#define E1000_INT_RXSEQ         (1u << 3)
#define E1000_INT_RXDMT0        (1u << 4)    // RX Desc Min Threshold
#define E1000_INT_RXO           (1u << 6)    // RX Overrun
#define E1000_INT_RXT0          (1u << 7)    // RX Timer

// ---- RCTL bits -------------------------------------------------------------
#define E1000_RCTL_EN           (1u << 1)    // Receiver Enable
#define E1000_RCTL_SBP          (1u << 2)    // Store Bad Packets
#define E1000_RCTL_UPE          (1u << 3)    // Unicast Promiscuous
#define E1000_RCTL_MPE          (1u << 4)    // Multicast Promiscuous
#define E1000_RCTL_LBM_NONE     (0u << 6)
#define E1000_RCTL_BAM          (1u << 15)   // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048   (0u << 16)   // 2048-byte receive buffer
#define E1000_RCTL_SECRC        (1u << 26)   // Strip CRC

// ---- TCTL bits -------------------------------------------------------------
#define E1000_TCTL_EN           (1u << 1)    // Transmit Enable
#define E1000_TCTL_PSP          (1u << 3)    // Pad Short Packets
#define E1000_TCTL_CT_SHIFT     4
#define E1000_TCTL_COLD_SHIFT   12

// ---- TX descriptor command bits --------------------------------------------
#define E1000_TXD_CMD_EOP       (1u << 0)    // End of Packet
#define E1000_TXD_CMD_IFCS      (1u << 1)    // Insert FCS (CRC)
#define E1000_TXD_CMD_RS        (1u << 3)    // Report Status (set DD)

// ---- RX / TX descriptor status bits ----------------------------------------
#define E1000_RXD_STAT_DD       (1u << 0)    // Descriptor Done
#define E1000_RXD_STAT_EOP      (1u << 1)
#define E1000_TXD_STAT_DD       (1u << 0)

// ---- Кольца ---------------------------------------------------------------
// Размеры выбраны кратными 128 байт (требование 82540EM RDLEN/TDLEN):
//   32 desc × 16 byte = 512 байт.
#define E1000_RX_DESC_COUNT     32
#define E1000_TX_DESC_COUNT     32
#define E1000_RX_BUF_SIZE       2048
#define E1000_TX_BUF_SIZE       2048

// Дескрипторы — 16 байт каждый, не путать с e1000e advanced (32 байта).
// Все поля natural-aligned в 16-byte boundary'е, поэтому `packed` НЕ нужен —
// он только тормозит доступ (GCC может разбить uint64_t addr на byte-by-byte
// store'ы, между которыми HW успеет прочитать половину descriptor'а).
// `aligned(16)` обязателен для DMA alignment.
typedef struct __attribute__((aligned(16))) {
    uint64_t addr;        // physical address of buffer       (offset 0,  8 byte)
    uint16_t length;      //                                  (offset 8,  2 byte)
    uint16_t checksum;    //                                  (offset 10, 2 byte)
    uint8_t  status;      //                                  (offset 12, 1 byte)
    uint8_t  errors;      //                                  (offset 13, 1 byte)
    uint16_t special;     //                                  (offset 14, 2 byte)
} e1000_rx_desc_t;

typedef struct __attribute__((aligned(16))) {
    uint64_t addr;        //                                  (offset 0,  8 byte)
    uint16_t length;      //                                  (offset 8,  2 byte)
    uint8_t  cso;         //                                  (offset 10, 1 byte)
    uint8_t  cmd;         //                                  (offset 11, 1 byte)
    uint8_t  status;      //                                  (offset 12, 1 byte)
    uint8_t  css;         //                                  (offset 13, 1 byte)
    uint16_t special;     //                                  (offset 14, 2 byte)
} e1000_tx_desc_t;

// ---- API -------------------------------------------------------------------
bool           e1000_init(void);
bool           e1000_is_ready(void);
const uint8_t* e1000_get_mac(void);
size_t         e1000_send(const void* data, size_t len);

// Polling backup для RX: если IRQ почему-то не приходит (PCI INT routing
// неверный, IOAPIC mismatch, или просто polling-режим), эта функция читает
// descriptor ring напрямую и dispatches любые ready packets в netif_receive.
// Безопасно вызывать из main loop рядом с dhcp_pump/sti+hlt.  Идемпотентна.
void           e1000_poll_rx(void);

#endif
