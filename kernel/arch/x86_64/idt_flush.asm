; ============================================================================
; idt_flush.asm — lidt в 64-битном режиме.
; ============================================================================
[bits 64]
global idt64_load
section .text

; void idt64_load(uint64_t idt_ptr_addr);
;   RDI = адрес idt_ptr_t
idt64_load:
    lidt [rdi]
    ret
