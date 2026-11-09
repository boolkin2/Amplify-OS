[BITS 16]

global trampoline_start
global trampoline_end

extern ap_main
extern gdt_ptr

trampoline_start:
    cli
    cld

    ; Load GDT (We can use the same GDT as BSP, but we need the physical address)
    ; Assuming the BSP's gdt_ptr is loaded and accessible at physical memory.
    ; Wait, we are at 0x8000. Let's load the GDT. 
    ; GDT ptr should be passed somehow. Let's just create a temporary GDT here to jump to PM.
    lgdt [cs:gdt_desc - trampoline_start + 0x8000]

    ; Enable PM
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Jump to 32-bit
    jmp 0x08:(pm32 - trampoline_start + 0x8000)

[BITS 32]
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Give AP a unique stack. 
    ; For simplicity, we just calculate stack based on local APIC ID.
    ; But we don't know it easily until LAPIC is accessible.
    ; Since LAPIC base is 0xFEE00000, which is identity mapped, we can read it.
    mov eax, [0xFEE00020]
    shr eax, 24
    
    ; Stack base 0x90000 + (LAPIC ID * 4096)
    inc eax ; IDs can be 0, 1, 2...
    shl eax, 12
    add eax, 0x90000
    mov esp, eax

    ; Call C function
    call ap_main

    cli
    hlt

; Temporary GDT for Trampoline
align 4
gdt_start:
    dq 0x0 ; Null descriptor
    dq 0x00CF9A000000FFFF ; 32-bit code, 4GB, ring 0
    dq 0x00CF92000000FFFF ; 32-bit data, 4GB, ring 0
gdt_desc:
    dw gdt_desc - gdt_start - 1
    dd gdt_start - trampoline_start + 0x8000

trampoline_end:
