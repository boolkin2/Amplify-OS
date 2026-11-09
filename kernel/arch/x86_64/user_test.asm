; ============================================================================
; user_test.asm — встроенная микро user-программа для smoke-теста этапа 37.4a.
; ----------------------------------------------------------------------------
; Лежит в отдельной секции .user_code (page-aligned) — чтобы можно было
; пометить её как PG_USER для CPU без захвата соседнего kernel-кода.
;
; Что делает:
;   1) syscall AOS_SYS_WRITE(fd=1, msg, len)
;   2) syscall AOS_SYS_EXIT(0)
;   3) ud2 (на случай если kernel вернёт нам управление, что не должно случиться)
;
; ABI:
;   rax = syscall #
;   rdi/rsi/rdx/r10/r8/r9 = args
; ============================================================================
[bits 64]

global user_test_start
global user_test_end

section .user_code progbits alloc exec nowrite align=4096

user_test_start:
    ; aos_write(1, msg, msg_len)
    mov  rax, 0x101                 ; AOS_SYS_WRITE
    mov  rdi, 1                     ; fd = stdout
    lea  rsi, [rel user_msg]
    mov  rdx, user_msg_len
    syscall

    ; aos_exit(0)
    mov  rax, 0x100                 ; AOS_SYS_EXIT
    xor  rdi, rdi
    syscall

    ; Не должно сюда дойти.
    ud2

align 8
user_msg:     db ">>> Hello from Ring 3 (Long Mode)! syscall/sysret works.", 10
user_msg_len: equ $ - user_msg

user_test_end:
