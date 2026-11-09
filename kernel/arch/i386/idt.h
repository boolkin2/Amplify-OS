// ============================================================================
// idt.h - Interrupt Descriptor Table (IDT) для MyOS
// ============================================================================
// IDT определяет обработчики прерываний и исключений
// Каждый дескриптор указывает на функцию-обработчик
// ============================================================================

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// ============================================================================
// Структуры IDT
// ============================================================================

// Дескриптор прерывания в IDT (8 байт)
// Формат дескриптора:
//
// 63        48 47  45 44 43  40 39    32
// +----------+------+--+------+--------+
// | Offset   | Flags|0 | Type | Reserved|
// | [31:16]  |      |  |      |        |
// +----------+------+--+------+--------+
//
// 31        16 15                 0
// +----------+--------------------+
// | Selector | Offset [15:0]      |
// +----------+--------------------+

struct idt_entry {
    uint16_t offset_low;     // Младшие 16 бит адреса обработчика
    uint16_t selector;       // Селектор сегмента кода (обычно 0x08)
    uint8_t  zero;           // Всегда 0
    uint8_t  type_attr;      // Тип и атрибуты
    uint16_t offset_high;    // Старшие 16 бит адреса обработчика
} __attribute__((packed));

typedef struct idt_entry idt_entry_t;

// Указатель на IDT (IDTR register)
// Загружается инструкцией LIDT
struct idt_ptr {
    uint16_t limit;          // Размер IDT - 1
    uint32_t base;           // Адрес начала IDT
} __attribute__((packed));

typedef struct idt_ptr idt_ptr_t;

// ============================================================================
// Константы для type_attr (тип и атрибуты)
// ============================================================================

// Биты type_attr:
// 7: Present (P) - дескриптор присутствует
// 6-5: Privilege Level (DPL) - уровень привилегий (0-3)
// 4: Storage Segment (S) - 0 для системных дескрипторов
// 3-0: Gate Type - тип шлюза

// Типы шлюзов (Gate Types)
#define IDT_GATE_TASK       0x05    // Task Gate
#define IDT_GATE_INT16      0x06    // 16-bit Interrupt Gate
#define IDT_GATE_TRAP16     0x07    // 16-bit Trap Gate
#define IDT_GATE_INT32      0x0E    // 32-bit Interrupt Gate
#define IDT_GATE_TRAP32     0x0F    // 32-bit Trap Gate

// Флаги атрибутов
#define IDT_FLAG_PRESENT    0x80    // Бит 7: Present
#define IDT_FLAG_RING0      0x00    // Биты 6-5: Ring 0
#define IDT_FLAG_RING3      0x60    // Биты 6-5: Ring 3

// Стандартные комбинации для 32-bit Protected Mode
#define IDT_INT_GATE_32     (IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_GATE_INT32)
#define IDT_TRAP_GATE_32    (IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_GATE_TRAP32)
#define IDT_INT_GATE_USER   (IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_GATE_INT32)

// ============================================================================
// Константы прерываний
// ============================================================================

// Количество дескрипторов в IDT
#define IDT_ENTRY_COUNT     256

// Исключения CPU (0-31)
#define INT_DIVIDE_ERROR            0   // Деление на ноль
#define INT_DEBUG                   1   // Отладка
#define INT_NMI                     2   // Non-Maskable Interrupt
#define INT_BREAKPOINT              3   // Точка останова
#define INT_OVERFLOW                4   // Переполнение
#define INT_BOUND_RANGE             5   // Выход за границы
#define INT_INVALID_OPCODE          6   // Недопустимая инструкция
#define INT_DEVICE_NOT_AVAILABLE    7   // Устройство недоступно (FPU)
#define INT_DOUBLE_FAULT            8   // Двойная ошибка
#define INT_COPROCESSOR_SEGMENT     9   // Сегмент сопроцессора (устарело)
#define INT_INVALID_TSS             10  // Недопустимый TSS
#define INT_SEGMENT_NOT_PRESENT     11  // Сегмент отсутствует
#define INT_STACK_SEGMENT_FAULT     12  // Ошибка сегмента стека
#define INT_GENERAL_PROTECTION      13  // Общая защита
#define INT_PAGE_FAULT              14  // Ошибка страницы
#define INT_RESERVED_15             15  // Зарезервировано
#define INT_FPU_ERROR               16  // Ошибка FPU
#define INT_ALIGNMENT_CHECK         17  // Проверка выравнивания
#define INT_MACHINE_CHECK           18  // Проверка машины
#define INT_SIMD_FP_EXCEPTION       19  // SIMD исключение
#define INT_VIRTUALIZATION          20  // Виртуализация
// 21-31 зарезервированы

// IRQ (аппаратные прерывания) переназначаются на 32-47
#define IRQ_BASE                    32  // Базовый номер IRQ

#define IRQ0                        32  // Системный таймер (PIT)
#define IRQ1                        33  // Клавиатура
#define IRQ2                        34  // Каскад (подчинённый PIC)
#define IRQ3                        35  // COM2
#define IRQ4                        36  // COM1
#define IRQ5                        37  // LPT2
#define IRQ6                        38  // Floppy disk
#define IRQ7                        39  // LPT1
#define IRQ8                        40  // CMOS Real-time clock
#define IRQ9                        41  // Свободно
#define IRQ10                       42  // Свободно
#define IRQ11                       43  // Свободно
#define IRQ12                       44  // PS/2 мышь
#define IRQ13                       45  // FPU
#define IRQ14                       46  // Primary ATA
#define IRQ15                       47  // Secondary ATA

// ============================================================================
// Функции для работы с IDT
// ============================================================================

// Инициализация IDT
void idt_init(void);

// Установка дескриптора в IDT
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags);

// Загрузка IDT в процессор (реализована в idt_flush.asm)
extern void idt_flush(uint32_t idt_ptr_addr);

#endif // IDT_H
