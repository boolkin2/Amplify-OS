#include "pic.h"
#include "io.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01
#define PIC_EOI    0x20

void pic_remap(uint8_t master_offset, uint8_t slave_offset) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // ICW1: начало инициализации.
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    // ICW2: новые векторы.
    outb(PIC1_DATA, master_offset); io_wait();
    outb(PIC2_DATA, slave_offset);  io_wait();

    // ICW3: каскад. Master знает про slave на IRQ2, slave знает свой ID = 2.
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    // ICW4: 8086 mode.
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Восстанавливаем маски (на этапе 37.1 включаем только IRQ0).
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port; uint8_t v;
    if (irq < 8) { port = PIC1_DATA; }
    else         { port = PIC2_DATA; irq -= 8; }
    v = inb(port) | (1 << irq);
    outb(port, v);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port; uint8_t v;
    if (irq < 8) { port = PIC1_DATA; }
    else         { port = PIC2_DATA; irq -= 8; }
    v = inb(port) & ~(1 << irq);
    outb(port, v);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
