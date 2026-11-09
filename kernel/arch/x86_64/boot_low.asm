; ============================================================================
; kernel/arch/x86_64/boot.asm — Multiboot2 + UEFI handoff + 32→64 трамплин.
; ----------------------------------------------------------------------------
; Структура:
;   1) Multiboot2 header (магия 0xE85250D6) с тэгами:
;        - information request (хотим mmap, framebuffer, ACPI RSDP)
;        - framebuffer 1024x768x32
;        - entry address (тип 3) — указывает GRUB на _start (32-bit)
;          ELF e_entry же установлен в _efi_start (64-bit) для UEFI пути
;        - end
;   2) 32-битная точка входа _start: получает EAX=MB2_MAGIC, EBX=info ptr.
;        - проверяет CPUID и наличие Long Mode bit'а
;        - строит PML4/PDPT/4×PD (identity-map первые 4 GB через 2-MB pages)
;        - PAE → EFER.LME → CR0.PG → lgdt → far jmp 0x08:long_mode_start
;   3) 64-битный long_mode_start: настраивает сегменты, передаёт magic+info
;        в kernel_main64 через rdi/rsi (SysV ABI).
;   4) [этап 38] 64-битная точка входа _efi_start (для UEFI handoff):
;        - принимает в RDI указатель на синтетический mb2_info_t от loader'а
;        - строит свои boot_pml4/pdpt/pd (32-bit _start делал в protected mode)
;        - грузит gdt64, переключает CR3, выставляет стек
;        - вызывает kernel_main64(MB2_BOOT_MAGIC, info)
; ============================================================================

; ----------------------------------------------------------------------------
; Multiboot2 header
; ----------------------------------------------------------------------------
MB2_HDR_MAGIC      equ 0xE85250D6
MB2_HDR_ARCH_I386  equ 0
MB2_HDR_LEN        equ mb2_hdr_end - mb2_hdr_start
MB2_HDR_CHECKSUM   equ -(MB2_HDR_MAGIC + MB2_HDR_ARCH_I386 + MB2_HDR_LEN)

; Multiboot2 boot magic (то, что GRUB кладёт в EAX при прыжке в _start).
MB2_BOOT_MAGIC     equ 0x36D76289

section .multiboot
align 8
mb2_hdr_start:
    dd MB2_HDR_MAGIC
    dd MB2_HDR_ARCH_I386
    dd MB2_HDR_LEN
    dd MB2_HDR_CHECKSUM

    ; --- Tag: information request (тип 1) ---
    ; Просим у GRUB конкретные тэги в boot info (mmap, framebuffer, ACPI).
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
    ; [HOTFIX 20.05.2026 FullHD] Просим 1920x1080x32. GRUB через VBE поднимет
    ; ближайший доступный VESA-режим. Если 1920x1080 не доступен (на старом
    ; VBE-БИОСе) — GRUB вернёт ближайший меньший (часто 1280x1024 / 1024x768),
    ; и fb_init() возьмёт его динамически из MB2 framebuffer-тэга. Все слои
    ; выше (doublebuf, compositor, WM, UI) считывают размер из fb_state(),
    ; так что fallback работает без правок логики.
    ; В QEMU `-vga std` обычно даёт точный 1920x1080; `-vga virtio` через
    ; virtio-gpu-pci просит модель напрямую (GET_DISPLAY_INFO).
align 8
mb2_fb_tag_start:
    dw 5                    ; type = framebuffer
    dw 0                    ; flags (1 = optional, 0 = required)
    dd mb2_fb_tag_end - mb2_fb_tag_start
    dd 1920                 ; width  (FullHD)
    dd 1080                 ; height (FullHD)
    dd 32                   ; depth (bpp)
mb2_fb_tag_end:

    ; --- Tag: entry address (тип 3) ---
    ; Перенаправляет GRUB на 32-битный _start, минуя ELF e_entry, который
    ; теперь указывает на _efi_start (64-bit, для UEFI handoff).
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
; .bss: стек ядра + page tables (PML4 + PDPT + 4×PD = 24 KB)
; ----------------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 32768
stack_top:

align 4096
boot_pml4:    resb 4096
boot_pdpt:    resb 4096
boot_pd:      resb 4 * 4096      ; 4 PD по 4 KB → identity-map 4 GB

; Сохранённые multiboot значения (8 байт каждое, чтобы прочитать в 64-bit).
align 8
mb_magic_save: resq 1
mb_info_save:  resq 1


; ----------------------------------------------------------------------------
; Ранний 64-битный GDT: null + kernel code + kernel data.
; ----------------------------------------------------------------------------
; В 64-bit базы/лимиты сегментов игнорируются, но дескрипторы должны быть.
;   gdt64[1]: 0x00AF9A000000FFFF
;     access=0x9A: P=1, DPL=0, S=1, type=Code(1010)
;     flags =0xAF: G=1, D=0 (зарезервирован), L=1 (long mode), AVL=0, limit_hi=F
;   gdt64[2]: 0x00CF92000000FFFF
;     access=0x92: P=1, DPL=0, S=1, type=Data(0010)
;     flags =0xCF: G=1, D=1, L=0, limit_hi=F
section .rodata
align 16
gdt64:
    dq 0x0000000000000000          ; 0x00 null
    dq 0x00AF9A000000FFFF          ; 0x08 kernel code (L=1)
    dq 0x00CF92000000FFFF          ; 0x10 kernel data
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64                       ; в 64-bit поле base 8 байт, но lgdt в 32-bit
                                   ; режиме читает только младшие 4. Адрес < 4 GB,
                                   ; так что верхние 4 байта = 0 и это работает.

; ----------------------------------------------------------------------------
; .text — 32-bit entry, затем 64-bit.
; ----------------------------------------------------------------------------
section .text

global _start
global _efi_start
global stack_top
global stack_bottom
global boot_pml4
global boot_pdpt
global boot_pd
extern kernel_main64


; ============================================================================
; 32-bit entry (вызывается GRUB)
; ============================================================================
[bits 32]

_start:
    cli

    ; Сохраняем multiboot магию и указатель.
    mov  [mb_magic_save], eax
    mov  dword [mb_magic_save + 4], 0
    mov  [mb_info_save],  ebx
    mov  dword [mb_info_save + 4], 0

    ; Стек.
    mov  esp, stack_top

    ; ---- [этап 44] Полная инициализация FPU + SSE ---------------------------
    ; CR0:
    ;   bit 1 MP = 1 (Monitor coProcessor)
    ;   bit 2 EM = 0 (нет эмуляции — есть железный FPU)
    ;   bit 3 TS = 0 (без lazy task switch — у нас eager FPU save/restore)
    ;   bit 5 NE = 1 (Native x87 exceptions через #MF, а не IRQ13)
    mov  ecx, cr0
    and  ecx, ~((1 << 2) | (1 << 3))    ; clear EM, TS
    or   ecx, (1 << 1) | (1 << 5)       ; set MP, NE
    mov  cr0, ecx
    fninit                              ; чистим x87 state в известное начальное

    ; ---- Проверка Long Mode через CPUID ------------------------------------
    ; Сначала — поддерживается ли расширенный CPUID (leaf 0x80000001)?
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_long_mode

    mov  eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; EDX bit 29 = LM
    jz   .no_long_mode

    ; ---- Построение PML4 ----------------------------------------------------
    ; PML4[0] -> PDPT
    mov  edi, boot_pml4
    mov  eax, boot_pdpt
    or   eax, 0x3                   ; P=1, RW=1
    mov  [edi], eax
    mov  dword [edi + 4], 0

    ; PDPT[0..3] -> PD0..PD3
    mov  edi, boot_pdpt
    mov  ecx, 4
    mov  eax, boot_pd
    or   eax, 0x3
.fill_pdpt:
    mov  [edi], eax
    mov  dword [edi + 4], 0
    add  edi, 8
    add  eax, 4096
    loop .fill_pdpt

    ; PDi[j] = (i*1G + j*2M) | 0x83  (P, RW, PS=1 → 2-MB страница)
    mov  edi, boot_pd
    xor  esi, esi                   ; ESI = текущий физ. адрес (нижние 32 бита)
    mov  ecx, 4 * 512               ; 2048 entries → 4 GB
.fill_pd:
    mov  eax, esi
    or   eax, 0x83
    mov  [edi], eax
    mov  dword [edi + 4], 0         ; верхние 32 бита физ. адреса = 0
    add  edi, 8
    add  esi, 0x200000              ; +2 MB
    loop .fill_pd

    ; ---- Включаем PAE + SSE (CR4) ------------------------------------------
    ; bit 5  PAE          — обязательно для Long Mode
    ; bit 9  OSFXSR       — ОС поддерживает FXSAVE/FXRSTOR (нужно для SSE)
    ; bit 10 OSXMMEXCPT   — ОС обрабатывает #XF (SIMD FP exception)
    mov  eax, cr4
    or   eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, eax

    ; ---- Загружаем дефолтный MXCSR -----------------------------------------
    ; 0x1F80 = маскируем все 6 SIMD FP-исключений (default x86 ABI). Иначе
    ; деление на 0 / invalid op в SSE даст #XF, который у нас не обработан.
    push dword 0x1F80
    ldmxcsr [esp]
    add  esp, 4

    ; ---- Грузим CR3 ---------------------------------------------------------
    mov  eax, boot_pml4
    mov  cr3, eax

    ; ---- Включаем LME (EFER MSR) и NXE если есть ---------------------------
    mov  ecx, 0xC0000080            ; IA32_EFER
    rdmsr
    or   eax, 1 << 8                ; LME
    or   eax, 1 << 11               ; NXE (если CPU не поддерживает — wrmsr игнорит)
    wrmsr

    ; ---- Включаем paging (CR0.PG=1) и WP -----------------------------------
    mov  eax, cr0
    or   eax, 1 << 31               ; PG
    or   eax, 1 << 16               ; WP (защита write-protect для kernel writes)
    mov  cr0, eax

    ; CPU теперь в Long Mode (compatibility submode 32-bit).
    ; Грузим 64-битный GDT.
    lgdt [gdt64_ptr]

    ; Far jump в 64-bit code segment.
    jmp  0x08:long_mode_start


.no_long_mode:
    ; CPU не поддерживает Long Mode — пишем сообщение в VGA-text и спим.
    ; Это чисто страховка для древних i486/Pentium.
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

msg_no_long: db "AMPLIFY-OS: 64-bit CPU required (Long Mode not supported).", 0


; ============================================================================
; 64-bit entry (после прыжка из 32-bit _start через GRUB-путь)
; ============================================================================
[bits 64]

long_mode_start:
    ; Сегментные регистры.
    mov  ax, 0x10                   ; kernel data selector
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Очистим старшие половины регистров и сбросим RBP (для красивого backtrace).
    xor  rbp, rbp

    ; Передаём аргументы согласно SysV x86_64 ABI:
    ;   RDI = magic, RSI = mb2_info_t*
    mov  rdi, [mb_magic_save]
    mov  rsi, [mb_info_save]

    ; Перезаписываем стек на тот же top, но 64-битным канонически выровненным.
    mov  rsp, stack_top
    and  rsp, ~0xF                  ; align 16 (требование SysV перед call)

    call kernel_main64

    ; Если main вернётся — спим.
.halt:
    cli
    hlt
    jmp  .halt


; ============================================================================
; [этап 38] 64-bit entry FROM UEFI loader
; ----------------------------------------------------------------------------
; Процессор уже в Long Mode (UEFI работает в 64-битке), ExitBootServices()
; вызван loader'ом, прерывания выключены. Loader передал в RDI указатель на
; синтетический mb2_info_t.
;
; Что нужно сделать перед kernel_main64:
;   - заполнить наши boot_pml4/pdpt/pd 4-GB identity-map'ом
;     (в 32-bit пути это делает _start; здесь — 64-битным кодом)
;   - загрузить наш ранний gdt64 (EFI GDT после EBS ещё валидна, но мы хотим
;     управлять сами — это базовая гигиена)
;   - переключить CR3 на свои таблицы
;   - выставить стек, RDI = magic, RSI = info, fall through в kernel_main64
;
; ВАЖНО: ABI до прыжка — SysV (мы вызвали entry() из gcc-кода), значит:
;   RDI = info_ptr  (первый аргумент)
; Сохраняем его в R15 на время инициализации.
; ============================================================================
[bits 64]

_efi_start:
    cli
    cld                             ; DF=0 — обязательно перед REP STOSQ.
                                    ; SysV-ABI гарантирует это на границах
                                    ; вызовов, но защитимся от UEFI-косяков.

    ; Сохраним указатель на info_t — он перейдёт в RSI перед call.
    mov  r15, rdi

    ; ---- Очищаем PML4 -------------------------------------------------------
    lea  rdi, [rel boot_pml4]
    xor  rax, rax
    mov  rcx, 512
    rep  stosq

    ; ---- Очищаем PDPT ------------------------------------------------------
    lea  rdi, [rel boot_pdpt]
    xor  rax, rax
    mov  rcx, 512
    rep  stosq

    ; ---- PML4[0] -> PDPT ---------------------------------------------------
    lea  rax, [rel boot_pdpt]
    or   rax, 0x3                   ; P + RW
    lea  rdi, [rel boot_pml4]
    mov  [rdi], rax

    ; ---- PDPT[0..3] -> PD0..PD3 --------------------------------------------
    lea  rdi, [rel boot_pdpt]
    lea  rax, [rel boot_pd]
    or   rax, 0x3
    mov  rcx, 4
.fill_pdpt_efi:
    mov  [rdi], rax
    add  rdi, 8
    add  rax, 4096
    loop .fill_pdpt_efi

    ; ---- PD entries: 2048 × 2 MB = 4 GB identity ---------------------------
    lea  rdi, [rel boot_pd]
    xor  rsi, rsi                   ; physical address counter
    mov  rcx, 2048
.fill_pd_efi:
    mov  rax, rsi
    or   rax, 0x83                  ; P + RW + PS=1 (2 MB page)
    mov  [rdi], rax
    add  rdi, 8
    add  rsi, 0x200000              ; +2 MB
    loop .fill_pd_efi

    ; ---- Переключаем CR3 на наши таблицы -----------------------------------
    lea  rax, [rel boot_pml4]
    mov  cr3, rax

    ; ---- Включаем WP (write-protect для kernel writes) ---------------------
    mov  rax, cr0
    or   rax, 1 << 16
    mov  cr0, rax

    ; ---- Загружаем наш ранний GDT64 ----------------------------------------
    lgdt [rel gdt64_ptr]

    ; ---- Far return для перезагрузки CS = 0x08 -----------------------------
    ; В Long Mode нельзя сделать far jmp на immediate без 16-byte префикса —
    ; стандартный приём: положить SS/RIP на стек и сделать REX.W retf.
    push 0x08
    lea  rax, [rel .reload_cs]
    push rax
    o64 retf

.reload_cs:
    ; Сегментные регистры.
    mov  ax, 0x10                   ; kernel data
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; ---- [этап 44] Полная инициализация FPU + SSE --------------------------
    ; CR0: clear EM(2), TS(3); set MP(1), NE(5).
    mov  rax, cr0
    and  rax, ~((1 << 2) | (1 << 3))
    or   rax, (1 << 1) | (1 << 5)
    mov  cr0, rax
    fninit

    ; CR4: set OSFXSR(9) + OSXMMEXCPT(10) — поддержка SSE.
    ; PAE (bit 5) уже включён UEFI, но на всякий случай оставим OR.
    mov  rax, cr4
    or   rax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, rax

    ; MXCSR = 0x1F80 (маска всех 6 SIMD FP-исключений).
    push qword 0x1F80
    ldmxcsr [rsp]
    add  rsp, 8

    ; ---- Стек ---------------------------------------------------------------
    lea  rsp, [rel stack_top]
    and  rsp, ~0xF                  ; align 16

    ; ---- Сброс RBP для красивого backtrace ---------------------------------
    xor  rbp, rbp

    ; ---- Имитация Multiboot2 handoff для kernel_main64 ---------------------
    ;   RDI = magic = MB2_BOOT_MAGIC
    ;   RSI = mb2_info_t* (передан loader'ом, мы сохранили в R15)
    mov  edi, MB2_BOOT_MAGIC        ; edi обнуляет верхние биты rdi
    mov  rsi, r15

    call kernel_main64

.halt_efi:
    cli
    hlt
    jmp  .halt_efi
