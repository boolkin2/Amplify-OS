// ============================================================================
// gdt.h — runtime GDT/TSS для 64-битного ядра AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Ранний GDT в boot.asm (3 дескриптора: null, kcode, kdata) — достаточен для
// перехода в Long Mode. Здесь мы перестраиваем "большой" GDT с TSS, чтобы
// в дальнейшем добавить user code/data и переключение Ring 0 ↔ Ring 3.
// ============================================================================
#ifndef AOS_X86_64_GDT_H
#define AOS_X86_64_GDT_H

#include <stdint.h>

// Селекторы (RPL=0, TI=0 → просто индекс × 8).
// ----------------------------------------------------------------------------
// ВАЖНО: layout под SYSRET. Команда SYSRET читает базовый селектор из
// IA32_STAR[63:48] и возвращает CS = base+16, SS = base+8 (с RPL=3).
// Поэтому ПОРЯДОК должен быть: ... user_DS, user_CS, ... — иначе SYSRET
// загрузит мусор в CS/SS и упадёт #GP сразу при возврате в Ring 3.
//
//   slot 0: null
//   slot 1: kernel CS (0x08)
//   slot 2: kernel DS (0x10)
//   slot 3: pad       (0x18) — STAR[63:48] = 0x18, через него SYSRET считает базу
//   slot 4: user DS   (0x20)         (= base + 8)
//   slot 5: user CS   (0x28)         (= base + 16)
//   slot 6,7: TSS     (0x30+0x38)    (16-байтный системный дескриптор)
// ----------------------------------------------------------------------------
#define GDT_SEL_NULL          0x00
#define GDT_SEL_KERNEL_CODE   0x08
#define GDT_SEL_KERNEL_DATA   0x10
#define GDT_SEL_USER_DATA     0x23    // 0x20 | RPL=3
#define GDT_SEL_USER_CODE     0x2B    // 0x28 | RPL=3
#define GDT_SEL_TSS           0x30
#define GDT_SYSRET_BASE       0x18    // STAR[63:48], база для SYSRET
// [этап 50] TSS ядра AP со слотом s >= 1: по 16 байт сразу за TSS BSP.
#define GDT_SEL_TSS_AP(s)     (0x40 + ((s) - 1) * 0x10)

// Стандартный 8-байтный дескриптор сегмента (для code/data).
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;     // верхние 4 бита flags (G/D/L/AVL) + low 4 bits limit
    uint8_t  base_high;
} gdt_desc_t;

// 16-байтный дескриптор TSS (System segment в long mode занимает 16 байт).
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_desc_t;

// 64-битный TSS. Поля используются для стеков прерываний (RSP0/IST1..7).
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

// Инициализирует runtime GDT: 5 segment-дескрипторов + 1 TSS-дескриптор.
// kernel_stack_top — RSP0, который CPU будет грузить при int из Ring 3.
void gdt64_init(uint64_t kernel_stack_top);

// Установить новый RSP0 для текущего CPU (для планировщика, этап 37.4).
void gdt64_set_kernel_stack(uint64_t rsp0);

// [этап 50] AP: перейти с GDT трамплина на общий GDT ядра и загрузить свой
// TSS. Звать на самом AP, один раз, до входа в цикл пула.
void gdt64_init_ap(int slot);

#endif
