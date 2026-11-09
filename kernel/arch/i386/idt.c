// ============================================================================
// idt.c - Реализация Interrupt Descriptor Table для MyOS
// ============================================================================

#include "idt.h"
#include "isr.h"
#include "../../io.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Массив дескрипторов IDT (256 записей)
static idt_entry_t idt_entries[IDT_ENTRY_COUNT];

// Указатель на IDT для загрузки в IDTR
static idt_ptr_t idt_ptr;

// ============================================================================
// Внутренние функции
// ============================================================================

// Установка дескриптора в IDT
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags) {
    // Адрес обработчика разбивается на две части (младшие и старшие 16 бит)
    idt_entries[num].offset_low  = handler & 0xFFFF;
    idt_entries[num].offset_high = (handler >> 16) & 0xFFFF;

    // Селектор сегмента кода (обычно 0x08 - код ядра)
    idt_entries[num].selector = selector;

    // Всегда 0
    idt_entries[num].zero = 0;

    // Тип и атрибуты
    idt_entries[num].type_attr = flags;
}

// Перепрограммирование PIC (Programmable Interrupt Controller)
// По умолчанию PIC отправляет IRQ 0-7 на прерывания 8-15,
// что конфликтует с исключениями CPU. Переназначаем на 32-47.
static void pic_remap(void) {
    // Сохраняем маски прерываний
    uint8_t mask1 = inb(0x21);
    uint8_t mask2 = inb(0xA1);

    // Начинаем инициализацию PIC
    outb(0x20, 0x11);    // ICW1: начать инициализацию (главный PIC)
    io_wait();
    outb(0xA0, 0x11);    // ICW1: начать инициализацию (подчинённый PIC)
    io_wait();

    // ICW2: устанавливаем базовые векторы прерываний
    outb(0x21, 0x20);    // Главный PIC: IRQ 0-7 → прерывания 32-39
    io_wait();
    outb(0xA1, 0x28);    // Подчинённый PIC: IRQ 8-15 → прерывания 40-47
    io_wait();

    // ICW3: настраиваем каскадирование
    outb(0x21, 0x04);    // Главный PIC: подчинённый на IRQ2
    io_wait();
    outb(0xA1, 0x02);    // Подчинённый PIC: каскад на IRQ2
    io_wait();

    // ICW4: устанавливаем режим 8086
    outb(0x21, 0x01);    // Режим 8086 (главный)
    io_wait();
    outb(0xA1, 0x01);    // Режим 8086 (подчинённый)
    io_wait();

    // Восстанавливаем маски прерываний
    outb(0x21, mask1);
    outb(0xA1, mask2);
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация IDT
void idt_init(void) {
    // Настройка указателя IDT
    idt_ptr.limit = (sizeof(idt_entry_t) * IDT_ENTRY_COUNT) - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    // Обнуляем все дескрипторы
    for (int i = 0; i < IDT_ENTRY_COUNT; i++) {
        idt_entries[i].offset_low  = 0;
        idt_entries[i].offset_high = 0;
        idt_entries[i].selector    = 0;
        idt_entries[i].zero        = 0;
        idt_entries[i].type_attr   = 0;
    }

    // Перепрограммируем PIC (переназначаем IRQ на 32-47)
    pic_remap();

    // Устанавливаем обработчики исключений CPU (0-31)
    // Эти функции определены в isr.asm
    idt_set_gate(0,  (uint32_t)isr0,  0x08, IDT_INT_GATE_32);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, IDT_INT_GATE_32);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, IDT_INT_GATE_32);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, IDT_INT_GATE_32);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, IDT_INT_GATE_32);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, IDT_INT_GATE_32);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, IDT_INT_GATE_32);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, IDT_INT_GATE_32);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, IDT_INT_GATE_32);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, IDT_INT_GATE_32);
    idt_set_gate(10, (uint32_t)isr10, 0x08, IDT_INT_GATE_32);
    idt_set_gate(11, (uint32_t)isr11, 0x08, IDT_INT_GATE_32);
    idt_set_gate(12, (uint32_t)isr12, 0x08, IDT_INT_GATE_32);
    idt_set_gate(13, (uint32_t)isr13, 0x08, IDT_INT_GATE_32);
    idt_set_gate(14, (uint32_t)isr14, 0x08, IDT_INT_GATE_32);
    idt_set_gate(15, (uint32_t)isr15, 0x08, IDT_INT_GATE_32);
    idt_set_gate(16, (uint32_t)isr16, 0x08, IDT_INT_GATE_32);
    idt_set_gate(17, (uint32_t)isr17, 0x08, IDT_INT_GATE_32);
    idt_set_gate(18, (uint32_t)isr18, 0x08, IDT_INT_GATE_32);
    idt_set_gate(19, (uint32_t)isr19, 0x08, IDT_INT_GATE_32);
    idt_set_gate(20, (uint32_t)isr20, 0x08, IDT_INT_GATE_32);
    idt_set_gate(21, (uint32_t)isr21, 0x08, IDT_INT_GATE_32);
    idt_set_gate(22, (uint32_t)isr22, 0x08, IDT_INT_GATE_32);
    idt_set_gate(23, (uint32_t)isr23, 0x08, IDT_INT_GATE_32);
    idt_set_gate(24, (uint32_t)isr24, 0x08, IDT_INT_GATE_32);
    idt_set_gate(25, (uint32_t)isr25, 0x08, IDT_INT_GATE_32);
    idt_set_gate(26, (uint32_t)isr26, 0x08, IDT_INT_GATE_32);
    idt_set_gate(27, (uint32_t)isr27, 0x08, IDT_INT_GATE_32);
    idt_set_gate(28, (uint32_t)isr28, 0x08, IDT_INT_GATE_32);
    idt_set_gate(29, (uint32_t)isr29, 0x08, IDT_INT_GATE_32);
    idt_set_gate(30, (uint32_t)isr30, 0x08, IDT_INT_GATE_32);
    idt_set_gate(31, (uint32_t)isr31, 0x08, IDT_INT_GATE_32);

    // Устанавливаем системный вызов (int 0x80 = 128)
    // IDT_INT_GATE_USER позволяет вызывать прерывание из Ring 3 (DPL=3)
    idt_set_gate(128, (uint32_t)isr128, 0x08, IDT_INT_GATE_USER);

    // Устанавливаем обработчики IRQ (32-47)
    // Эти функции определены в irq.asm
    idt_set_gate(32, (uint32_t)irq0,  0x08, IDT_INT_GATE_32);
    idt_set_gate(33, (uint32_t)irq1,  0x08, IDT_INT_GATE_32);
    idt_set_gate(34, (uint32_t)irq2,  0x08, IDT_INT_GATE_32);
    idt_set_gate(35, (uint32_t)irq3,  0x08, IDT_INT_GATE_32);
    idt_set_gate(36, (uint32_t)irq4,  0x08, IDT_INT_GATE_32);
    idt_set_gate(37, (uint32_t)irq5,  0x08, IDT_INT_GATE_32);
    idt_set_gate(38, (uint32_t)irq6,  0x08, IDT_INT_GATE_32);
    idt_set_gate(39, (uint32_t)irq7,  0x08, IDT_INT_GATE_32);
    idt_set_gate(40, (uint32_t)irq8,  0x08, IDT_INT_GATE_32);
    idt_set_gate(41, (uint32_t)irq9,  0x08, IDT_INT_GATE_32);
    idt_set_gate(42, (uint32_t)irq10, 0x08, IDT_INT_GATE_32);
    idt_set_gate(43, (uint32_t)irq11, 0x08, IDT_INT_GATE_32);
    idt_set_gate(44, (uint32_t)irq12, 0x08, IDT_INT_GATE_32);
    idt_set_gate(45, (uint32_t)irq13, 0x08, IDT_INT_GATE_32);
    idt_set_gate(46, (uint32_t)irq14, 0x08, IDT_INT_GATE_32);
    idt_set_gate(47, (uint32_t)irq15, 0x08, IDT_INT_GATE_32);

    // Загружаем IDT в процессор
    idt_flush((uint32_t)&idt_ptr);
}
