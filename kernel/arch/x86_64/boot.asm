; ============================================================================
; kernel/arch/x86_64/boot.asm — вход в ядро: Multiboot2 (GRUB) и UEFI.
; ----------------------------------------------------------------------------
; [38.1.1] Ядро слинковано в верхней половине адресов (KERNEL_VMA, см.
; linker_x86_64.ld), а загружено по физическому адресу «LMA + сдвиг»:
;   GRUB        — сдвиг 0 (сегменты ровно по LMA, с 1 МБ);
;   BOOTX64.EFI — сдвиг любой, кратный 2 МБ (куда прошивка дала место).
;
; Поэтому вход двухступенчатый:
;   1) ЗАГЛУШКА (.boot.text / .boot.data, слинкована «внизу», VMA = LMA).
;      Исполняется по физическому адресу, до наших таблиц страниц. Строит
;      таблицы и включает их:
;        PML4[0]            — тождественное отображение первых 4 ГБ (пока
;                             нужно всему ядру: «физический адрес = указатель»;
;                             снимается в 38.1.4);
;        PML4[511]/PDPT[510] — образ ядра: KERNEL_VMA + x -> сдвиг + x,
;                             1 ГБ страницами по 2 МБ.
;      Затем прыгает на верхний адрес.
;   2) ВЕРХНЯЯ часть (.text): стек, аргументы, kernel_main64.
;
; Заглушка для UEFI позиционно-независима: всё своё она адресует относительно
; RIP, а сдвиг узнаёт как разницу между адресом исполнения и адресом линковки.
; Таблицы лежат в .bss (верхний VMA), чтобы код на C работал с ними как с
; обычными массивами; заглушке их «нижние» адреса даёт линкер (*_lma).
;
; Настройки CR0/CR4/EFER/MXCSR повторяются в трёх точках входа (CLAUDE.md
; §5.2): _start, _efi_start здесь и tramp_pm32 в smp_trampoline.asm.
; Прежний вариант файла (ядро по адресу 1 МБ) — boot_low.asm.
; ============================================================================

MB2_HDR_MAGIC      equ 0xE85250D6
MB2_HDR_ARCH_I386  equ 0
MB2_HDR_LEN        equ mb2_hdr_end - mb2_hdr_start
MB2_HDR_CHECKSUM   equ -(MB2_HDR_MAGIC + MB2_HDR_ARCH_I386 + MB2_HDR_LEN)

; Multiboot2 boot magic (то, что GRUB кладёт в EAX при прыжке в _start).
MB2_BOOT_MAGIC     equ 0x36D76289

; Сколько страниц занимают загрузочные таблицы подряд (см. .bss ниже):
; PML4 + PDPT + 4 PD + PDPT_HI + PD_HI.
BOOT_TABLE_PAGES   equ 8

section .multiboot
align 8
mb2_hdr_start:
    dd MB2_HDR_MAGIC
    dd MB2_HDR_ARCH_I386
    dd MB2_HDR_LEN
    dd MB2_HDR_CHECKSUM

    ; --- Tag: information request (тип 1) ---
align 8
mb2_inforeq_start:
    dw 1                    ; type = information request
    dw 0                    ; flags
    dd mb2_inforeq_end - mb2_inforeq_start
    dd 6                    ; mmap
    dd 8                    ; framebuffer
    dd 14                   ; ACPI old RSDP
    dd 15                   ; ACPI new RSDP
    dd 4                    ; basic memory info
    dd 1                    ; cmdline
mb2_inforeq_end:

    ; --- Tag: framebuffer (тип 5) ---
    ; Просим 1920x1080x32; GRUB через VBE поднимет ближайший доступный режим,
    ; fb_init() возьмёт фактический из тэга framebuffer.
align 8
mb2_fb_tag_start:
    dw 5                    ; type = framebuffer
    dw 0                    ; flags (1 = optional, 0 = required)
    dd mb2_fb_tag_end - mb2_fb_tag_start
    dd 1920
    dd 1080
    dd 32
mb2_fb_tag_end:

    ; --- Tag: entry address (тип 3) ---
    ; GRUB идёт сюда, а не в e_entry (тот — для UEFI). _start лежит в
    ; заглушке, его адрес «нижний» = физический: GRUB грузит по LMA.
align 8
mb2_entry_tag_start:
    dw 3                    ; type = entry address
    dw 0                    ; flags
    dd mb2_entry_tag_end - mb2_entry_tag_start
    dd _start
mb2_entry_tag_end:

    ; --- Tag: end (тип 0) ---
align 8
    dw 0
    dw 0
    dd 8
mb2_hdr_end:


; ----------------------------------------------------------------------------
; .bss (верхний VMA): стек ядра и загрузочные таблицы страниц.
; Порядок таблиц важен: заглушка обнуляет их одним куском (BOOT_TABLE_PAGES).
; ----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 32768
stack_top:

align 4096
boot_pml4:    resb 4096
boot_pdpt:    resb 4096          ; PML4[0]: тождественное отображение
boot_pd:      resb 4 * 4096      ; 4 PD по 1 ГБ -> первые 4 ГБ
boot_pdpt_hi: resb 4096          ; PML4[511]: верхние 512 ГБ
boot_pd_hi:   resb 4096          ; PDPT_HI[510]: 1 ГБ с KERNEL_VMA — образ ядра


; ----------------------------------------------------------------------------
; Данные заглушки («нижние» адреса).
; ----------------------------------------------------------------------------
section .boot.data progbits alloc noexec write align=16

; Ранний GDT: null + код ядра (L=1) + данные. Живёт до gdt64_init() в C.
align 16
boot_gdt:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00AF9A000000FFFF          ; 0x08 kernel code (L=1)
    dq 0x00CF92000000FFFF          ; 0x10 kernel data
boot_gdt_end:

boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dq boot_gdt                    ; адрес линковки; путь UEFI перед lgdt
                                   ; вписывает сюда адрес с учётом сдвига

; MXCSR по умолчанию: все шесть исключений SIMD замаскированы. Грузим из
; памяти, а не через стек: у 32-битной заглушки стека нет вовсе.
align 4
boot_mxcsr:    dd 0x1F80

; Что передал GRUB (EAX — магия, EBX — инфоблок).
align 8
mb_magic_save: dq 0
mb_info_save:  dq 0

msg_no_long:   db "AMPLIFY-OS: 64-bit CPU required (Long Mode not supported).", 0


; ============================================================================
; Заглушка, вход GRUB (32 бита, сдвиг 0: адрес исполнения = адрес линковки).
; ============================================================================
section .boot.text progbits alloc exec nowrite align=16

global _start
global _efi_start
global stack_top
global stack_bottom
global boot_pml4
global boot_pdpt
global boot_pd
global boot_pdpt_hi
global boot_pd_hi
extern kernel_main64
extern boot_kphys_delta
extern boot_pml4_lma
extern boot_pdpt_lma
extern boot_pd_lma
extern boot_pdpt_hi_lma
extern boot_pd_hi_lma

[bits 32]
_start:
    cli
    cld

    ; Магия и инфоблок — в память: CPUID ниже портит EAX/EBX, а стека нет.
    mov  [mb_magic_save], eax
    mov  [mb_info_save],  ebx

    ; ---- FPU (CR0): MP=1, NE=1, EM=0, TS=0 ---------------------------------
    mov  ecx, cr0
    and  ecx, ~((1 << 2) | (1 << 3))
    or   ecx, (1 << 1) | (1 << 5)
    mov  cr0, ecx
    fninit

    ; ---- Есть ли Long Mode --------------------------------------------------
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_long_mode
    mov  eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; EDX[29] = LM
    jz   .no_long_mode

    ; ---- Таблицы страниц ----------------------------------------------------
    ; Обнулить все восемь страниц: GRUB чистит .bss, но полагаться на это в
    ; таблицах страниц незачем.
    mov  edi, boot_pml4_lma
    xor  eax, eax
    mov  ecx, BOOT_TABLE_PAGES * 1024
    rep  stosd

    ; PML4[0] -> PDPT (тождественное), PML4[511] -> PDPT_HI (образ ядра).
    mov  edi, boot_pml4_lma
    mov  eax, boot_pdpt_lma
    or   eax, 0x3                   ; P + RW
    mov  [edi], eax
    mov  eax, boot_pdpt_hi_lma
    or   eax, 0x3
    mov  [edi + 511 * 8], eax

    ; PDPT[0..3] -> PD0..PD3
    mov  edi, boot_pdpt_lma
    mov  eax, boot_pd_lma
    or   eax, 0x3
    mov  ecx, 4
.fill_pdpt:
    mov  [edi], eax
    add  edi, 8
    add  eax, 4096
    loop .fill_pdpt

    ; PD: 2048 записей по 2 МБ = 4 ГБ, адрес = он сам.
    mov  edi, boot_pd_lma
    xor  esi, esi
    mov  ecx, 4 * 512
.fill_pd:
    mov  eax, esi
    or   eax, 0x83                  ; P + RW + PS
    mov  [edi], eax
    add  edi, 8
    add  esi, 0x200000
    loop .fill_pd

    ; PDPT_HI[510] -> PD_HI; PD_HI[i] = i * 2 МБ (сдвиг 0): KERNEL_VMA + x -> x.
    mov  edi, boot_pdpt_hi_lma
    mov  eax, boot_pd_hi_lma
    or   eax, 0x3
    mov  [edi + 510 * 8], eax

    mov  edi, boot_pd_hi_lma
    xor  esi, esi
    mov  ecx, 512
.fill_pd_hi:
    mov  eax, esi
    or   eax, 0x83
    mov  [edi], eax
    add  edi, 8
    add  esi, 0x200000
    loop .fill_pd_hi

    ; ---- CR4: PAE + OSFXSR + OSXMMEXCPT ------------------------------------
    mov  eax, cr4
    or   eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, eax

    ldmxcsr [boot_mxcsr]

    ; ---- CR3, EFER.LME (+NXE), CR0.PG + WP ---------------------------------
    mov  eax, boot_pml4_lma
    mov  cr3, eax

    mov  ecx, 0xC0000080            ; IA32_EFER
    rdmsr
    or   eax, (1 << 8) | (1 << 11)  ; LME, NXE
    wrmsr

    mov  eax, cr0
    or   eax, (1 << 31) | (1 << 16) ; PG, WP
    mov  cr0, eax

    lgdt [boot_gdt_ptr]
    jmp  0x08:.lm_low

.no_long_mode:
    ; Процессор без Long Mode: сообщение в текстовый экран и стоп.
    mov  edi, 0xB8000
    mov  esi, msg_no_long
    mov  ah, 0x4F                   ; белый на красном
.print_loop:
    lodsb
    test al, al
    jz   .print_done
    stosw
    jmp  .print_loop
.print_done:
    cli
    hlt
    jmp  .print_done

[bits 64]
.lm_low:
    ; Мы в длинном режиме, но всё ещё по «нижнему» адресу. Прыжок наверх —
    ; только через регистр: относительному переходу туда не дотянуться.
    mov  rax, long_mode_start
    jmp  rax


; ============================================================================
; Заглушка, вход UEFI (64 бита, сдвиг любой).
; ----------------------------------------------------------------------------
; Загрузчик вызвал нас как функцию SysV: RDI = синтетический инфоблок MB2.
; ExitBootServices уже сделан, прерывания выключены, работаем по таблицам
; прошивки (тождественным) и на её стеке.
; ============================================================================
[bits 64]
_efi_start:
    cli
    cld
    mov  r15, rdi                   ; инфоблок — до вызова kernel_main64

    ; Сдвиг = где исполняемся − где слинкованы.
    lea  rbx, [rel _efi_start]
    mov  rax, _efi_start
    sub  rbx, rax                   ; RBX = сдвиг (живёт до верхней части)

    ; Физические адреса таблиц: «нижний» адрес + сдвиг.
    mov  r8d,  boot_pml4_lma
    add  r8,   rbx
    mov  r9d,  boot_pdpt_lma
    add  r9,   rbx
    mov  r10d, boot_pd_lma
    add  r10,  rbx
    mov  r11d, boot_pdpt_hi_lma
    add  r11,  rbx
    mov  r12d, boot_pd_hi_lma
    add  r12,  rbx

    ; Обнулить все таблицы (они идут подряд, начиная с PML4).
    mov  rdi, r8
    xor  eax, eax
    mov  ecx, BOOT_TABLE_PAGES * 512
    rep  stosq

    ; PML4[0] -> PDPT, PML4[511] -> PDPT_HI.
    lea  rax, [r9 + 3]
    mov  [r8], rax
    lea  rax, [r11 + 3]
    mov  [r8 + 511 * 8], rax

    ; PDPT[0..3] -> PD0..PD3
    mov  rdi, r9
    lea  rax, [r10 + 3]
    mov  ecx, 4
.fill_pdpt_efi:
    mov  [rdi], rax
    add  rdi, 8
    add  rax, 4096
    loop .fill_pdpt_efi

    ; PD: тождественные 4 ГБ.
    mov  rdi, r10
    xor  esi, esi
    mov  ecx, 4 * 512
.fill_pd_efi:
    mov  rax, rsi
    or   rax, 0x83
    mov  [rdi], rax
    add  rdi, 8
    add  rsi, 0x200000
    loop .fill_pd_efi

    ; PDPT_HI[510] -> PD_HI; PD_HI[i] = сдвиг + i * 2 МБ.
    lea  rax, [r12 + 3]
    mov  [r11 + 510 * 8], rax

    mov  rdi, r12
    mov  rsi, rbx
    mov  ecx, 512
.fill_pd_hi_efi:
    mov  rax, rsi
    or   rax, 0x83
    mov  [rdi], rax
    add  rdi, 8
    add  rsi, 0x200000
    loop .fill_pd_hi_efi

    ; ---- Наши таблицы -------------------------------------------------------
    mov  cr3, r8

    mov  rax, cr0
    or   rax, 1 << 16               ; WP
    mov  cr0, rax

    ; ---- Ранний GDT: в указателе — адрес линковки, нужен настоящий ----------
    lea  rax, [rel boot_gdt]
    mov  [rel boot_gdt_ptr + 2], rax
    lgdt [rel boot_gdt_ptr]

    ; Перезагрузить CS = 0x08: дальний возврат (в длинном режиме far jmp на
    ; непосредственный адрес недоступен).
    push 0x08
    lea  rax, [rel .reload_cs]
    push rax
    o64 retf

.reload_cs:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; ---- FPU + SSE ----------------------------------------------------------
    mov  rax, cr0
    and  rax, ~((1 << 2) | (1 << 3))
    or   rax, (1 << 1) | (1 << 5)
    mov  cr0, rax
    fninit

    mov  rax, cr4
    or   rax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, rax

    ldmxcsr [rel boot_mxcsr]

    ; ---- Наверх -------------------------------------------------------------
    mov  rax, efi_high
    jmp  rax


; ============================================================================
; Верхняя часть: исполняется уже по виртуальным адресам ядра.
; ============================================================================
section .text
[bits 64]

; --- после входа GRUB --------------------------------------------------------
long_mode_start:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    mov  rsp, stack_top
    and  rsp, ~0xF                  ; SysV: выравнивание 16 перед call
    xor  rbp, rbp

    ; Сдвиг у GRUB нулевой; пишем явно, чтобы значение не зависело от того,
    ; чем инициализирована переменная.
    mov  qword [rel boot_kphys_delta], 0

    ; Аргументы: магия и инфоблок. Сохранённые значения лежат в заглушке по
    ; «нижним» адресам — они доступны через тождественное отображение.
    mov  rax, mb_magic_save
    mov  rdi, [rax]
    mov  rax, mb_info_save
    mov  rsi, [rax]

    call kernel_main64
.halt:
    cli
    hlt
    jmp  .halt

; --- после входа UEFI: RBX = сдвиг, R15 = инфоблок ---------------------------
efi_high:
    mov  rsp, stack_top
    and  rsp, ~0xF
    xor  rbp, rbp

    mov  [rel boot_kphys_delta], rbx

    mov  edi, MB2_BOOT_MAGIC        ; запись в EDI обнуляет верх RDI
    mov  rsi, r15
    call kernel_main64
.halt_efi:
    cli
    hlt
    jmp  .halt_efi
