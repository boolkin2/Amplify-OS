// ============================================================================
// gdt.c - Реализация Global Descriptor Table для MyOS
// ============================================================================

#include "gdt.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Массив дескрипторов GDT
static gdt_entry_t gdt_entries[GDT_ENTRY_COUNT];

// Указатель на GDT для загрузки в GDTR
static gdt_ptr_t gdt_ptr;

// Глобальная структура TSS
static tss_entry_t tss_entry;

// ============================================================================
// Внутренние функции
// ============================================================================

// Установка дескриптора в GDT
void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, 
                  uint8_t access, uint8_t gran) {
    // Проверка корректности индекса
    if (num < 0 || num >= GDT_ENTRY_COUNT) {
        return;
    }

    // Базовый адрес (32 бита разбиваются на 3 части)
    gdt_entries[num].base_low    = (base & 0xFFFF);           // Биты 0-15
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;       // Биты 16-23
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;       // Биты 24-31

    // Лимит сегмента (20 бит разбиваются на 2 части)
    gdt_entries[num].limit_low   = (limit & 0xFFFF);          // Биты 0-15
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;      // Биты 16-19

    // Байт гранулярности (старшие 4 бита) + лимит (младшие 4 бита)
    gdt_entries[num].granularity |= gran & 0xF0;

    // Байт доступа
    gdt_entries[num].access = access;
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация GDT
void gdt_init(void) {
    // Настройка указателя GDT
    gdt_ptr.limit = (sizeof(gdt_entry_t) * GDT_ENTRY_COUNT) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // Дескриптор 0: NULL дескриптор (обязателен по спецификации)
    // Все поля = 0
    gdt_set_gate(GDT_NULL_SEGMENT, 0, 0, 0, 0);

    // Дескриптор 1: Сегмент кода ядра (Ring 0)
    // База: 0x00000000, Лимит: 0xFFFFFFFF (4GB)
    // Доступ: Present, Ring 0, Code/Data, Executable, Readable
    // Гранулярность: 4KB, 32-bit
    gdt_set_gate(GDT_KERNEL_CODE, 
                 0,                         // База: 0
                 0xFFFFFFFF,                // Лимит: 4GB
                 GDT_ACCESS_KERNEL_CODE,    // Код ядра
                 GDT_GRAN_STANDARD);        // 4KB, 32-bit

    // Дескриптор 2: Сегмент данных ядра (Ring 0)
    // База: 0x00000000, Лимит: 0xFFFFFFFF (4GB)
    // Доступ: Present, Ring 0, Code/Data, Writable
    // Гранулярность: 4KB, 32-bit
    gdt_set_gate(GDT_KERNEL_DATA,
                 0,                         // База: 0
                 0xFFFFFFFF,                // Лимит: 4GB
                 GDT_ACCESS_KERNEL_DATA,    // Данные ядра
                 GDT_GRAN_STANDARD);        // 4KB, 32-bit

    // Дескриптор 3: Сегмент кода пользователя (Ring 3)
    // База: 0x00000000, Лимит: 0xFFFFFFFF (4GB)
    // Доступ: Present, Ring 3, Code/Data, Executable, Readable
    // Гранулярность: 4KB, 32-bit
    gdt_set_gate(GDT_USER_CODE,
                 0,                         // База: 0
                 0xFFFFFFFF,                // Лимит: 4GB
                 GDT_ACCESS_USER_CODE,      // Код пользователя
                 GDT_GRAN_STANDARD);        // 4KB, 32-bit

    // Дескриптор 4: Сегмент данных пользователя (Ring 3)
    // База: 0x00000000, Лимит: 0xFFFFFFFF (4GB)
    // Доступ: Present, Ring 3, Code/Data, Writable
    // Гранулярность: 4KB, 32-bit
    gdt_set_gate(GDT_USER_DATA,
                 0,                         // База: 0
                 0xFFFFFFFF,                // Лимит: 4GB
                 GDT_ACCESS_USER_DATA,      // Данные пользователя
                 GDT_GRAN_STANDARD);        // 4KB, 32-bit

    // Дескриптор 5: TSS
    // Инициализируется отдельно, так как требует заполнения структуры TSS
    tss_init(GDT_TSS, KERNEL_DATA_SELECTOR, 0);

    // Загрузка GDT в процессор
    // Вызываем ассемблерную функцию для загрузки GDTR
    gdt_flush((uint32_t)&gdt_ptr);

    // Только ПОСЛЕ загрузки новой GDT можно вызывать LTR!
    // Иначе ltr вызовет #GP, так как в старой GDT от GRUB нет TSS.
    tss_flush();
}

// Инициализация TSS
void tss_init(uint32_t idx, uint32_t kernel_ss, uint32_t kernel_esp) {
    uint32_t base = (uint32_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry_t) - 1;

    // Добавляем дескриптор TSS в GDT
    // Access: Present(1) | DPL(3) | 0 | Type(1001 = 32-bit TSS) = 0xE9
    // Granularity: 0x00
    gdt_set_gate(idx, base, limit, 0xE9, 0x00);

    // Очищаем TSS
    uint8_t* p = (uint8_t*)&tss_entry;
    for (uint32_t i = 0; i < sizeof(tss_entry_t); i++) {
        p[i] = 0;
    }

    // Настраиваем стек для Ring 0 (когда происходит прерывание в Ring 3)
    tss_entry.ss0 = kernel_ss;
    tss_entry.esp0 = kernel_esp;
    
    // Отключаем I/O bitmap (смещение за пределами TSS)
    tss_entry.iomap_base = sizeof(tss_entry_t);
}

// Установить стек ядра (используется при переключении контекста в планировщике)
void tss_set_stack(uint32_t kernel_ss, uint32_t kernel_esp) {
    tss_entry.ss0 = kernel_ss;
    tss_entry.esp0 = kernel_esp;
}
