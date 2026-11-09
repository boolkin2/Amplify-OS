; ============================================================================
; isr.asm — 32 exception + 16 IRQ + syscall стабы для AMPLIFY-OS x86_64.
; ----------------------------------------------------------------------------
; В отличие от 32-битного варианта, в 64-bit:
;   - нет pusha/popa: все 15 GP-регистров пушим руками
;   - аргумент C-функции передаётся в RDI (SysV ABI)
;   - error code для исключений 8,10,11,12,13,14,17 кладёт сам процессор
;   - iretq снимает rip,cs,rflags,rsp,ss (5×8 = 40 байт)
; ============================================================================
[bits 64]

extern isr_dispatch

section .text

; ----------------------------------------------------------------------------
; Макросы
; ----------------------------------------------------------------------------

; Exception БЕЗ кода ошибки: пушим фиктивный 0, потом номер вектора.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0            ; фиктивный err_code
    push qword %1           ; int_no
    jmp  isr_common
%endmacro

; Exception С кодом ошибки: процессор уже запушил err_code, мы — только vector.
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1           ; int_no
    jmp  isr_common
%endmacro

; IRQ: фиктивный err_code + смещённый vector (32..47), затем общий хвост.
%macro IRQ 2
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp  isr_common
%endmacro

; ----------------------------------------------------------------------------
; Exception стабы 0..31
; ----------------------------------------------------------------------------
ISR_NOERR 0      ; #DE Divide Error
ISR_NOERR 1      ; #DB Debug
ISR_NOERR 2      ; NMI
ISR_NOERR 3      ; #BP Breakpoint
ISR_NOERR 4      ; #OF Overflow
ISR_NOERR 5      ; #BR Bound range
ISR_NOERR 6      ; #UD Invalid opcode
ISR_NOERR 7      ; #NM Device not available
ISR_ERR   8      ; #DF Double fault
ISR_NOERR 9      ; (reserved)
ISR_ERR   10     ; #TS Invalid TSS
ISR_ERR   11     ; #NP Segment not present
ISR_ERR   12     ; #SS Stack-segment fault
ISR_ERR   13     ; #GP General protection
ISR_ERR   14     ; #PF Page fault
ISR_NOERR 15     ; (reserved)
ISR_NOERR 16     ; #MF x87 FP
ISR_ERR   17     ; #AC Alignment check
ISR_NOERR 18     ; #MC Machine check
ISR_NOERR 19     ; #XM SIMD FP
ISR_NOERR 20     ; #VE Virtualization
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ----------------------------------------------------------------------------
; IRQ 32..47 (legacy PIC после ремапа: master 0x20→32, slave 0x28→40)
; ----------------------------------------------------------------------------
IRQ 32, 0
IRQ 33, 1
IRQ 34, 2
IRQ 35, 3
IRQ 36, 4
IRQ 37, 5
IRQ 38, 6
IRQ 39, 7
IRQ 40, 8
IRQ 41, 9
IRQ 42, 10
IRQ 43, 11
IRQ 44, 12
IRQ 45, 13
IRQ 46, 14
IRQ 47, 15

; ----------------------------------------------------------------------------
; [41.2] Векторы 80..95 (0x50..0x5F) — под MSI-X устройств virtio. MSI-X пишет
; номер вектора прямо в LAPIC, минуя PIC/IOAPIC и таблицы маршрутизации ACPI,
; поэтому ему нужны свои гейты вне диапазона legacy IRQ (32..47).
; Раздаёт их irq_alloc_vector() в idt.c.
; ----------------------------------------------------------------------------
IRQ 80, 0
IRQ 81, 1
IRQ 82, 2
IRQ 83, 3
IRQ 84, 4
IRQ 85, 5
IRQ 86, 6
IRQ 87, 7
IRQ 88, 8
IRQ 89, 9
IRQ 90, 10
IRQ 91, 11
IRQ 92, 12
IRQ 93, 13
IRQ 94, 14
IRQ 95, 15

; ----------------------------------------------------------------------------
; Syscall (int 0x80) — пока тоже идёт в isr_common, на этапе 37.4 заменим на SYSCALL/SYSRET.
; ----------------------------------------------------------------------------
global isr128
isr128:
    push qword 0
    push qword 128
    jmp  isr_common

; ----------------------------------------------------------------------------
; LAPIC vectors (этап 37.3): 0xEF=timer, 0xFE=error, 0xFF=spurious.
; ----------------------------------------------------------------------------
global isr239
isr239:
    push qword 0
    push qword 239
    jmp  isr_common

; [этап 49] 0xF0 — IPI «проснись» для пула вычислительных ядер (smp_pool.c).
; AP спят в `sti; hlt`; BSP будит их фиксированным IPI с этим вектором.
; Обработчик только подтверждает EOI — вся логика в цикле AP после hlt.
global isr240
isr240:
    push qword 0
    push qword 240
    jmp  isr_common

global isr254
isr254:
    push qword 0
    push qword 254
    jmp  isr_common

global isr255
isr255:
    push qword 0
    push qword 255
    jmp  isr_common

; ----------------------------------------------------------------------------
; Общий диспатчер
; ----------------------------------------------------------------------------
; На входе на стеке (сверху вниз):
;   int_no   <- rsp
;   err_code
;   rip, cs, rflags, rsp, ss   ← запушено процессором
;
; Мы должны построить registers64_t (см. idt.h), вызвать C-обработчик,
; потом восстановить регистры и iretq.
;
; Layout registers64_t (низкий адрес → высокий):
;   r15, r14, r13, r12, r11, r10, r9, r8,
;   rdi, rsi, rbp, rdx, rcx, rbx, rax,
;   int_no, err_code,
;   rip, cs, rflags, rsp, ss
;
; В стеке память растёт вниз → пушим в обратном порядке (последнее push'нутое
; окажется на rsp, что соответствует первому полю структуры).
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Сохраняем DS/ES — формально в long mode не нужны, но если когда-то
    ; будем переключаться в compat-режим, пригодятся. Пока пропускаем.

    ; ------------------------------------------------------------------------
    ; [этап 44] Сохранение FPU/SSE state на время C-диспатчера.
    ; ------------------------------------------------------------------------
    ; Зачем: ядро AMPLIFY-OS теперь компилируется с включёнными SSE/SSE2
    ; (см. Makefile.x86_64 — флаги -mno-sse выпилены). GCC может на любую
    ; функцию вставить movss/mulps/etc. Любое прерывание поверх user/kernel
    ; кода, активно использующего FPU (например, GLES-рендер), может
    ; затереть xmm0..xmm15, ST(0..7) или MXCSR. FXSAVE/FXRSTOR вокруг
    ; isr_dispatch гарантирует, что прерванный контекст найдёт регистры в
    ; точно том же состоянии.
    ;
    ; Память: 512 байт scratch'а на kernel-stack'е (16-byte aligned).
    ; Перформанс: ~30-100 ns в зависимости от CPU, для 100-Hz PIT'а это
    ; ничтожно.
    ;
    ; Alignment: после 15×push qword (120 байт) + 56 байт сверху от CPU
    ; (iret-frame 40 + int_no/err_code 16) = 176 байт, 16-byte aligned.
    ; sub rsp, 512 — 16-aligned остаётся.
    ; ------------------------------------------------------------------------
    sub  rsp, 512
    fxsave [rsp]

    ; Вызываем C-функцию: первый аргумент = указатель на регистровую
    ; структуру.  Она лежит на 512 байт ВЫШЕ текущего RSP (потому что мы
    ; только что выделили FPU-scratch перед ней).
    lea  rdi, [rsp + 512]

    cld                             ; SysV: DF=0
    call isr_dispatch

    ; Восстанавливаем FPU/SSE state, сбрасываем scratch.
    fxrstor [rsp]
    add  rsp, 512

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16                    ; убираем int_no и err_code
    iretq
