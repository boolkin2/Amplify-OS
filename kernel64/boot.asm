; ============================================================================
; kernel64/boot.asm — Этап 33C: 32->64 bit boot trampoline для 64-битного ядра
; ----------------------------------------------------------------------------
; GRUB загружает нас в 32-битном protected mode по Multiboot1. Мы:
;   1) Сохраняем eax (multiboot magic) и ebx (multiboot info) в g_mb_*.
;   2) Настраиваем стек.
;   3) Строим PML4 + PDPT + 4 PD (identity-map первые 4 GB через 2-MB страницы).
;   4) Включаем PAE, грузим CR3 = PML4, включаем LME (EFER), включаем PG.
;   5) Загружаем 64-битный GDT и far-jmp на 64-bit code segment.
;   6) В 64-bit коде передаём управление в C-функцию kmain64(magic, info).
;
; Multiboot1 header — стандартный, флаги: align modules + memory info.
; (графический режим тут НЕ запрашиваем — упрощаем boot, выводим в VGA text
;  и в Multiboot framebuffer info на этапе 33C не полагаемся.)
; ============================================================================

; ----------------------------------------------------------------------------
; Multiboot1 header
; ----------------------------------------------------------------------------
MB_MAGIC      equ 0x1BADB002
MB_FLAGS      equ 0x00000003       ; align modules + memory info
MB_CHECKSUM   equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

; ----------------------------------------------------------------------------
; Стек — 16 KB
; ----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; ----------------------------------------------------------------------------
; Page tables (4-level): PML4 (4K) + PDPT (4K) + 4×PD (16K)
; Все таблицы выровнены на 4 KB и обнулены в .bss.
; ----------------------------------------------------------------------------
align 4096
pml4_table:    resb 4096
pdpt_table:    resb 4096
pd_tables:     resb 4 * 4096       ; 4 PD по 4 KB

; Сохранённые multiboot регистры (для kmain64).
align 8
g_mb_magic:    resq 1
g_mb_info:     resq 1

; ----------------------------------------------------------------------------
; 64-битный GDT
; ----------------------------------------------------------------------------
section .rodata
align 16
gdt64:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00AF9A000000FFFF          ; 0x08 64-bit code  (L=1)
    dq 0x00CF92000000FFFF          ; 0x10 32-bit data  (RW)
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; ----------------------------------------------------------------------------
; 32-bit entry point
; ----------------------------------------------------------------------------
section .text
[bits 32]

global _start
extern kmain64

_start:
    cli
    ; Сохраняем multiboot значения (eax = magic, ebx = info ptr).
    mov  [g_mb_magic], eax
    mov  dword [g_mb_magic + 4], 0
    mov  [g_mb_info],  ebx
    mov  dword [g_mb_info + 4], 0

    ; Стек.
    mov  esp, stack_top

    ; ---- Построение PML4 ----------------------------------------------------
    ; PML4[0] -> PDPT
    mov  edi, pml4_table
    mov  eax, pdpt_table
    or   eax, 0x3                  ; P=1, RW=1
    mov  [edi], eax
    mov  dword [edi + 4], 0

    ; PDPT[0..3] -> PD0..PD3
    mov  edi, pdpt_table
    mov  ecx, 4
    mov  eax, pd_tables
    or   eax, 0x3
.fill_pdpt:
    mov  [edi], eax
    mov  dword [edi + 4], 0
    add  edi, 8
    add  eax, 4096                 ; следующий PD
    loop .fill_pdpt

    ; PD i [j] = (i*1G + j*2M) | 0x83  (P=1, RW=1, PS=1)
    mov  edi, pd_tables
    xor  esi, esi                  ; ESI = текущий физ. адрес страницы
    mov  ecx, 4 * 512              ; 4 PD по 512 entries = 2048 entries (4 GB)
.fill_pd:
    mov  eax, esi
    or   eax, 0x83                 ; P, RW, PS (2-MB)
    mov  [edi], eax
    mov  edx, 0                    ; верхние 32 бита физ.адреса = 0
    mov  [edi + 4], edx
    add  edi, 8
    add  esi, 0x200000             ; +2 MB
    loop .fill_pd

    ; ---- Включаем PAE -------------------------------------------------------
    mov  eax, cr4
    or   eax, 1 << 5
    mov  cr4, eax

    ; ---- Грузим CR3 ---------------------------------------------------------
    mov  eax, pml4_table
    mov  cr3, eax

    ; ---- Включаем LME (EFER MSR 0xC0000080 bit 8) ---------------------------
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 1 << 8
    wrmsr

    ; ---- Включаем paging (CR0.PG=1) -----------------------------------------
    mov  eax, cr0
    or   eax, 1 << 31
    mov  cr0, eax
    ; CPU теперь в Long Mode (compatibility 32-bit submode).

    ; ---- Грузим 64-битный GDT и прыгаем в 64-bit code -----------------------
    lgdt [gdt64_ptr]
    jmp  0x08:long_mode_start


[bits 64]

long_mode_start:
    ; Сегментные регистры в 64-bit в основном игнорируются, но загрузим
    ; null или 0x10 на всякий случай.
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Стек уже настроен (RSP < 4 GB, identity-mapped).
    ; Передаём аргументы согласно SysV x86_64 ABI:
    ;   rdi = magic, rsi = mb_info_ptr
    mov  rdi, [g_mb_magic]
    mov  rsi, [g_mb_info]

    ; Очистим регистры (хорошая практика).
    xor  rbp, rbp

    ; Вызываем C-точку входа.
    call kmain64

    ; Если kmain64 вернётся — спим.
.halt_loop:
    cli
    hlt
    jmp  .halt_loop
