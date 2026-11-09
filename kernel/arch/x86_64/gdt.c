// ============================================================================
// gdt.c — runtime GDT64 + TSS для AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Layout:
//   [0] null
//   [1] kernel code  (DPL=0, L=1, type=code)
//   [2] kernel data  (DPL=0, type=data)
//   [3] user code    (DPL=3, L=1, type=code)
//   [4] user data    (DPL=3, type=data)
//   [5..6] TSS (16-байтный системный дескриптор)
// ============================================================================
#include "gdt.h"
#include "cpu_stats.h"
#include <stddef.h>

// Помещаем GDT в .bss, выровненный по 16 байт.
// 8 слотов × 8 байт: 0..5 для null/kCS/kDS/pad/uDS/uCS + 2 (6,7) под TSS BSP.
// [этап 50] Дальше — по два слота на TSS каждого AP (GDT_SEL_TSS_AP).
__attribute__((aligned(16))) static uint8_t  gdt_storage[(8 + 2 * (MAX_CPUS - 1)) * 8];
__attribute__((aligned(16))) static tss64_t  tss64;
__attribute__((aligned(16))) static tss64_t  tss_ap[MAX_CPUS];     // [0] не используется
static gdt_ptr_t gdt_ptr;

// gdt_flush.asm.
extern void gdt64_flush(uint64_t gdt_ptr_addr);
extern void gdt64_load_tss(uint16_t selector);

// Установка обычного 8-байтного дескриптора кода/данных по индексу.
static void set_segment(int idx, uint8_t access, uint8_t flags) {
    gdt_desc_t* d = (gdt_desc_t*)(gdt_storage + idx * 8);
    d->limit_low        = 0xFFFF;
    d->base_low         = 0;
    d->base_mid         = 0;
    d->access           = access;
    d->flags_limit_high = flags | 0x0F; // limit_high = 0xF, flags в верхних 4 битах
    d->base_high        = 0;
}

// Установка 16-байтного TSS-дескриптора начиная с индекса idx.
static void set_tss(int idx, uint64_t base, uint32_t limit) {
    gdt_tss_desc_t* d = (gdt_tss_desc_t*)(gdt_storage + idx * 8);
    d->limit_low        = limit & 0xFFFF;
    d->base_low         = base & 0xFFFF;
    d->base_mid         = (base >> 16) & 0xFF;
    d->access           = 0x89;             // P=1, DPL=0, type=Available 64-bit TSS (1001)
    d->flags_limit_high = ((limit >> 16) & 0x0F) | 0x00;  // G=0
    d->base_high        = (base >> 24) & 0xFF;
    d->base_upper       = (uint32_t)(base >> 32);
    d->reserved         = 0;
}

void gdt64_init(uint64_t kernel_stack_top) {
    // Очищаем GDT и TSS.
    for (size_t i = 0; i < sizeof(gdt_storage); i++) gdt_storage[i] = 0;
    for (size_t i = 0; i < sizeof(tss64); i++) ((uint8_t*)&tss64)[i] = 0;

    // [1] kernel code: P=1, DPL=0, S=1, type=Code(1010), L=1, G=1
    //   access = 1001_1010 = 0x9A,  flags = 1010_0000 = 0xA0 (G=1, D=0, L=1)
    set_segment(1, 0x9A, 0xA0);

    // [2] kernel data: P=1, DPL=0, S=1, type=Data(0010), G=1, D=1
    //   access = 1001_0010 = 0x92,  flags = 1100_0000 = 0xC0
    set_segment(2, 0x92, 0xC0);

    // [3] PAD slot — нужно чтобы STAR[63:48] = 0x18 указывал на «дыру»
    // между kernel-data и user-data. SYSRET считает базу так:
    //   user SS = STAR[63:48] + 8 = 0x20 (-> slot 4)
    //   user CS = STAR[63:48] + 16 = 0x28 (-> slot 5)
    set_segment(3, 0x00, 0x00);

    // [4] user data: P=1, DPL=3, S=1, type=Data(0010), G=1, D=1
    //   access = 1111_0010 = 0xF2
    set_segment(4, 0xF2, 0xC0);

    // [5] user code: P=1, DPL=3, S=1, type=Code(1010), L=1, G=1
    //   access = 1111_1010 = 0xFA
    set_segment(5, 0xFA, 0xA0);

    // [6..7] TSS
    tss64.rsp0 = kernel_stack_top;
    tss64.iomap_base = sizeof(tss64_t);
    set_tss(6, (uint64_t)&tss64, sizeof(tss64_t) - 1);

    // Указатель на GDT.
    gdt_ptr.limit = sizeof(gdt_storage) - 1;
    gdt_ptr.base  = (uint64_t)gdt_storage;

    // Загружаем GDT и TSS.
    gdt64_flush((uint64_t)&gdt_ptr);
    gdt64_load_tss(GDT_SEL_TSS);
}

void gdt64_set_kernel_stack(uint64_t rsp0) {
    tss64.rsp0 = rsp0;
}

// [этап 50] До этого AP жили на трёх дескрипторах трамплина и вовсе без TSS
// (TR = 0). Пока AP только считал матрицы, этого хватало. Но VM entry
// проверяет состояние ХОСТА и отказывает, если селектор TR нулевой, а после
// VM exit процессор грузит TR, GDTR и сегменты из полей VMCS — им нужна
// настоящая, общая с BSP таблица. Поэтому каждый AP при старте переходит на
// общий GDT и получает свой TSS: дескриптор TSS помечается «занят» командой
// ltr, значит один TSS на два ядра загрузить нельзя.
void gdt64_init_ap(int slot) {
    if (slot < 1 || slot >= MAX_CPUS) return;
    tss64_t* t = &tss_ap[slot];
    for (size_t i = 0; i < sizeof(*t); i++) ((uint8_t*)t)[i] = 0;
    // rsp0 остаётся 0: кода Ring 3 на AP нет, стек при прерывании не меняется.
    t->iomap_base = sizeof(tss64_t);
    set_tss(8 + 2 * (slot - 1), (uint64_t)t, sizeof(tss64_t) - 1);
    gdt64_flush((uint64_t)&gdt_ptr);
    gdt64_load_tss(GDT_SEL_TSS_AP(slot));
}
