; ============================================================================
; user_mode.asm — Ring 0 → Ring 3 через IRETQ (первый вход в user mode).
; ----------------------------------------------------------------------------
; В long mode IRETQ снимает 5 qword'ов: SS, RSP, RFLAGS, CS, RIP.
; Мы строим этот frame руками и делаем iretq → CPU попадает в Ring 3.
;
; После этого вернуться в kernel можно только через:
;   * syscall (наш entry в syscall_entry.asm)
;   * прерывание (CPU автоматически переключается на TSS.RSP0 как kernel-стек)
;   * exception (#PF, #GP, ...)
; ============================================================================
[bits 64]

global enter_user_mode
global enter_user_mode_thread

section .text

; void enter_user_mode(uint64_t rip, uint64_t rsp);
;   RDI = rip, RSI = rsp
enter_user_mode:
    ; Очистим сегментные регистры на user_DS — это рекомендуемая чистка
    ; в long mode (хотя сегментация в основном игнорируется, нулевой DS
    ; портит SYSRET позже).
    mov  ax, 0x23                   ; user data selector (RPL=3)
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    ; Строим IRETQ frame в обратном порядке (CPU pop'ит сверху вниз).
    ; Layout (от вершины стека ниже после pushes):
    ;   [RSP+32]  SS
    ;   [RSP+24]  RSP_user
    ;   [RSP+16]  RFLAGS
    ;   [RSP+8]   CS
    ;   [RSP+0]   RIP
    push qword 0x23                 ; SS = user data
    push rsi                        ; RSP_user
    push qword 0x202                ; RFLAGS: IF=1, bit1=reserved=1
    push qword 0x2B                 ; CS = user code (0x28 | RPL=3)
    push rdi                        ; RIP

    ; Очищаем GP-регистры — чтобы не утечь kernel-данные в user mode.
    xor  rax, rax
    xor  rbx, rbx
    xor  rcx, rcx
    xor  rdx, rdx
    xor  rsi, rsi
    xor  rdi, rdi
    xor  rbp, rbp
    xor  r8,  r8
    xor  r9,  r9
    xor  r10, r10
    xor  r11, r11
    xor  r12, r12
    xor  r13, r13
    xor  r14, r14
    xor  r15, r15

    iretq

; ----------------------------------------------------------------------------
; Этап 40 группа D: enter_user_mode_thread(rip, rsp, rdi).
; Аналог enter_user_mode, но устанавливает RDI = arg (pthread entry arg).
; Сигнатура SysV: RDI=rip, RSI=rsp, RDX=arg.
; ----------------------------------------------------------------------------
enter_user_mode_thread:
    mov  ax, 0x23
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push qword 0x23            ; SS
    push rsi                   ; user RSP
    push qword 0x202           ; RFLAGS
    push qword 0x2B            ; CS
    push rdi                   ; RIP

    ; Устанавливаем RDI = user arg.
    mov  rdi, rdx

    xor  rax, rax
    xor  rbx, rbx
    xor  rcx, rcx
    xor  rdx, rdx
    xor  rsi, rsi
    xor  rbp, rbp
    xor  r8,  r8
    xor  r9,  r9
    xor  r10, r10
    xor  r11, r11
    xor  r12, r12
    xor  r13, r13
    xor  r14, r14
    xor  r15, r15

    iretq
