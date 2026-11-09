; ============================================================================
; smp_trampoline.asm — 16→32→64-bit AP boot trampoline для AMPLIFY-OS.
; ----------------------------------------------------------------------------
; После SIPI (Startup IPI) AP стартует в 16-bit real mode по адресу
; vector*4096. Мы передаём vector = 0x08, поэтому код должен лежать по
; физическому адресу 0x8000 и быть position-independent: BSP скопирует туда
; целый блок trampoline_start..trampoline_end из ядра.
;
; Что делает AP по шагам:
;   1) Загружает временный 16/32-bit GDT (внутри trampoline);
;   2) CR0.PE = 1 → переход в 32-bit PM;
;   3) CR3 = boot_pml4_phys (ту же таблицу, что и BSP);
;      CR4.PAE=1, EFER.LME=1, CR0.PG=1 → попадает в Long Mode;
;   4) Загружает 64-bit GDT (внутри trampoline) и far-jmp в 64-bit код;
;   5) Через `lock xadd` берёт свой "слот" (1, 2, ...), вычисляет адрес стека
;      из массива ap_stacks_top, переключает RSP, вызывает ap_main64.
;
; Все данные/символы внутри trampoline доступны через смещение от
; (TRAMPOLINE_PHYS = 0x8000), которое мы вшиваем константой.
; ============================================================================

%define TRAMPOLINE_PHYS  0x8000

global trampoline_start
global trampoline_end
global tramp_pml4_phys
global tramp_ap_stacks_top
global tramp_ap_slot

extern ap_main64

[bits 16]
section .text

; ----------------------------------------------------------------------------
; trampoline_start — то, что копируем в 0x8000
; ----------------------------------------------------------------------------
trampoline_start:
    cli
    cld

    ; После SIPI(vec=0x08) у нас CS=0x800, поэтому в real-mode [cs:X] даёт
    ; физический адрес 0x800*16 + X = 0x8000 + X.  Чтобы попасть в данные
    ; внутри trampoline blob (он лежит на 0x8000..0x8000+size), X должен
    ; быть просто смещением ВНУТРИ blob'а: (label - trampoline_start).
    ; БЕЗ + TRAMPOLINE_PHYS — иначе адрес удвоится до 0x10000+X.
    lgdt [cs:tramp_gdt32_desc - trampoline_start]

    ; Включаем Protected Mode.
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax

    ; far jump в 32-bit PM. После CR0.PE и far-jmp CS становится 0x08
    ; (новый GDT), сегментная база = 0, поэтому offset должен быть полным
    ; ЛИНЕЙНЫМ адресом (= 0x8000 + смещение_в_blob), отсюда +TRAMPOLINE_PHYS.
    jmp  0x08:(tramp_pm32 - trampoline_start + TRAMPOLINE_PHYS)


[bits 32]
tramp_pm32:
    ; Сегментные регистры для 32-bit data.
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; ---- [этап 44] Инициализация FPU + SSE на AP ----------------------------
    ; Это КРИТИЧНО: после этапа 44 ядро собирается БЕЗ -mno-sse, и любой
    ; C-код (включая ap_main64) может содержать SSE-инструкции. Если AP
    ; войдёт в Long Mode без CR0.MP|NE и CR4.OSFXSR|OSXMMEXCPT, первая же
    ; SSE-инструкция в ap_main64 даст #UD → #DF → #TF, что свалит и AP,
    ; и при `-no-reboot` весь QEMU вместе с BSP.
    ;
    ; CR0:
    ;   bit 1 MP = 1 (Monitor coProcessor)
    ;   bit 2 EM = 0 (нет эмуляции — есть железный FPU)
    ;   bit 3 TS = 0 (без lazy task switch — у нас eager FPU save/restore)
    ;   bit 5 NE = 1 (Native x87 exceptions через #MF, а не IRQ13)
    mov  eax, cr0
    and  eax, ~((1 << 2) | (1 << 3))    ; clear EM, TS
    or   eax, (1 << 1) | (1 << 5)       ; set MP, NE
    mov  cr0, eax
    fninit                              ; чистим x87 state на AP

    ; ---- Подготовка к Long Mode --------------------------------------------
    ; CR3 = адрес PML4, который BSP положил в tramp_pml4_phys.
    mov  eax, [tramp_pml4_phys - trampoline_start + TRAMPOLINE_PHYS]
    mov  cr3, eax

    ; CR4:
    ;   bit 5  PAE        — обязательно для Long Mode
    ;   bit 9  OSFXSR     — OC поддерживает FXSAVE/FXRSTOR (нужно для SSE)
    ;   bit 10 OSXMMEXCPT — OC обрабатывает #XF (SIMD FP exception)
    mov  eax, cr4
    or   eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov  cr4, eax

    ; Дефолтный MXCSR (маскируем все 6 SIMD FP-исключений).
    ; HOTFIX 19.05.2026 (KVM): прежний код делал `push dword 0x1F80; ldmxcsr
    ; [esp]`. Но у AP после SIPI ESP = 0 (мы его явно не устанавливаем),
    ; первый push увёл бы ESP в 0xFFFFFFFC. На VM с 1 ГБ RAM это адрес ВЫШЕ
    ; физической памяти. Под TCG запись по такому адресу молча проходит, под
    ; KVM — #PF → #DF → triple fault. Грузим MXCSR прямо из памяти блоба.
    ldmxcsr [tramp_mxcsr_init - trampoline_start + TRAMPOLINE_PHYS]

    ; EFER.LME = 1 (+NXE если есть).
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 1 << 8
    or   eax, 1 << 11
    wrmsr

    ; CR0.PG = 1 → Long Mode (compat-режим, пока без 64-bit CS).
    mov  eax, cr0
    or   eax, 1 << 31
    mov  cr0, eax

    ; Грузим 64-bit GDT и далеко прыгаем в 64-bit code segment.
    lgdt [tramp_gdt64_desc - trampoline_start + TRAMPOLINE_PHYS]
    jmp  0x08:(tramp_lm64 - trampoline_start + TRAMPOLINE_PHYS)


[bits 64]
tramp_lm64:
    ; data segments
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Атомарно забираем свой слот (1, 2, 3, ...): inc и взять старое значение.
    mov  rax, qword tramp_ap_slot - trampoline_start + TRAMPOLINE_PHYS
    mov  r9d, 1
    lock xadd dword [rax], r9d
    ; r9d теперь = старое значение счётчика = мой слот (1, 2, 3, ...).

    ; Достаём указатель на массив (это поле хранит kernel-адрес ap_stacks_top[]).
    mov  rax, qword tramp_ap_stacks_top - trampoline_start + TRAMPOLINE_PHYS
    mov  rax, [rax]                ; rax = &ap_stacks_top[0]
    mov  ecx, r9d                  ; rcx = slot (zero-extended)
    shl  rcx, 3                    ; * 8 байт на qword
    add  rax, rcx
    mov  rsp, [rax]                ; rsp = ap_stacks_top[slot]

    ; SysV ABI: align RSP по 16, передаём 0 аргументов.
    and  rsp, ~0xF
    xor  rbp, rbp

    ; Вызываем C-точку входа.
    mov  rax, qword ap_main64
    call rax

.halt:
    cli
    hlt
    jmp  .halt


; ----------------------------------------------------------------------------
; Данные trampoline — последние, чтобы расположение в [TRAMPOLINE_PHYS .. _end]
; было известно и стабильно.
; ----------------------------------------------------------------------------
section .text
align 8

; Временный GDT для 32-bit PM (3 entry).
align 8
tramp_gdt32_start:
    dq 0x0000000000000000          ; null
    dq 0x00CF9A000000FFFF          ; 32-bit code (0x08)
    dq 0x00CF92000000FFFF          ; 32-bit data (0x10)
tramp_gdt32_end:
tramp_gdt32_desc:
    dw tramp_gdt32_end - tramp_gdt32_start - 1
    dd tramp_gdt32_start - trampoline_start + TRAMPOLINE_PHYS

; 64-bit GDT для перехода в Long Mode.
align 8
tramp_gdt64_start:
    dq 0x0000000000000000          ; null
    dq 0x00AF9A000000FFFF          ; 64-bit code (L=1) (0x08)
    dq 0x00CF92000000FFFF          ; 64-bit data       (0x10)
tramp_gdt64_end:
tramp_gdt64_desc:
    dw tramp_gdt64_end - tramp_gdt64_start - 1
    dq tramp_gdt64_start - trampoline_start + TRAMPOLINE_PHYS

; Заполняется BSP перед SMP startup:
;   tramp_pml4_phys     — физ. адрес общего PML4 (boot_pml4)
;   tramp_ap_stacks_top — pointer to array[MAX_CPUS] of uint64_t (top of each AP stack)
;   tramp_ap_slot       — slot counter, инициализируется в 1 (BSP = 0)
align 8
tramp_pml4_phys:        dq 0
tramp_ap_stacks_top:    dq 0
tramp_ap_slot:          dq 0

; MXCSR-инит: маскируем все 6 SIMD FP-исключений (default x86 ABI).
; HOTFIX KVM: загружаем ldmxcsr из этой константы вместо push/pop, потому
; что у AP в trampoline нет валидного ESP (он = 0 или undefined после SIPI).
align 4
tramp_mxcsr_init:       dd 0x1F80

trampoline_end:
