// ============================================================================
// interrupt.c - Общая логика обработки прерываний для MyOS
// ============================================================================
// Реализует обработчики исключений CPU и IRQ
// Позволяет регистрировать пользовательские обработчики
// ============================================================================

#include "isr.h"
#include "../../io.h"
#include "../../vga.h"
#include "../../memory/paging.h"
#include "../../security/security.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Массив пользовательских обработчиков прерываний
static isr_handler_t interrupt_handlers[256] = {0};

// Названия исключений CPU
static const char* exception_messages[] = {
    "Division By Zero",                 // 0
    "Debug",                            // 1
    "Non Maskable Interrupt",           // 2
    "Breakpoint",                       // 3
    "Overflow",                         // 4
    "Bound Range Exceeded",             // 5
    "Invalid Opcode",                   // 6
    "Device Not Available",             // 7
    "Double Fault",                     // 8
    "Coprocessor Segment Overrun",      // 9
    "Invalid TSS",                      // 10
    "Segment Not Present",              // 11
    "Stack-Segment Fault",              // 12
    "General Protection Fault",         // 13
    "Page Fault",                       // 14
    "Reserved",                         // 15
    "x87 FPU Error",                    // 16
    "Alignment Check",                  // 17
    "Machine Check",                    // 18
    "SIMD Floating-Point Exception",    // 19
    "Virtualization Exception",         // 20
    "Reserved",                         // 21
    "Reserved",                         // 22
    "Reserved",                         // 23
    "Reserved",                         // 24
    "Reserved",                         // 25
    "Reserved",                         // 26
    "Reserved",                         // 27
    "Reserved",                         // 28
    "Reserved",                         // 29
    "Reserved",                         // 30
    "Reserved"                          // 31
};

// ============================================================================
// Функции для работы с обработчиками
// ============================================================================

// Регистрация обработчика для конкретного прерывания
void isr_register_handler(uint8_t num, isr_handler_t handler) {
    interrupt_handlers[num] = handler;
}

// Удаление обработчика
void isr_unregister_handler(uint8_t num) {
    interrupt_handlers[num] = 0;
}

// ============================================================================
// Обработчики прерываний
// ============================================================================

// Общий обработчик исключений CPU (вызывается из isr.asm)
void isr_handler(registers_t* regs) {
    // ----------------------------------------------------------------
    // Этап 32: Ring 3 isolation.
    //   Если исключение пришло из Ring 3 (CS.RPL == 3) — это упало
    //   пользовательское приложение, а не ядро. Помечаем задачу как
    //   DEAD и крутим sti/hlt — таймер отдаст управление следующей
    //   задаче. Так одно сломавшееся приложение не валит всю ОС.
    //   Page fault для Ring 3 тоже сюда: paging-handler паникует, и
    //   нам это не нужно.
    // ----------------------------------------------------------------
    // ВАЖНО: вектор 128 (int 0x80) — это легитимный системный вызов из
    // Ring 3, а не неисправность приложения. Его нельзя гасить как
    // Ring 3 fault — иначе любое обращение к syscall API убивает
    // вызывающий процесс ещё ДО того, как диспетчер syscall'ов получит
    // управление. Поэтому пропускаем такие случаи в общий путь, где
    // ниже сработает зарегистрированный syscall_handler.
    if ((regs->cs & 0x3) == 3 && regs->int_no != 128) {
        uint32_t cr2 = 0;
        if (regs->int_no == 14) {
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        }
        // sec_ring3_handle_fault не возвращает управление, если есть
        // current_task. Если ещё нет — провалимся в стандартный путь.
        if (sec_ring3_handle_fault((int)regs->int_no, regs->err_code,
                                   regs->eip, cr2)) {
            return; /* unreachable */
        }
    }

    // Специальная обработка Page Fault (ISR 14)
    // CR2 содержит виртуальный адрес, вызвавший ошибку страницы
    if (regs->int_no == 14) {
        uint32_t fault_addr;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));
        paging_page_fault_handler(fault_addr, regs->err_code);
        return;  // paging_page_fault_handler останавливает систему
    }

    // Проверяем, есть ли зарегистрированный обработчик
    if (interrupt_handlers[regs->int_no] != 0) {
        // Вызываем пользовательский обработчик
        isr_handler_t handler = interrupt_handlers[regs->int_no];
        handler(regs);
    } else {
        // Обработчик не зарегистрирован - выводим информацию об исключении
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_println("");
        vga_println("========================================");
        vga_println("   ISKLYUCHENIE PROCESSORA (EXCEPTION)  ");
        vga_println("========================================");
        vga_println("");

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("Isklyuchenie: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        
        if (regs->int_no < 32) {
            vga_println(exception_messages[regs->int_no]);
        } else {
            vga_puts("Neizvestnoe isklyuchenie #");
            vga_print_dec(regs->int_no);
            vga_println("");
        }

        vga_println("");
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        
        // Выводим номер прерывания и код ошибки
        vga_puts("INT:  ");
        vga_print_dec(regs->int_no);
        vga_puts("  (");
        vga_print_hex(regs->int_no);
        vga_println(")");

        vga_puts("ERR:  ");
        vga_print_hex(regs->err_code);
        vga_println("");

        vga_println("");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_println("Sostoyanie registrov:");
        vga_println("");

        // Выводим регистры общего назначения
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("EAX: ");
        vga_print_hex(regs->eax);
        vga_puts("  EBX: ");
        vga_print_hex(regs->ebx);
        vga_puts("  ECX: ");
        vga_print_hex(regs->ecx);
        vga_puts("  EDX: ");
        vga_print_hex(regs->edx);
        vga_println("");

        vga_puts("ESI: ");
        vga_print_hex(regs->esi);
        vga_puts("  EDI: ");
        vga_print_hex(regs->edi);
        vga_puts("  EBP: ");
        vga_print_hex(regs->ebp);
        vga_puts("  ESP: ");
        vga_print_hex(regs->esp);
        vga_println("");

        vga_println("");
        vga_puts("EIP: ");
        vga_print_hex(regs->eip);
        vga_puts("  CS:  ");
        vga_print_hex(regs->cs);
        vga_puts("  DS:  ");
        vga_print_hex(regs->ds);
        vga_println("");

        vga_puts("EFLAGS: ");
        vga_print_hex(regs->eflags);
        vga_println("");

        vga_println("");
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Sistema ostanovlena.");
        vga_println("");

        // Останавливаем систему
        __asm__ volatile ("cli; hlt");
    }
}

// Общий обработчик IRQ (вызывается из irq.asm)
uint32_t irq_handler(registers_t* regs) {
    uint32_t current_esp = (uint32_t)regs;
    uint32_t new_esp = current_esp;

    // Отправляем EOI (End Of Interrupt) в PIC
    // Это сообщает PIC что прерывание обработано
    
    // Если IRQ пришёл от подчинённого PIC (IRQ 8-15)
    if (regs->int_no >= 40) {
        // Отправляем EOI подчинённому PIC
        outb(0xA0, 0x20);
    }
    
    // Всегда отправляем EOI главному PIC
    outb(0x20, 0x20);

    // Вызываем пользовательский обработчик, если он зарегистрирован
    if (interrupt_handlers[regs->int_no] != 0) {
        isr_handler_t handler = interrupt_handlers[regs->int_no];
        new_esp = handler(regs);
    }
    
    return new_esp;
}
