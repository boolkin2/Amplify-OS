// ============================================================================
// setjmp.S — non-local goto для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// jmp_buf layout:
//   [0] RBX, [1] RBP, [2] R12, [3] R13, [4] R14, [5] R15, [6] RSP, [7] RIP
// SysV ABI: callee-saved = RBX, RBP, R12..R15 — этого набора достаточно
// (RSP/RIP — обязательны, остальные caller-saved compiler сохранит сам).
// ============================================================================
    .intel_syntax noprefix
    .text

    .globl setjmp
    .type  setjmp, @function
setjmp:
    mov  [rdi + 0],  rbx
    mov  [rdi + 8],  rbp
    mov  [rdi + 16], r12
    mov  [rdi + 24], r13
    mov  [rdi + 32], r14
    mov  [rdi + 40], r15
    /* RSP сохраняем как «вызывающего» — без своего ret-фрейма. */
    lea  rax, [rsp + 8]
    mov  [rdi + 48], rax
    /* RIP — возвратный адрес: [rsp]. */
    mov  rax, [rsp]
    mov  [rdi + 56], rax
    xor  rax, rax              /* setjmp возвращает 0 в первый раз */
    ret
    .size setjmp, .-setjmp

    .globl longjmp
    .type  longjmp, @function
longjmp:
    mov  rax, rsi              /* val (нет преобразования; если 0 → 1) */
    test rax, rax
    jne  1f
    inc  rax
1:
    mov  rbx, [rdi + 0]
    mov  rbp, [rdi + 8]
    mov  r12, [rdi + 16]
    mov  r13, [rdi + 24]
    mov  r14, [rdi + 32]
    mov  r15, [rdi + 40]
    mov  rsp, [rdi + 48]
    /* RIP — прыгаем как обычный ret: пушим и retq. */
    push qword ptr [rdi + 56]
    ret
    .size longjmp, .-longjmp

    /* sigsetjmp/siglongjmp — sigmask не сохраняем, считаем savesigs=0. */
    .globl sigsetjmp
    .type  sigsetjmp, @function
sigsetjmp:
    /* То же, что setjmp; второй аргумент игнорим. */
    jmp  setjmp
    .size sigsetjmp, .-sigsetjmp

    .globl siglongjmp
    .type  siglongjmp, @function
siglongjmp:
    jmp  longjmp
    .size siglongjmp, .-siglongjmp

    .section .note.GNU-stack, "", @progbits
