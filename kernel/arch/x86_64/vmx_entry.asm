; ============================================================================
; vmx_entry.asm — вход в гостя и возврат из него (этап 50.1).
; ----------------------------------------------------------------------------
;   int vmx_enter(vmx_regs_t* regs, int launched);
;     rdi = регистры гостя (раскладка — vmx_regs_t в hv.h, смещения ниже),
;     esi = 0 — первый вход (VMLAUNCH), иначе VMRESUME.
;   Возврат: 0 — гость исполнялся и случился VM exit;
;            1 — VMfailInvalid (CF): нет текущей VMCS;
;            2 — VMfailValid (ZF): вход отклонён, код — в поле VMCS 0x4400.
;
; Почему на ассемблере: между загрузкой регистров гостя и VMLAUNCH нельзя
; исполнить ни одной команды, сгенерированной компилятором, — он считает
; регистры своими. А после VM exit процессор попадает не «за VMLAUNCH», а в
; адрес из поля HOST_RIP со стеком из HOST_RSP: единственное, что там
; известно, — то, что мы сами положили на вершину стека перед входом.
;
; RSP гостя живёт в VMCS, поэтому в vmx_regs_t его нет. XMM здесь не трогаем:
; состояние FPU/SSE хоста и гостя меняет вызывающий (fxsave/fxrstor в hv.c).
; ============================================================================
[bits 64]

global vmx_enter
global vmx_exit_point

%define R_RAX   0
%define R_RBX   8
%define R_RCX   16
%define R_RDX   24
%define R_RSI   32
%define R_RDI   40
%define R_RBP   48
%define R_R8    56
%define R_R9    64
%define R_R10   72
%define R_R11   80
%define R_R12   88
%define R_R13   96
%define R_R14   104
%define R_R15   112
%define R_CR2   120
%define R_HRSP  128               ; кэш значения HOST_RSP

%define VMCS_H_RSP 0x6C14

section .text

vmx_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi                      ; [rsp] = regs — найдём его после VM exit

    ; HOST_RSP пишем, только если стек сместился: под вложенной
    ; виртуализацией каждый VMWRITE — выход в гипервизор узла.
    cmp  [rdi + R_HRSP], rsp
    je   .rsp_ok
    mov  [rdi + R_HRSP], rsp
    mov  eax, VMCS_H_RSP
    vmwrite rax, rsp
.rsp_ok:
    ; CR2 в VMCS не хранится: без этого гость увидел бы чужой адрес сбоя.
    mov  rax, [rdi + R_CR2]
    mov  cr2, rax

    ; Выбор LAUNCH/RESUME — ДО загрузки регистров: дальше идут только mov,
    ; флаги они не меняют.
    test esi, esi

    mov  rax, [rdi + R_RAX]
    mov  rbx, [rdi + R_RBX]
    mov  rcx, [rdi + R_RCX]
    mov  rdx, [rdi + R_RDX]
    mov  rsi, [rdi + R_RSI]
    mov  rbp, [rdi + R_RBP]
    mov  r8,  [rdi + R_R8]
    mov  r9,  [rdi + R_R9]
    mov  r10, [rdi + R_R10]
    mov  r11, [rdi + R_R11]
    mov  r12, [rdi + R_R12]
    mov  r13, [rdi + R_R13]
    mov  r14, [rdi + R_R14]
    mov  r15, [rdi + R_R15]
    mov  rdi, [rdi + R_RDI]       ; rdi — последним: через него шли все остальные
    jnz  .resume
    vmlaunch
    jmp  .fail
.resume:
    vmresume
.fail:
    ; Сюда попадаем, только если вход НЕ состоялся (иначе управление вернётся
    ; в vmx_exit_point). Стек цел; в регистрах — значения гостя.
    mov  eax, 1                   ; mov флагов не трогает
    jc   .fail_out
    mov  eax, 2
.fail_out:
    pop  rdi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret

; HOST_RIP. RSP = HOST_RSP, то есть [rsp] = regs. RFLAGS = 2 (IF=0, DF=0).
vmx_exit_point:
    xchg rdi, [rsp]               ; rdi = regs, на стеке — RDI гостя
    mov  [rdi + R_RAX], rax
    mov  [rdi + R_RBX], rbx
    mov  [rdi + R_RCX], rcx
    mov  [rdi + R_RDX], rdx
    mov  [rdi + R_RSI], rsi
    mov  [rdi + R_RBP], rbp
    mov  [rdi + R_R8],  r8
    mov  [rdi + R_R9],  r9
    mov  [rdi + R_R10], r10
    mov  [rdi + R_R11], r11
    mov  [rdi + R_R12], r12
    mov  [rdi + R_R13], r13
    mov  [rdi + R_R14], r14
    mov  [rdi + R_R15], r15
    pop  rax                      ; RDI гостя
    mov  [rdi + R_RDI], rax
    mov  rax, cr2
    mov  [rdi + R_CR2], rax
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    xor  eax, eax
    ret
