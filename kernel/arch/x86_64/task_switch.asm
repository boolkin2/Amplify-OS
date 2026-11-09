; ============================================================================
; task_switch.asm — голый context switch для round-robin планировщика 37.4c.
; ----------------------------------------------------------------------------
; Save/restore только callee-saved регистры (SysV ABI x86_64):
;   rbx, rbp, r12, r13, r14, r15
; Остальные (rax, rcx, rdx, rdi, rsi, r8-r11) считаются клобберами вызова —
; компилятор сам сохраняет их на стек, если нужно.
;
; ВАЖНО: переключение CR3 (vm_switch) делается ОТДЕЛЬНО в task.c — здесь
; только переключение стека. CR3 для kernel-кода не критичен (PML4[0] shared
; across all vm_space), а user-CR3 вступает в силу при следующем sysret.
; ============================================================================
[bits 64]

global context_switch
section .text

; void context_switch(uint64_t* old_rsp_save, uint64_t new_rsp);
;   RDI = адрес, куда сохранить текущий RSP
;   RSI = новый RSP, который загрузить
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov  [rdi], rsp        ; *old_rsp_save = current RSP (после 6 push'ей)
    mov  rsp, rsi          ; load new RSP

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret                    ; pops next 8 bytes from new stack as RIP
