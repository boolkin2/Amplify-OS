// ============================================================================
// ac97.c — реализация AC'97 driver для x86_64.
// IRQ — через IOAPIC + LAPIC EOI (как RTL8139).
// ============================================================================
#include "ac97.h"
#include "memory.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "idt.h"
#include "pic.h"
#include "ioapic.h"
#include "acpi.h"
#include "lapic.h"
#include "serial.h"

#define AC97_VENDOR_INTEL   0x8086
#define AC97_DEVICE_ICH     0x2415
#define AC97_DEVICE_ICH_2   0x2425
#define AC97_DEVICE_ICH_3   0x2445
#define AC97_DEVICE_ICH_4   0x2485
#define AC97_DEVICE_ICH_5   0x24C5

typedef struct {
    bool      ready;
    uint16_t  nam_base;
    uint16_t  nabm_base;
    uint8_t   irq;

    ac97_bdl_entry_t* bdl;
    int16_t*  buffers[AC97_BDL_ENTRIES];

    volatile uint8_t  head;
    volatile uint8_t  tail;
    volatile bool     running;
    volatile uint32_t irq_count;
} ac97_state_t;

static ac97_state_t ac97 = {0};

static inline void nam_w16(uint16_t off, uint16_t v) { outw(ac97.nam_base + off, v); }
static inline uint16_t nam_r16(uint16_t off) { return inw(ac97.nam_base + off); }

static inline void nabm_w8 (uint16_t off, uint8_t  v) { outb(ac97.nabm_base + off, v); }
static inline void nabm_w16(uint16_t off, uint16_t v) { outw(ac97.nabm_base + off, v); }
static inline void nabm_w32(uint16_t off, uint32_t v) { outl(ac97.nabm_base + off, v); }
static inline uint8_t  nabm_r8 (uint16_t off) { return inb(ac97.nabm_base + off); }
static inline uint16_t nabm_r16(uint16_t off) { return inw(ac97.nabm_base + off); }
static inline uint32_t nabm_r32(uint16_t off) { return inl(ac97.nabm_base + off); }

static void ac97_irq_handler(registers64_t* r) {
    (void)r;
    uint16_t sr = nabm_r16(AC97_PO_SR);
    uint16_t clear = sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
    if (clear) nabm_w16(AC97_PO_SR, clear);

    ac97.tail = nabm_r8(AC97_PO_CIV);
    ac97.irq_count++;

    if (sr & AC97_SR_DCH) ac97.running = false;

    lapic_eoi();
    pic_send_eoi(ac97.irq);
}

bool ac97_init(void) {
    pci_device_t dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_2);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_3);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_4);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_5);
    if (!dev.found) dev = pci_find_by_class(0x04, 0x01);

    if (!dev.found) { serial_puts("[ac97] not found — sound disabled\n"); return false; }

    ac97.nam_base  = (uint16_t)(pci_read_bar(&dev, 0) & 0xFFFF);
    ac97.nabm_base = (uint16_t)(pci_read_bar(&dev, 1) & 0xFFFF);
    ac97.irq       = dev.irq_line;

    serial_puts("[ac97] NAM=0x");  serial_print_hex(ac97.nam_base, 4);
    serial_puts(" NABM=0x");       serial_print_hex(ac97.nabm_base, 4);
    serial_puts(" IRQ=");          serial_print_dec(ac97.irq);
    serial_puts("\n");

    pci_enable_bus_master(&dev);

    // Cold reset.
    nabm_w32(AC97_GLOB_CNT, 0x00000002);
    for (int i = 0; i < 1000; i++) io_wait();

    nabm_w8(AC97_PO_CR, AC97_CR_RR);
    for (int i = 0; i < 1000; i++) io_wait();

    nam_w16(AC97_NAM_RESET, 0x0000);
    nam_w16(AC97_NAM_POWERDOWN, 0x0000);
    nam_w16(AC97_NAM_MASTER_VOLUME, 0x0000);
    nam_w16(AC97_NAM_PCM_OUT_VOL,   0x0000);

    // BDL + 32 buffers.
    ac97.bdl = (ac97_bdl_entry_t*)kmalloc_aligned(
        sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 16);
    if (!ac97.bdl) { serial_puts("[ac97] BDL OOM\n"); return false; }

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        size_t bufsz = AC97_SAMPLES_PER_ENTRY * sizeof(int16_t);
        ac97.buffers[i] = (int16_t*)kmalloc_aligned(bufsz, 16);
        if (!ac97.buffers[i]) { serial_puts("[ac97] buf OOM\n"); return false; }
        for (uint32_t j = 0; j < AC97_SAMPLES_PER_ENTRY; j++) ac97.buffers[i][j] = 0;
        uint64_t bp = virt_to_phys(ac97.buffers[i]);
        if (bp >> 32) { serial_puts("[ac97] FATAL buf above 4GB\n"); return false; }
        ac97.bdl[i].buf_ptr     = (uint32_t)bp;
        ac97.bdl[i].num_samples = AC97_SAMPLES_PER_ENTRY;
        ac97.bdl[i].flags       = 0;
    }

    uint64_t bdl_phys = virt_to_phys(ac97.bdl);
    if (bdl_phys >> 32) { serial_puts("[ac97] FATAL BDL above 4GB\n"); return false; }
    nabm_w32(AC97_PO_BDBAR, (uint32_t)bdl_phys);

    // IRQ через IOAPIC + PIC fallback (как RTL8139).
    if (ac97.irq < 16) {
        idt64_set_handler((uint8_t)(32 + ac97.irq), ac97_irq_handler);
        pic_clear_mask(ac97.irq);
        if (ac97.irq >= 8) pic_clear_mask(2);
        if (ioapic_present()) {
            uint8_t gsi = acpi_irq_to_gsi[ac97.irq];
            ioapic_route_irq(gsi, (uint8_t)(32 + ac97.irq),
                             (uint8_t)acpi_bsp_lapic_id, false);
        }
        serial_puts("[ac97] IRQ "); serial_print_dec(ac97.irq); serial_puts(" routed\n");
    }

    ac97.head = 0; ac97.tail = 0; ac97.running = false; ac97.irq_count = 0;
    ac97.ready = true;
    serial_puts("[ac97] init OK\n");
    return true;
}

bool ac97_is_ready(void) { return ac97.ready; }

void ac97_set_volume(uint8_t percent) {
    if (!ac97.ready) return;
    if (percent > 100) percent = 100;
    uint8_t att = (uint8_t)(((100 - percent) * 63) / 100);
    uint16_t val = ((uint16_t)att << 8) | att;
    nam_w16(AC97_NAM_MASTER_VOLUME, val);
    nam_w16(AC97_NAM_PCM_OUT_VOL,   val);
}

void ac97_stop(void) {
    if (!ac97.ready) return;
    nabm_w8(AC97_PO_CR, 0);
    nabm_w16(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
    ac97.running = false;
    ac97.head = 0; ac97.tail = 0;
}

size_t ac97_play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    (void)sample_rate;
    if (!ac97.ready || !samples || sample_count == 0) return 0;

    size_t written = 0;
    while (written < sample_count) {
        uint8_t next = (uint8_t)((ac97.head + 1) & (AC97_BDL_ENTRIES - 1));
        if (next == ac97.tail && ac97.running) {
            for (volatile int i = 0; i < 100000; i++) { }
            continue;
        }

        int16_t* dst = ac97.buffers[ac97.head];
        size_t   copy = sample_count - written;
        if (copy > AC97_SAMPLES_PER_ENTRY) copy = AC97_SAMPLES_PER_ENTRY;

        for (size_t i = 0; i < copy; i++) dst[i] = samples[written + i];
        for (size_t i = copy; i < AC97_SAMPLES_PER_ENTRY; i++) dst[i] = 0;

        ac97.bdl[ac97.head].num_samples = AC97_SAMPLES_PER_ENTRY;
        ac97.bdl[ac97.head].flags       = (1 << 15);   // IOC

        ac97.head = next;
        written += copy;

        uint8_t lvi = (uint8_t)((ac97.head + AC97_BDL_ENTRIES - 1) & (AC97_BDL_ENTRIES - 1));
        nabm_w8(AC97_PO_LVI, lvi);

        if (!ac97.running) {
            nabm_w16(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
            nabm_w8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_FEIE);
            ac97.running = true;
        }
    }
    return written;
}
