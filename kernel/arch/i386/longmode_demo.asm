; ============================================================================
; longmode_demo.asm — однократный round-trip 32-bit -> Long Mode -> 32-bit.
; ----------------------------------------------------------------------------
; uint32_t lm_demo_trampoline(uint32_t pml4_phys, uint64_t* out_signature);
;
; Алгоритм перехода 32-bit -> Long Mode (Intel SDM Vol.3 §10.8):
;   1. Сохраняем GDTR, CR3, CR4, EFER, регистры (callee-save).
;   2. Загружаем временный GDT с 64-bit code segment + 32-bit code + data.
;   3. Выключаем paging (CR0.PG=0).
;   4. Включаем PAE (CR4.PAE=1).
;   5. Загружаем PML4 в CR3.
;   6. Включаем LME в EFER (MSR 0xC0000080, bit 8).
;   7. Включаем paging (CR0.PG=1) — CPU вошёл в Long Mode (compat 32-bit).
;   8. Far-JMP на 64-bit code selector (0x08 во временном GDT)
;      — теперь CPU в 64-bit submode.
;   9. В 64-bit коде записываем сигнатуру по адресу out_signature.
;  10. Far-JMP на 32-bit code selector (0x10 во временном GDT)
;      — обратно в compatibility 32-bit Long Mode.
;  11. Выключаем paging, выключаем LME, выключаем PAE.
;  12. Восстанавливаем оригинальный CR3, GDTR.
;  13. Включаем paging обратно — мы снова в обычном 32-bit protected.
;  14. Возвращаем EAX = 1 / 0.
;
; Все шаги выполняются с identity-mapped физической памятью, поэтому
; адреса меток внутри функции одинаково валидны и в 32-, и в 64-bit.
; ============================================================================

; ----------------------------------------------------------------------------
; Временные данные (выровнены).
; ----------------------------------------------------------------------------
section .data
align 8

; -- Временный GDT для перехода в LM ----------------------------------------
; 0x00 — null
; 0x08 — 64-bit code  (L=1, D=0)
; 0x10 — 32-bit code  (L=0, D=1, G=1)
; 0x18 — 32-bit data  (RW, G=1)
lm_tmp_gdt:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF      ; 0x08: 64-bit code
    dq 0x00CF9A000000FFFF      ; 0x10: 32-bit code
    dq 0x00CF92000000FFFF      ; 0x18: data
lm_tmp_gdt_end:

lm_tmp_gdt_ptr:
    dw lm_tmp_gdt_end - lm_tmp_gdt - 1
    dd lm_tmp_gdt

; -- Сохраняемые значения ---------------------------------------------------
align 8
saved_gdtr:    times 8 db 0      ; 6-byte GDTR (limit + base), padded to 8.
saved_cr3:     dd 0
saved_cr4:     dd 0
saved_outsig:  dd 0              ; uint64_t* out_signature
saved_pml4:    dd 0              ; uint32_t pml4_phys
g_lm_flag:     dd 0              ; ставится в 1 после успешного 64-bit кода

; -- m16:32 указатель для возврата в 32-bit (используется в 64-bit JMP FAR) -
align 4
far32_ptr:
    dd 0                       ; offset (заполнится перед jmp)
    dw 0x10                    ; selector = 0x10 (32-bit code в lm_tmp_gdt)

; ----------------------------------------------------------------------------
; Код
; ----------------------------------------------------------------------------
section .text

global lm_demo_trampoline

[bits 32]

; ----------------------------------------------------------------------------
; lm_demo_trampoline (cdecl):
;   [esp + 4]  = pml4_phys
;   [esp + 8]  = out_signature
; ----------------------------------------------------------------------------
lm_demo_trampoline:
    pushad                       ; 8*4=32 байта
    pushfd                       ; +4 = 36

    ; ---- Сохраняем GDTR -----------------------------------------------------
    sgdt [saved_gdtr]

    ; ---- Извлекаем аргументы (с учётом pushad+pushfd+ret) -------------------
    ;   esp+0  = saved EFLAGS (pushfd)
    ;   esp+4  = saved EAX..EDI (pushad, 32 байта)
    ;   esp+36 = return address (4)
    ;   esp+40 = pml4_phys
    ;   esp+44 = out_signature
    mov eax, [esp + 40]
    mov [saved_pml4], eax
    mov edx, [esp + 44]
    mov [saved_outsig], edx

    ; Заполняем far32_ptr.offset на адрес метки .back_to_32 (32-bit address).
    mov eax, lm_back_to_32
    mov [far32_ptr], eax

    ; ---- Сохраняем CR3 / CR4 -----------------------------------------------
    mov eax, cr3
    mov [saved_cr3], eax
    mov eax, cr4
    mov [saved_cr4], eax

    ; ---- 1) Выключаем paging (CR0.PG = 0) ----------------------------------
    mov eax, cr0
    and eax, 0x7FFFFFFF
    mov cr0, eax

    ; ---- 2) Включаем PAE (CR4.PAE = 1) -------------------------------------
    mov eax, cr4
    or  eax, 0x20
    mov cr4, eax

    ; ---- 3) Загружаем PML4 в CR3 -------------------------------------------
    mov eax, [saved_pml4]
    mov cr3, eax

    ; ---- 4) Включаем LME в EFER (MSR 0xC0000080) ---------------------------
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 0x100               ; bit 8 = LME
    wrmsr

    ; ---- 5) Загружаем временный GDT ----------------------------------------
    lgdt [lm_tmp_gdt_ptr]

    ; ---- 6) Включаем paging (CR0.PG = 1) -----------------------------------
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax
    ; CPU теперь в IA-32e, но CS всё ещё 32-bit -> compatibility submode.

    ; ---- 7) Far-JMP на 64-bit code segment ---------------------------------
    jmp 0x08:lm_64_entry


; ----------------------------------------------------------------------------
; 64-bit код
; ----------------------------------------------------------------------------
[bits 64]

lm_64_entry:
    ; Загрузим селекторы данных (нужно для совместимости).
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Записываем 64-битную сигнатуру по адресу out_signature.
    ; saved_outsig — 32-битный физ.адрес. В 64-bit identity-mapped это
    ; же — виртуальный адрес. Расширяем до 64 бит mov eax, ... (zero-ext).
    mov  eax, [saved_outsig]
    mov  rcx, 0x34364649_4C504D41    ; "AMPLIF64" little-endian
    mov  qword [rax], rcx

    ; Отмечаем, что 64-bit код выполнился.
    mov  dword [g_lm_flag], 1

    ; Far-JMP назад в 32-bit code: jmp far m16:32 через память [far32_ptr].
    ; ВАЖНО: в 64-bit режиме нужно явно указать "dword" чтобы NASM
    ; не добавил REX.W (который требует m16:64 — 10 байт).
    ; m16:32 — 6 байт: 2 байта selector + 4 байта offset.
    jmp far dword [far32_ptr]


; ----------------------------------------------------------------------------
; Возврат в 32-bit compatibility (всё ещё в Long Mode IA-32e!)
; ----------------------------------------------------------------------------
[bits 32]

lm_back_to_32:
    ; Селектор 0x18 — data из lm_tmp_gdt (32-bit data).
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- Выключаем paging (PG=0) ------------------------------------------
    mov eax, cr0
    and eax, 0x7FFFFFFF
    mov cr0, eax

    ; ---- Выключаем LME ----------------------------------------------------
    mov ecx, 0xC0000080
    rdmsr
    and eax, 0xFFFFFEFF          ; clear bit 8 (LME)
    wrmsr

    ; ---- Восстанавливаем оригинальный CR3 ---------------------------------
    mov eax, [saved_cr3]
    mov cr3, eax

    ; ---- Восстанавливаем CR4 ----------------------------------------------
    mov eax, [saved_cr4]
    mov cr4, eax

    ; ---- Включаем paging --------------------------------------------------
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax

    ; ---- Восстанавливаем оригинальный GDT --------------------------------
    lgdt [saved_gdtr]

    ; ---- Перезагружаем CS на оригинальный kernel code selector (0x08) ----
    jmp 0x08:lm_restore_segs

lm_restore_segs:
    mov ax, 0x10                 ; оригинальный GDT_KERNEL_DATA = 2*8 = 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- Возвращаем результат ---------------------------------------------
    mov eax, [g_lm_flag]

    popfd
    popad
    ret
