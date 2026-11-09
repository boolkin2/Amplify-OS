// ============================================================================
// pic.h — legacy 8259 PIC remap для AMPLIFY-OS x86_64 (этап 37.1).
// ----------------------------------------------------------------------------
// IRQ master (8259-1, ports 0x20/0x21) → векторы 0x20..0x27 (32..39)
// IRQ slave  (8259-2, ports 0xA0/0xA1) → векторы 0x28..0x2F (40..47)
// На этапе 37.3 заменим на LAPIC + IOAPIC.
// ============================================================================
#ifndef AOS_X86_64_PIC_H
#define AOS_X86_64_PIC_H

#include <stdint.h>

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_set_mask(uint8_t irq);     // выключает IRQ
void pic_clear_mask(uint8_t irq);   // включает IRQ
void pic_send_eoi(uint8_t irq);

#endif
