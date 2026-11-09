// ============================================================================
// ac97.h — драйвер AC'97 (Intel ICH 82801AA) для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Порт kernel/drivers/ac97.h. AC'97 — фиксированный 16-bit stereo @ 48 kHz,
// DMA через 32-элементный BDL ring.  IRQ роутится через IOAPIC (как RTL8139),
// EOI идёт в LAPIC.
// ============================================================================
#ifndef AOS_X86_64_AC97_H
#define AOS_X86_64_AC97_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// NABM Playback box (BAR1 + offset).
#define AC97_PO_BDBAR   0x10
#define AC97_PO_CIV     0x14
#define AC97_PO_LVI     0x15
#define AC97_PO_SR      0x16
#define AC97_PO_PICB    0x18
#define AC97_PO_PIV     0x1A
#define AC97_PO_CR      0x1B

#define AC97_GLOB_CNT   0x2C
#define AC97_GLOB_STA   0x30

// NAM mixer (BAR0 + offset).
#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOLUME  0x02
#define AC97_NAM_PCM_OUT_VOL    0x18
#define AC97_NAM_POWERDOWN      0x26

#define AC97_SR_DCH         (1 << 0)
#define AC97_SR_LVBCI       (1 << 2)
#define AC97_SR_BCIS        (1 << 3)
#define AC97_SR_FIFOE       (1 << 4)

#define AC97_CR_RPBM        (1 << 0)
#define AC97_CR_RR          (1 << 1)
#define AC97_CR_LVBIE       (1 << 2)
#define AC97_CR_FEIE        (1 << 3)
#define AC97_CR_IOCE        (1 << 4)

#define AC97_BDL_ENTRIES        32
#define AC97_SAMPLES_PER_ENTRY  0x1000     // 4096 sample-pair (~43 ms @48k)

typedef struct __attribute__((packed)) {
    uint32_t buf_ptr;
    uint16_t num_samples;
    uint16_t flags;       // bit15=IOC, bit14=BUP
} ac97_bdl_entry_t;

bool   ac97_init(void);
bool   ac97_is_ready(void);
size_t ac97_play(const int16_t* samples, size_t sample_count, uint32_t sample_rate);
void   ac97_stop(void);
void   ac97_set_volume(uint8_t percent);

#endif
