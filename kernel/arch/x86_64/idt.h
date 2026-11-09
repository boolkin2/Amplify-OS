// ============================================================================
// idt.h — IDT64 для AMPLIFY-OS (256 векторов).
// ============================================================================
#ifndef AOS_X86_64_IDT_H
#define AOS_X86_64_IDT_H

#include <stdint.h>

// 16-байтная запись IDT в 64-битном режиме.
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          // 0..7 (0 = обычный стек из TSS.RSP0)
    uint8_t  type_attr;    // P|DPL|0|TYPE (E=interrupt gate, F=trap gate)
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

// Кадр регистров, который isr_common_stub строит на стеке перед вызовом C.
// Порядок ВАЖЕН — он зеркален к push/pop в isr.asm.
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;          // запушены стабом
    uint64_t rip, cs, rflags, rsp, ss;  // запушены процессором при int
} registers64_t;

void idt64_init(void);

// Регистрация C-обработчика для номера прерывания (0..255).
typedef void (*int_handler_t)(registers64_t*);
void idt64_set_handler(uint8_t vec, int_handler_t h);

// [41.2] Динамические векторы для MSI-X (гейты 80..95 есть в isr.asm).
#define IRQ_DYN_BASE   80
#define IRQ_DYN_COUNT  16
uint8_t irq_alloc_vector(int_handler_t h);   // 0 — свободных нет
// [41.2] Сколько аппаратных прерываний пришло, не считая тиков таймера LAPIC.
uint64_t irq_event_count(void);

// [этап 49] lidt общей таблицы на текущем ядре (для AP из smp_pool).
void idt64_load_on_this_cpu(void);

#endif
