// ============================================================================
// rtl8139.h — RTL8139 PCI Ethernet driver для AMPLIFY-OS x86_64.
// 1:1 по семантике с i386, но IRQ через idt64_set_handler и адаса 64-битные.
// ============================================================================
#ifndef AOS_X86_64_RTL8139_H
#define AOS_X86_64_RTL8139_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RTL8139_VENDOR_ID   0x10EC
#define RTL8139_DEVICE_ID   0x8139

// Регистры (offset от BAR0).
#define RTL_REG_MAC0        0x00
#define RTL_REG_TSD0        0x10
#define RTL_REG_TSAD0       0x20
#define RTL_REG_RBSTART     0x30
#define RTL_REG_CR          0x37
#define RTL_REG_CAPR        0x38
#define RTL_REG_CBR         0x3A
#define RTL_REG_IMR         0x3C
#define RTL_REG_ISR         0x3E
#define RTL_REG_TCR         0x40
#define RTL_REG_RCR         0x44
#define RTL_REG_CONFIG1     0x52

// CR.
#define RTL_CR_BUFE         (1u << 0)
#define RTL_CR_TE           (1u << 2)
#define RTL_CR_RE           (1u << 3)
#define RTL_CR_RST          (1u << 4)

// ISR/IMR.
#define RTL_INT_ROK         (1u << 0)
#define RTL_INT_RER         (1u << 1)
#define RTL_INT_TOK         (1u << 2)
#define RTL_INT_TER         (1u << 3)
#define RTL_INT_RXOVW       (1u << 4)

// RCR.
#define RTL_RCR_AAP         (1u << 0)
#define RTL_RCR_APM         (1u << 1)
#define RTL_RCR_AM          (1u << 2)
#define RTL_RCR_AB          (1u << 3)
#define RTL_RCR_MXDMA_1024  (6u << 8)
#define RTL_RCR_RBLEN_8K    (0u << 11)

// TCR.
#define RTL_TCR_MXDMA_2048  (7u << 8)
#define RTL_TCR_IFG_STD     (3u << 24)

// TSD.
#define RTL_TSD_OWN         (1u << 13)
#define RTL_TSD_TOK         (1u << 15)

#define RTL_RX_BUF_SIZE     8192
#define RTL_RX_BUF_PAD      16
#define RTL_RX_BUF_WRAP     1500
#define RTL_RX_BUF_TOTAL    (RTL_RX_BUF_SIZE + RTL_RX_BUF_PAD + RTL_RX_BUF_WRAP)
#define RTL_TX_BUF_COUNT    4
#define RTL_TX_BUF_SIZE     2048

bool           rtl8139_init(void);
const uint8_t* rtl8139_get_mac(void);
size_t         rtl8139_send(const void* data, size_t len);
bool           rtl8139_is_ready(void);

#endif
