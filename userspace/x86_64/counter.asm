; ============================================================================
; userspace/x86_64/counter.asm — second user-программа для теста планировщика.
; ----------------------------------------------------------------------------
; Цикл из 4 итераций:
;   1) AOS_SYS_PID → читаем свой tid, подставляем в строку
;   2) AOS_SYS_WRITE → печатаем "[pid=N] tick I"
;   3) AOS_SYS_YIELD → отдаём CPU следующей задаче
;   4) inc i, повтор
; После цикла: AOS_SYS_EXIT(0)
;
; Если запустить две копии — в serial должно появиться чередование:
;   [pid=1] tick 0
;   [pid=2] tick 0
;   [pid=1] tick 1
;   [pid=2] tick 1
;   ...
; ============================================================================
[bits 64]

global _start

section .text
_start:
    xor  r12, r12                      ; r12 = i (итерация)
.loop:
    ; --- aos_pid → AL ---
    mov  rax, 0x106
    syscall
    add  al, '0'
    mov  [rel pid_char], al

    ; --- iter digit (0..3) → tick_char ---
    mov  rax, r12
    add  al, '0'
    mov  [rel tick_char], al

    ; --- aos_write(1, msg, msg_len) ---
    mov  rax, 0x101
    mov  rdi, 1
    lea  rsi, [rel msg]
    mov  rdx, msg_len
    syscall

    ; --- aos_yield ---
    mov  rax, 0x109
    syscall

    inc  r12
    cmp  r12, 4
    jl   .loop

    ; --- aos_exit(0) ---
    mov  rax, 0x100
    xor  rdi, rdi
    syscall

    ud2

section .data
msg:        db "[pid=?] tick ?", 10
msg_len     equ $ - msg
pid_char    equ msg + 5                 ; позиция '?' после "pid="
tick_char   equ msg + 13                ; позиция '?' после "tick "
