; ============================================================================
; gdt_flush.asm — обёртки lgdt + ltr для 64-битного GDT.
; ============================================================================
[bits 64]

global gdt64_flush
global gdt64_load_tss

section .text

; void gdt64_flush(uint64_t gdt_ptr_addr);
;   RDI = адрес gdt_ptr_t
gdt64_flush:
    lgdt [rdi]

    ; Перезагружаем data segments.
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Перезагрузка CS требует far return / far jump. В 64-bit нет
    ; прямого "jmp far imm:imm", поэтому используем far return через стек.
    pop  rdi                ; снимаем return address с вершины
    push qword 0x08         ; новый CS
    push rdi                ; вернуть туда же
    retfq                   ; far return → грузит CS и RIP

; void gdt64_load_tss(uint16_t selector);
;   DI = TSS selector
gdt64_load_tss:
    mov  ax, di
    ltr  ax
    ret
