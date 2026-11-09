// ============================================================================
// isr.h - Interrupt Service Routines (обработчики прерываний)
// ============================================================================
// Определяет обработчики исключений CPU (0-31) и IRQ (32-47)
// ============================================================================

#ifndef ISR_H
#define ISR_H

#include <stdint.h>

// ============================================================================
// Структура регистров при прерывании
// ============================================================================

// Эта структура передаётся обработчику прерывания
// Содержит состояние всех регистров на момент прерывания
typedef struct {
    // Сегментные регистры (сохраняются вручную)
    uint32_t ds;                                    // Data Segment
    
    // Регистры общего назначения (сохраняются PUSHA)
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    
    // Номер прерывания и код ошибки
    uint32_t int_no;                                // Номер прерывания
    uint32_t err_code;                              // Код ошибки (если есть)
    
    // Автоматически сохраняются процессором
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

// ============================================================================
// Прототипы обработчиков исключений (ISR 0-31)
// ============================================================================
// Эти функции определены в isr.asm

extern void isr0(void);   // 0: Деление на ноль
extern void isr1(void);   // 1: Отладка
extern void isr2(void);   // 2: Non-Maskable Interrupt
extern void isr3(void);   // 3: Точка останова
extern void isr4(void);   // 4: Переполнение
extern void isr5(void);   // 5: Выход за границы
extern void isr6(void);   // 6: Недопустимая инструкция
extern void isr7(void);   // 7: Устройство недоступно
extern void isr8(void);   // 8: Двойная ошибка
extern void isr9(void);   // 9: Сегмент сопроцессора
extern void isr10(void);  // 10: Недопустимый TSS
extern void isr11(void);  // 11: Сегмент отсутствует
extern void isr12(void);  // 12: Ошибка сегмента стека
extern void isr13(void);  // 13: Общая защита
extern void isr14(void);  // 14: Ошибка страницы
extern void isr15(void);  // 15: Зарезервировано
extern void isr16(void);  // 16: Ошибка FPU
extern void isr17(void);  // 17: Проверка выравнивания
extern void isr18(void);  // 18: Проверка машины
extern void isr19(void);  // 19: SIMD исключение
extern void isr20(void);  // 20: Виртуализация
extern void isr21(void);  // 21: Зарезервировано
extern void isr22(void);  // 22: Зарезервировано
extern void isr23(void);  // 23: Зарезервировано
extern void isr24(void);  // 24: Зарезервировано
extern void isr25(void);  // 25: Зарезервировано
extern void isr26(void);  // 26: Зарезервировано
extern void isr27(void);  // 27: Зарезервировано
extern void isr28(void);  // 28: Зарезервировано
extern void isr29(void);  // 29: Зарезервировано
extern void isr30(void);  // 30: Зарезервировано
extern void isr31(void);  // 31: Зарезервировано

// Системный вызов (Syscall)
extern void isr128(void); // 128: int 0x80

// ============================================================================
// Прототипы обработчиков IRQ (32-47)
// ============================================================================
// Эти функции определены в irq.asm

extern void irq0(void);   // 32: Системный таймер (PIT)
extern void irq1(void);   // 33: Клавиатура
extern void irq2(void);   // 34: Каскад (подчинённый PIC)
extern void irq3(void);   // 35: COM2
extern void irq4(void);   // 36: COM1
extern void irq5(void);   // 37: LPT2
extern void irq6(void);   // 38: Floppy disk
extern void irq7(void);   // 39: LPT1
extern void irq8(void);   // 40: CMOS Real-time clock
extern void irq9(void);   // 41: Свободно
extern void irq10(void);  // 42: Свободно
extern void irq11(void);  // 43: Свободно
extern void irq12(void);  // 44: PS/2 мышь
extern void irq13(void);  // 45: FPU
extern void irq14(void);  // 46: Primary ATA
extern void irq15(void);  // 47: Secondary ATA

// ============================================================================
// Тип функции-обработчика прерывания
// ============================================================================

typedef uint32_t (*isr_handler_t)(registers_t* regs);

// ============================================================================
// Функции для работы с обработчиками
// ============================================================================

// Регистрация обработчика для конкретного прерывания
void isr_register_handler(uint8_t num, isr_handler_t handler);

// Удаление обработчика
void isr_unregister_handler(uint8_t num);

// Общий обработчик исключений (вызывается из isr.asm)
void isr_handler(registers_t* regs);

// Общий обработчик IRQ (вызывается из irq.asm). Возвращает новый ESP.
uint32_t irq_handler(registers_t* regs);

#endif // ISR_H
