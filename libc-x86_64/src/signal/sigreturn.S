// ============================================================================
// sigreturn.S — signal-wrapper для AMPLIFY-OS x86_64 (этап 40 группа B).
// ----------------------------------------------------------------------------
// Ядро при доставке сигнала подменяет user RIP на адрес __aos_sigreturn_tramp
// и RSP на начало signal-frame на user stack:
//
//   RSP --->   [magic]           (qword, 0xA05516EFCAFEBABE)
//              [signum]          (qword)
//              [saved_rsp]       (qword)
//              [saved_rip]       (qword)
//              [saved_rflags]    (qword)
//
// Наша задача (wrapper):
//   1) Достать signum из frame (RSP + 8).
//   2) Найти handler через sig_get_handler_for_frame(signum) syscall-вариант.
//      Но прямого syscall для этого нет; handler мы получаем через
//      AOS_SYS_SIGACTION-getter-семантику, что дорого.  Поэтому храним
//      зарегистрированные handler'ы в userspace-таблице __aos_user_handlers.
//   3) Вызвать handler(signum) с 16-байтно-выровненным RSP.
//   4) После возврата handler'а — сделать syscall SIGRETURN (0x118).
//      Ядро восстановит saved_rip/saved_rsp/saved_rflags.
// ============================================================================
    .intel_syntax noprefix
    .text

    // AOS_SYS_SIGRETURN = 0x118
    .equ AOS_SYS_SIGRETURN, 0x118

    .globl __aos_sigreturn_tramp
    .type  __aos_sigreturn_tramp, @function
__aos_sigreturn_tramp:
    // RSP указывает на frame (magic).  signum = [rsp + 8].
    mov  rdi, [rsp + 8]               // signum -> 1st arg
    // Берём handler из userspace-таблицы __aos_user_handlers[signum].
    // Для совместимости с PIC (libc.so) — через GOTPCREL косвенно.
    mov  rax, [rip + __aos_user_handlers@GOTPCREL]
    mov  rax, [rax + rdi * 8]
    test rax, rax
    jz   2f                            // NULL -> skip to sigreturn

    // Выравнивание стека: frame занимает 40 байт, поэтому после под-выравнивания
    // 16 на момент `call` нам нужно +8 байт (frame смещён).
    sub  rsp, 8
    call rax                           // handler(signum)
    add  rsp, 8
2:
    // Делаем syscall SIGRETURN. Номер в RAX.
    mov  rax, AOS_SYS_SIGRETURN
    syscall
    // Ядро подменит RIP/RSP/RFLAGS — сюда мы не вернёмся.
    ud2
    .size __aos_sigreturn_tramp, .-__aos_sigreturn_tramp

    .section .note.GNU-stack, "", @progbits
