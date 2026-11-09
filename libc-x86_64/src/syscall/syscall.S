// ============================================================================
// syscall.S — низкоуровневые SYSCALL-трамплины для AMPLIFY-OS x86_64 (этап 39).
// ----------------------------------------------------------------------------
// Все семь арити-вариантов — отдельные функции. Каждый сохраняет SysV
// callee-saved (только RBX мы трогаем для перемещения a4 в R10 в *5/6), ставит
// `num` в RAX и делает `syscall`. Возврат — в RAX, прокидываем напрямую.
//
// КРИТИЧНО: после `syscall` ядро испортит RCX (user RIP) и R11 (user RFLAGS).
// Это уже учтено SysV ABI как clobber. RBX/RBP/R12..R15 ядро не трогает (наш
// syscall_entry.asm их сохраняет).
//
// ABI вызова трамплина: SysV — RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5,
// stack=a6. Внутри мы должны переставить аргументы в syscall-ABI:
//   syscall_num     ← RDI  → RAX
//   arg1            ← RSI  → RDI
//   arg2            ← RDX  → RSI
//   arg3            ← RCX  → RDX
//   arg4            ← R8   → R10   (не RCX! RCX занят user RIP)
//   arg5            ← R9   → R8
//   arg6            ← [rsp+8] → R9
// ============================================================================
    .intel_syntax noprefix

    .text

    .globl __aos_syscall0
    .type  __aos_syscall0, @function
__aos_syscall0:
    mov  rax, rdi
    syscall
    ret
    .size __aos_syscall0, .-__aos_syscall0

    .globl __aos_syscall1
    .type  __aos_syscall1, @function
__aos_syscall1:
    mov  rax, rdi
    mov  rdi, rsi
    syscall
    ret
    .size __aos_syscall1, .-__aos_syscall1

    .globl __aos_syscall2
    .type  __aos_syscall2, @function
__aos_syscall2:
    mov  rax, rdi
    mov  rdi, rsi
    mov  rsi, rdx
    syscall
    ret
    .size __aos_syscall2, .-__aos_syscall2

    .globl __aos_syscall3
    .type  __aos_syscall3, @function
__aos_syscall3:
    mov  rax, rdi
    mov  rdi, rsi
    mov  rsi, rdx
    mov  rdx, rcx
    syscall
    ret
    .size __aos_syscall3, .-__aos_syscall3

    .globl __aos_syscall4
    .type  __aos_syscall4, @function
__aos_syscall4:
    mov  rax, rdi
    mov  rdi, rsi
    mov  rsi, rdx
    mov  rdx, rcx
    mov  r10, r8                /* a4 → R10 (SYSCALL ABI), не RCX */
    syscall
    ret
    .size __aos_syscall4, .-__aos_syscall4

    .globl __aos_syscall5
    .type  __aos_syscall5, @function
__aos_syscall5:
    mov  rax, rdi
    mov  rdi, rsi
    mov  rsi, rdx
    mov  rdx, rcx
    mov  r10, r8
    mov  r8,  r9
    syscall
    ret
    .size __aos_syscall5, .-__aos_syscall5

    .globl __aos_syscall6
    .type  __aos_syscall6, @function
__aos_syscall6:
    mov  rax, rdi
    mov  rdi, rsi
    mov  rsi, rdx
    mov  rdx, rcx
    mov  r10, r8
    mov  r8,  r9
    mov  r9,  [rsp + 8]         /* a6 со стека (1-й аргумент после ret-addr) */
    syscall
    ret
    .size __aos_syscall6, .-__aos_syscall6

    /* W^X — никакого .note.GNU-stack RWE */
    .section .note.GNU-stack, "", @progbits
