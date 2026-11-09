; ============================================================================
; syscall_entry.asm — точка входа SYSCALL для AMPLIFY-OS x86_64.
; ----------------------------------------------------------------------------
; Когда user-mode выполняет инструкцию SYSCALL, CPU:
;   1) Загружает CS = STAR[47:32]    (kernel_CS, у нас 0x08)
;   2) Загружает SS = STAR[47:32]+8  (kernel_DS, 0x10)
;   3) RCX  = старый RIP user-mode
;   4) R11  = старый RFLAGS
;   5) RIP  = LSTAR  (= syscall_entry, эта функция)
;   6) RFLAGS &= ~FMASK  (мы маскируем IF и DF)
;   7) RSP  ОСТАЁТСЯ user'ским — мы сами должны переключить!
;
; SYSRETQ обратно делает зеркальное:
;   1) CS = STAR[63:48]+16, SS = STAR[63:48]+8 (с RPL=3)
;   2) RIP = RCX, RFLAGS = R11
;   3) RSP не восстанавливается (мы сами должны)
;
; Этап 40 группа B: добавлена проверка pending signals через
; sig_check_and_deliver_user() после syscall_dispatch. Функция получает
; указатели на kernel-stack слоты RIP (RCX) и RFLAGS (R11) и может их
; переопределить для прыжка в signal handler.
; ============================================================================
[bits 64]

extern syscall_dispatch
extern sig_check_and_deliver_user

global syscall_entry
global kernel_rsp_save
global user_rsp_save

section .bss
align 16
kernel_rsp_save:   resq 1     ; куда переключаемся при входе в kernel
user_rsp_save:     resq 1     ; куда вернёмся при выходе в user

section .text

; ----------------------------------------------------------------------------
; void syscall_entry(void); — RIP, на который CPU прыгает по SYSCALL.
; ----------------------------------------------------------------------------
syscall_entry:
    ; Сохраняем user RSP, грузим kernel RSP.
    mov  [rel user_rsp_save], rsp
    mov  rsp, [rel kernel_rsp_save]    ; 16-aligned (KSTACK_TOP)

    ; ------------------------------------------------------------------------
    ; [этап 44] Сохранение FPU/SSE state user-процесса.
    ; ------------------------------------------------------------------------
    ; Когда user-mode делает syscall, его FPU-state (x87 + xmm0..xmm15 +
    ; MXCSR) лежит в регистрах. Ядро между SYSCALL и SYSRETQ может (и будет!)
    ; пользоваться FPU/SSE — рантайм-математика, memcpy через SSE, GLES
    ; рендер, всё это активно использует xmm-регистры. Без сохранения мы бы
    ; вернулись в user с разрушенным FPU-state.
    ;
    ; Слот для FXSAVE — 512 байт на kernel-stack'е, прямо под kernel_rsp_save.
    ; ------------------------------------------------------------------------
    sub  rsp, 512                       ; RSP при kernel_rsp_save 16-aligned,
                                        ; 512 mod 16 = 0 → остаёмся aligned.
    fxsave [rsp]

    ; Сохраняем то, что нужно SYSRETQ: user RIP (RCX) и user RFLAGS (R11).
    push rcx                            ; -8 от FXSAVE-конца
    push r11                            ; -16

    ; Строим syscall_args_t на стеке (порядок снизу вверх):
    ;   [rsp + 0]  = num   (RAX)
    ;   [rsp + 8]  = a1    (RDI)
    ;   [rsp + 16] = a2    (RSI)
    ;   [rsp + 24] = a3    (RDX)
    ;   [rsp + 32] = a4    (R10)
    ;   [rsp + 40] = a5    (R8)
    ;   [rsp + 48] = a6    (R9)
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax                            ; -72 от FXSAVE-конца (8 mod 16)

    ; SysV: первый аргумент C-функции — RDI = указатель на структуру.
    mov  rdi, rsp
    sub  rsp, 8                         ; -80 (16-aligned для SysV CALL)
    cld
    call syscall_dispatch
    add  rsp, 8                         ; снимаем выравнивающую заплатку
    ; rax = возвращаемое значение, кладём его обратно в args.num
    mov  [rsp + 0], rax

    ; Попаем структуру.
    pop  rax
    pop  rdi
    pop  rsi
    pop  rdx
    pop  r10
    pop  r8
    pop  r9

    ; Теперь стек: [rsp + 0] = сохранённый R11 (user RFLAGS),
    ;              [rsp + 8] = сохранённый RCX (user RIP).
    ; Этап 40 группа B: проверяем pending signals. sig_check_and_deliver_user
    ; получает RDI = pointer на RIP-слот, RSI = pointer на RFLAGS-слот.
    ; Функция может переопределить их значения для доставки signal-handler'а.
    lea  rdi, [rsp + 8]            ; &saved_rip
    lea  rsi, [rsp + 0]            ; &saved_rflags
    ; RAX уже содержит возвращаемое значение syscall — сохраняем его через push.
    push rax
    sub  rsp, 8                    ; 16-byte alignment для SysV call
    call sig_check_and_deliver_user
    add  rsp, 8
    pop  rax

    ; Восстанавливаем user RFLAGS (R11) и user RIP (RCX).
    pop  r11
    pop  rcx

    ; [этап 44] Восстанавливаем user FPU/SSE state.
    fxrstor [rsp]                       ; RSP сейчас точно совпадает с местом FXSAVE
    add  rsp, 512                       ; снимаем FPU-scratch со стека

    ; Возвращаемся на user стек.
    mov  rsp, [rel user_rsp_save]

    ; SYSRETQ: возврат в Ring 3.
    o64 sysret
