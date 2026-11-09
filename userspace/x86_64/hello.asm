; ============================================================================
; userspace/x86_64/hello.asm — first real ELF64 user program for AMPLIFY-OS.
; ----------------------------------------------------------------------------
; Statically linked, без libc. Делает два syscall:
;   AOS_SYS_WRITE(1, msg, len)
;   AOS_SYS_EXIT(0)
;
; Линкер user.ld кладёт _start по адресу 0x80_0000_0000 (USER_BASE = PML4[1]).
;
; ABI:
;   rax = syscall_num
;   rdi/rsi/rdx/r10/r8/r9 = аргументы
; ============================================================================
[bits 64]

global _start

section .text
_start:
    mov  rax, 0x101                ; AOS_SYS_WRITE
    mov  rdi, 1                    ; fd = stdout
    lea  rsi, [rel msg]
    mov  rdx, msg_len
    syscall

    mov  rax, 0x100                ; AOS_SYS_EXIT
    xor  rdi, rdi                  ; exit code = 0
    syscall

    ud2

section .rodata
msg:    db ">>> Hello from REAL ELF64 program (loaded by elf64_load)!", 10
msg_len equ $ - msg
