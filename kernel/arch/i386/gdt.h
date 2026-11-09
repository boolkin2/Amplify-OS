// ============================================================================
// gdt.h - Global Descriptor Table (GDT) для MyOS
// ============================================================================
// GDT определяет сегменты памяти в Protected Mode
// Каждый дескриптор описывает сегмент: базовый адрес, лимит, права доступа
// ============================================================================

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// ============================================================================
// Структуры GDT
// ============================================================================

// Дескриптор сегмента GDT (8 байт)
// Формат дескриптора в Protected Mode:
//
// 63        56 55  52 51  48 47        40 39        32
// +----------+------+------+------------+------------+
// | Base     | Flags| Limit| Access     | Base       |
// | [31:24]  |      |[19:16]| Byte       | [23:16]    |
// +----------+------+------+------------+------------+
//
// 31                16 15                 0
// +------------------+--------------------+
// | Base [15:0]      | Limit [15:0]       |
// +------------------+--------------------+

struct gdt_entry {
    uint16_t limit_low;      // Лимит сегмента (биты 0-15)
    uint16_t base_low;       // Базовый адрес (биты 0-15)
    uint8_t  base_middle;    // Базовый адрес (биты 16-23)
    uint8_t  access;         // Байт доступа (флаги прав)
    uint8_t  granularity;    // Гранулярность и лимит (биты 16-19)
    uint8_t  base_high;      // Базовый адрес (биты 24-31)
} __attribute__((packed));   // Без выравнивания

typedef struct gdt_entry gdt_entry_t;

// Указатель на GDT (GDTR register)
// Загружается инструкцией LGDT
struct gdt_ptr {
    uint16_t limit;          // Размер GDT - 1
    uint32_t base;           // Адрес начала GDT
} __attribute__((packed));

typedef struct gdt_ptr gdt_ptr_t;

// ============================================================================
// Структура Task State Segment (TSS)
// ============================================================================

struct tss_entry_struct {
    uint32_t prev_tss;   // Предыдущий TSS (если используется аппаратное переключение)
    uint32_t esp0;       // Стек кольца 0 (Ring 0)
    uint32_t ss0;        // Сегмент стека кольца 0
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;         
    uint32_t cs;        
    uint32_t ss;        
    uint32_t ds;        
    uint32_t fs;        
    uint32_t gs;        
    uint32_t ldt;       
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

typedef struct tss_entry_struct tss_entry_t;

// ============================================================================
// Константы для байта доступа (access byte)
// ============================================================================

// Биты байта доступа:
// 7: Present (P) - сегмент присутствует в памяти
// 6-5: Privilege Level (DPL) - уровень привилегий (0-3)
// 4: Descriptor Type (S) - 1 для кода/данных, 0 для системных
// 3: Executable (E) - 1 для кода, 0 для данных
// 2: Direction/Conforming (DC)
// 1: Readable/Writable (RW)
// 0: Accessed (A) - устанавливается процессором при обращении

#define GDT_ACCESS_PRESENT      0x80    // Бит 7: Present
#define GDT_ACCESS_PRIV_RING0   0x00    // Биты 6-5: Ring 0 (ядро)
#define GDT_ACCESS_PRIV_RING3   0x60    // Биты 6-5: Ring 3 (пользователь)
#define GDT_ACCESS_CODE_DATA    0x10    // Бит 4: Код/данные (не системный)
#define GDT_ACCESS_EXECUTABLE   0x08    // Бит 3: Исполняемый (код)
#define GDT_ACCESS_DIRECTION    0x04    // Бит 2: Direction (для данных)
#define GDT_ACCESS_RW           0x02    // Бит 1: Readable/Writable
#define GDT_ACCESS_ACCESSED     0x01    // Бит 0: Accessed

// Готовые комбинации для типичных сегментов
#define GDT_ACCESS_KERNEL_CODE  (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING0 | \
                                 GDT_ACCESS_CODE_DATA | GDT_ACCESS_EXECUTABLE | \
                                 GDT_ACCESS_RW)

#define GDT_ACCESS_KERNEL_DATA  (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING0 | \
                                 GDT_ACCESS_CODE_DATA | GDT_ACCESS_RW)

#define GDT_ACCESS_USER_CODE    (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING3 | \
                                 GDT_ACCESS_CODE_DATA | GDT_ACCESS_EXECUTABLE | \
                                 GDT_ACCESS_RW)

#define GDT_ACCESS_USER_DATA    (GDT_ACCESS_PRESENT | GDT_ACCESS_PRIV_RING3 | \
                                 GDT_ACCESS_CODE_DATA | GDT_ACCESS_RW)

// ============================================================================
// Константы для байта гранулярности (granularity byte)
// ============================================================================

// Биты байта гранулярности:
// 7: Granularity (G) - 0 = лимит в байтах, 1 = лимит в 4KB страницах
// 6: Size (D/B) - 0 = 16-bit, 1 = 32-bit
// 5: Long mode (L) - 1 для 64-bit (не используется в 32-bit)
// 4: Reserved (должен быть 0)
// 3-0: Limit [19:16] - старшие 4 бита лимита

#define GDT_GRAN_4K             0x80    // Бит 7: Гранулярность 4KB
#define GDT_GRAN_BYTE           0x00    // Бит 7: Гранулярность 1 байт
#define GDT_GRAN_32BIT          0x40    // Бит 6: 32-bit сегмент
#define GDT_GRAN_16BIT          0x00    // Бит 6: 16-bit сегмент
#define GDT_GRAN_LONG_MODE      0x20    // Бит 5: 64-bit (Long Mode)

// Стандартная комбинация для 32-bit Protected Mode
#define GDT_GRAN_STANDARD       (GDT_GRAN_4K | GDT_GRAN_32BIT)

// ============================================================================
// Индексы сегментов в GDT
// ============================================================================

#define GDT_NULL_SEGMENT        0       // Нулевой дескриптор (обязателен)
#define GDT_KERNEL_CODE         1       // Сегмент кода ядра
#define GDT_KERNEL_DATA         2       // Сегмент данных ядра
#define GDT_USER_CODE           3       // Сегмент кода пользователя
#define GDT_USER_DATA           4       // Сегмент данных пользователя

#define GDT_TSS                 5       // Task State Segment

#define GDT_ENTRY_COUNT         6       // Всего дескрипторов

// Селекторы сегментов (индекс * 8)
// Формат селектора: [Index:13][TI:1][RPL:2]
// TI = 0 (GDT), RPL = уровень привилегий

#define KERNEL_CODE_SELECTOR    0x08    // 1 * 8, RPL=0
#define KERNEL_DATA_SELECTOR    0x10    // 2 * 8, RPL=0
#define USER_CODE_SELECTOR      0x18    // 3 * 8, RPL=0
#define USER_DATA_SELECTOR      0x20    // 4 * 8, RPL=0
#define TSS_SELECTOR            0x28    // 5 * 8, RPL=0

// ============================================================================
// Функции для работы с GDT
// ============================================================================

// Инициализация GDT
void gdt_init(void);

// Установка дескриптора в GDT
void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, 
                  uint8_t access, uint8_t gran);

// Загрузка GDT в процессор (реализована в gdt_flush.asm)
extern void gdt_flush(uint32_t gdt_ptr_addr);

// Инициализация TSS и загрузка Task Register (LTR)
void tss_init(uint32_t idx, uint32_t kernel_ss, uint32_t kernel_esp);

// Установить новый стек для Ring 0 (вызывается при переключении контекста)
void tss_set_stack(uint32_t kernel_ss, uint32_t kernel_esp);

// Загрузка Task Register (реализована в gdt_flush.asm)
extern void tss_flush(void);

#endif // GDT_H
