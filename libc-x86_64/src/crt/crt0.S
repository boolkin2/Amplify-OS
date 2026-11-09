// ============================================================================
// crt0.S — точка входа всех ELF-программ AMPLIFY-OS x86_64 (этап 39).
// ----------------------------------------------------------------------------
// Ядро (kernel/arch/x86_64/elf.c → enter_user_mode) прыгает на _start, не
// настраивая SysV-стартовый фрейм (argc/argv/envp на стеке). На 39-м этапе
// мы синтезируем минимальный фрейм прямо здесь:
//
//   argc = 0
//   argv = { NULL }
//   envp = { NULL }
//
// После этого вызываем __libc_start_main(main, argc, argv, envp), который
// сделает atexit-инициализацию, вызовет main и передаст exit-код в _exit.
//
// Если ядро в будущем начнёт сюда передавать SysV-фрейм (этапы 44+, когда
// добавим execve с реальным argv/envp на user-стеке) — заменим этот файл
// на классический musl-стиль:
//
//   _start:
//     xor  rbp, rbp
//     pop  rdi          ; argc
//     mov  rsi, rsp     ; argv
//     lea  rdx, [rsp+8*rdi+8]   ; envp
//     and  rsp, -16
//     call __libc_start_main
//
// Пока используем «фиксированный» вариант.
// ============================================================================
    .intel_syntax noprefix
    .text

    .globl _start
    .type  _start, @function
_start:
    xor  rbp, rbp                 /* выход из цепочки фреймов */

    /* Готовим NULL-терминатор для argv/envp в .bss → null_arg/null_env. */
    lea  rdi, [rip + null_argv]   /* a2: argv */
    lea  rdx, [rip + null_envp]   /* a3: envp */
    xor  rsi, rsi                 /* a1: argc=0  (NB: чтобы avoid GCC warning,
                                                  main(void) тоже работает) */

    /* Выравнивание RSP на 16 байт перед `call` (SysV ABI). */
    and  rsp, -16

    /* Вызываем __libc_start_main(main, argc, argv, envp). */
    lea  rcx, [rip + main]        /* a4: main → R10 не нужно: тут SysV ABI! */
    mov  rdi, rcx                 /* main */
    mov  rsi, 0                   /* argc=0 */
    lea  rdx, [rip + null_argv]   /* argv = {NULL} */
    lea  rcx, [rip + null_envp]   /* envp = {NULL} */
    call __libc_start_main

    /* __libc_start_main не возвращается — на всякий случай ud2. */
    ud2
    .size _start, .-_start

    /* NULL-array для argv/envp.  Помещены в .data, чтобы линкер выделил
     * настоящие нулевые qword'ы, а не оставил BSS placeholder. */
    .data
    .align 8
null_argv:
    .quad 0
null_envp:
    .quad 0

    .section .note.GNU-stack, "", @progbits
