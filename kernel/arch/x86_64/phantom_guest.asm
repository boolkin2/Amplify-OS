; ============================================================================
; phantom_guest.asm — проверочный гость гипервизора phantom (этап 50.1).
; ----------------------------------------------------------------------------
; Это НЕ код ядра: кусок между phantom_guest_start и phantom_guest_end
; копируется в память гостя по адресу GLOAD и исполняется там процессором в
; режиме VMX non-root. Лежит в .rodata как данные.
;
; Гость проходит тот же путь, что настоящая машина при загрузке: стартует в
; РЕАЛЬНОМ режиме, сам включает защищённый, строит таблицы страниц и входит в
; длинный. По дороге проверяет то, что обещает гипервизор, и докладывает
; буквами в отладочный порт:
;   R — реальный режим исполняется; CPUID показывает гипервизор и прячет VMX
;   P — защищённый режим (запись CR0.PE прошла через гипервизор)
;   L — длинный режим (EFER.LME + CR0.PG; гость видит EFER.LMA)
;   I — команда IN получила ответ модели устройств
;   F — регистры xmm пережили выход в гипервизор (хост их нарочно портит)
;   V — VMCALL вернулся
;   ! — проверка не прошла
; Дальше — по номеру режима из блока параметров, который кладёт хост.
;
; Файл собирается в elf64 вместе с ядром, поэтому ORG здесь нет: адреса внутри
; гостя считаются как (метка - начало + GLOAD), см. макрос G — тот же приём,
; что в smp_trampoline.asm.
; ============================================================================

%define GLOAD      0x1000                   ; куда хост копирует гостя
%define G(x)       ((x) - phantom_guest_start + GLOAD)

PORT_NULL   equ 0xE7                        ; «устройство», которое ничего не делает (замер)
PORT_RES    equ 0xE8                        ; 32-битный результат
PORT_CHAR   equ 0xE9                        ; буква отчёта
PORT_XMM    equ 0xEA                        ; «хост, испорти xmm»
PORT_IN     equ 0xEB                        ; чтение: модель отвечает IN_MAGIC
IN_MAGIC    equ 0xC0FFEE42

PARAM       equ 0x500                       ; dword: режим (см. ниже)
PARG        equ 0x508                       ; qword: аргумент режима
COUNTER     equ 0x600                       ; qword: счётчик режима «крутиться»
SUM_BASE    equ 0x20000                     ; 64 КБ данных, сумму которых знает хост
SUM_BYTES   equ 0x10000
PT_BASE     equ 0x10000                     ; таблицы страниц гостя (3 страницы)

MODE_HALT   equ 0                           ; пройти проверки и остановиться
MODE_EPT    equ 1                           ; записать по адресу PARG (вне памяти гостя)
MODE_SPIN   equ 2                           ; считать в цикле без конца
MODE_IO     equ 3                           ; PARG команд OUT подряд (цена выхода)
MODE_TRIPLE equ 4                           ; тройной сбой
MODE_REAL   equ 5                           ; только реальный режим: R и HLT

section .rodata

global phantom_guest_start
global phantom_guest_end

phantom_guest_start:

; ---------------------------------------------------------------------------
[bits 16]
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x8000

    mov  eax, 1
    cpuid
    bt   ecx, 31                            ; гипервизор обязан быть виден
    jnc  .bad16
    bt   ecx, 5                             ; а VMX — скрыт
    jc   .bad16
    mov  al, 'R'
    out  PORT_CHAR, al

    cmp  dword [PARAM], MODE_REAL
    je   .halt16

    lgdt [G(gdt_desc)]
    mov  eax, cr0
    or   eax, 1                             ; PE — этот бит гипервизор держит у себя
    mov  cr0, eax
    jmp  dword 0x08:G(pm32)

.bad16:
    mov  al, '!'
    out  PORT_CHAR, al
.halt16:
    hlt
    jmp  .halt16

; ---------------------------------------------------------------------------
[bits 32]
pm32:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  esp, 0x8000
    mov  al, 'P'
    out  PORT_CHAR, al

    ; Таблицы страниц: PML4 -> PDPT -> PD, первый гигабайт 2-МБ страницами
    ; один-в-один. Памяти у гостя меньше — обращение за её край должно
    ; кончиться не сбоем страницы у гостя, а выходом EPT у гипервизора.
    mov  edi, PT_BASE
    xor  eax, eax
    mov  ecx, 0x3000 / 4
    rep  stosd
    mov  dword [PT_BASE], PT_BASE + 0x1000 + 3
    mov  dword [PT_BASE + 0x1000], PT_BASE + 0x2000 + 3
    mov  edi, PT_BASE + 0x2000
    mov  eax, 0x83                          ; присутствует, запись, 2 МБ
    mov  ecx, 512
.pd:
    mov  [edi], eax
    add  eax, 0x200000
    add  edi, 8
    loop .pd

    mov  eax, cr4
    or   eax, (1 << 5) | (1 << 9) | (1 << 10)   ; PAE, OSFXSR, OSXMMEXCPT
    mov  cr4, eax
    mov  eax, PT_BASE
    mov  cr3, eax
    mov  ecx, 0xC0000080                    ; EFER
    rdmsr
    or   eax, 1 << 8                        ; LME
    wrmsr
    mov  eax, cr0
    or   eax, 1 << 31                       ; PG -> длинный режим
    mov  cr0, eax
    jmp  0x18:G(lm64)

; ---------------------------------------------------------------------------
[bits 64]
lm64:
    mov  al, 'L'
    out  PORT_CHAR, al

    mov  ecx, 0xC0000080
    rdmsr
    bt   eax, 10                            ; LMA виден гостю
    jnc  bad64

    in   eax, PORT_IN
    cmp  eax, IN_MAGIC
    jne  bad64
    mov  al, 'I'
    out  PORT_CHAR, al

    ; xmm гостя обязаны пережить выход: обработчик порта PORT_XMM на стороне
    ; хоста нарочно затирает все шестнадцать регистров.
    movdqu xmm0,  [rel pat0]
    movdqu xmm7,  [rel pat1]
    movdqu xmm15, [rel pat0]
    xor  eax, eax
    out  PORT_XMM, al
    movdqu xmm1, [rel pat0]
    movdqu xmm2, [rel pat1]
    pcmpeqb xmm0, xmm1
    pmovmskb eax, xmm0
    cmp  eax, 0xFFFF
    jne  bad64
    pcmpeqb xmm7, xmm2
    pmovmskb eax, xmm7
    cmp  eax, 0xFFFF
    jne  bad64
    pcmpeqb xmm15, xmm1
    pmovmskb eax, xmm15
    cmp  eax, 0xFFFF
    jne  bad64
    mov  al, 'F'
    out  PORT_CHAR, al

    ; Сумма 64 КБ памяти: гость видит ровно те байты, что положил хост.
    mov  rsi, SUM_BASE
    mov  ecx, SUM_BYTES / 8
    xor  eax, eax
.sum:
    add  rax, [rsi]
    rol  rax, 1
    add  rsi, 8
    dec  ecx
    jnz  .sum
    mov  rbx, rax
    out  PORT_RES, eax                      ; младшая половина
    mov  rax, rbx
    shr  rax, 32
    out  PORT_RES, eax                      ; старшая половина

    vmcall
    mov  al, 'V'
    out  PORT_CHAR, al

    mov  eax, [PARAM]
    cmp  eax, MODE_EPT
    je   mode_ept
    cmp  eax, MODE_SPIN
    je   mode_spin
    cmp  eax, MODE_IO
    je   mode_io
    cmp  eax, MODE_TRIPLE
    je   mode_triple
halt64:
    hlt
    jmp  halt64

mode_ept:
    mov  rdi, [PARG]
    mov  qword [rdi], 0x1234                ; за краем памяти гостя -> выход EPT
    mov  al, '?'                            ; сюда попадать нельзя
    out  PORT_CHAR, al
    jmp  halt64

mode_spin:
    inc  qword [COUNTER]
    jmp  mode_spin

mode_io:
    mov  rcx, [PARG]
    xor  eax, eax
.io:
    out  PORT_NULL, al
    dec  rcx
    jnz  .io
    mov  al, 'B'
    out  PORT_CHAR, al
    jmp  halt64

mode_triple:
    lidt [rel zero_idt]                     ; IDT нулевой длины
    int3                                    ; #BP -> #GP -> #DF -> тройной сбой

bad64:
    mov  al, '!'
    out  PORT_CHAR, al
    jmp  halt64

; ---------------------------------------------------------------------------
align 16
pat0:       db 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
pat1:       db 0xA5, 0x5A, 0xC3, 0x3C, 0x0F, 0xF0, 0x99, 0x66, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88

align 8
gdt:
    dq 0x0000000000000000                   ; 0x00 пусто
    dq 0x00CF9A000000FFFF                   ; 0x08 код 32 бита
    dq 0x00CF92000000FFFF                   ; 0x10 данные
    dq 0x00AF9A000000FFFF                   ; 0x18 код 64 бита
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd G(gdt)

zero_idt:
    dw 0
    dq 0

phantom_guest_end:
