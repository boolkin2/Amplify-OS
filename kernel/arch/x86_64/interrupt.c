// ============================================================================
// interrupt.c — C-обработчики исключений для скелетного 64-битного ядра.
// ----------------------------------------------------------------------------
// На этапе 37.1 любое exception → паника с дампом регистров в serial+fb.
// На этапе 37.3 page fault получит реальный обработчик (vm.c), а syscall —
// будет вынесен в SYSCALL/SYSRET (этап 37.4).
// ============================================================================
#include "idt.h"
#include "fb.h"
#include "serial.h"
#include "io.h"

static const char* exception_name(uint64_t v) {
    static const char* names[] = {
        "Divide Error", "Debug", "NMI", "Breakpoint",
        "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
        "Segment Not Present", "Stack-Segment Fault", "General Protection",
        "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check",
        "Machine Check", "SIMD FP Exception", "Virtualization", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
    };
    if (v < 32) return names[v];
    return "?";
}

static void panic_dump(registers64_t* r) {
    fb_console_set_color(0xFFFFFF, 0xC03030);
    fb_console_puts("\n*** AMPLIFY-OS PANIC ***\n");

    serial_puts("\n*** AMPLIFY-OS PANIC ***\n");

    fb_console_puts("Exception #"); fb_console_print_dec(r->int_no);
    fb_console_puts(" ("); fb_console_puts(exception_name(r->int_no)); fb_console_puts(")\n");

    serial_puts("Exception #"); serial_print_dec(r->int_no);
    serial_puts(" ("); serial_puts(exception_name(r->int_no)); serial_puts(")\n");

    fb_console_puts("err_code="); fb_console_print_hex(r->err_code, 16);
    fb_console_puts("  RIP="); fb_console_print_hex(r->rip, 16);
    fb_console_puts("\nRSP="); fb_console_print_hex(r->rsp, 16);
    fb_console_puts(" RFLAGS="); fb_console_print_hex(r->rflags, 16);
    fb_console_puts("\nRAX="); fb_console_print_hex(r->rax, 16);
    fb_console_puts(" RBX="); fb_console_print_hex(r->rbx, 16);
    fb_console_puts("\nRCX="); fb_console_print_hex(r->rcx, 16);
    fb_console_puts(" RDX="); fb_console_print_hex(r->rdx, 16);
    fb_console_puts("\nRSI="); fb_console_print_hex(r->rsi, 16);
    fb_console_puts(" RDI="); fb_console_print_hex(r->rdi, 16);
    fb_console_puts("\nRBP="); fb_console_print_hex(r->rbp, 16);
    fb_console_puts(" R8 ="); fb_console_print_hex(r->r8,  16);
    fb_console_puts("\nR9 ="); fb_console_print_hex(r->r9,  16);
    fb_console_puts(" R10="); fb_console_print_hex(r->r10, 16);
    fb_console_puts("\n");

    serial_puts("err="); serial_print_hex(r->err_code, 16);
    serial_puts(" RIP="); serial_print_hex(r->rip, 16);
    serial_puts(" RSP="); serial_print_hex(r->rsp, 16);
    serial_puts("\n");

    if (r->int_no == 14) {
        // Page fault — в CR2 адрес.
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        fb_console_puts("CR2="); fb_console_print_hex(cr2, 16); fb_console_puts("\n");
        serial_puts("CR2="); serial_print_hex(cr2, 16); serial_puts("\n");
    }

    cli();
    for (;;) hlt();
}

// Обработчики, которые регистрирует kernel_main64.
// Этап 40 группа B: page fault в userspace → SIGSEGV вместо паники.
extern void sig_deliver_segv(void);

void exception_panic(registers64_t* r) {
    // Page fault из userspace (CS имеет RPL=3, т.е. низший бит = 1 при 0x2B).
    // Отправляем SIGSEGV текущей задаче; если handler не установлен — sig.c
    // сам вызовет task_exit(139).
    if (r->int_no == 14 && (r->cs & 3) == 3) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        serial_puts("[sig] SIGSEGV: user PF at CR2=0x");
        serial_print_hex(cr2, 16);
        serial_puts(" RIP=0x");
        serial_print_hex(r->rip, 16);
        serial_puts("\n");
        sig_deliver_segv();
        // На 40-м у нас нет преемптивного планировщика для возврата на
        // syscall_entry path — вместо этого завершаем задачу прямо сейчас.
        // Когда добавим PIT-preemption с signal-delivery в IRQ retpath,
        // этот путь заменится на возврат через kernel stack.
        extern void task_exit(int) __attribute__((noreturn));
        task_exit(139);
    }
    panic_dump(r);
}
