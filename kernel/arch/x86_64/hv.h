// ============================================================================
// hv.h — ядро гипервизора phantom: ВМ, её память и vCPU (этап 50.1).
// ----------------------------------------------------------------------------
// Место в слоях (задумано как у KVM):
//   vmx.c      — что умеет процессор, VMXON/VMXOFF            (kvm-intel)
//   hv.c       — ВМ: память через EPT, VMCS, цикл входа и
//                выхода, то, что гипервизор решает сам        (kvm)
//   модель     — устройства гостя: порты, BIOS, диск, экран   (QEMU)
// Между hv и моделью устройств один контракт, как у ioctl(KVM_RUN): «исполняй
// гостя, пока не понадобится устройство». Всё, что hv умеет сам (CPUID,
// регистры управления, MSR, прерывания хоста, конец кванта), наружу не
// выходит; модель видит только hv_exit_t.
//
// Отличие от KVM, которое определяет весь файл: там vCPU — поток, которым
// распоряжается планировщик. У нас планировщика на AP нет, поэтому vCPU —
// «жилец» одного AP (smp_pool.h): исполняется квантами и делит ядро с пулом.
// Отсюда же правило: обработчик выходов (hv_exit_fn) работает НА AP и обязан
// обходиться чистыми вычислениями над заранее выделенной памятью — никаких
// kmalloc, VFS, диска, экрана и печати (CLAUDE.md §5.11).
// ============================================================================
#ifndef AOS_X86_64_HV_H
#define AOS_X86_64_HV_H

#include <stdint.h>
#include <stdbool.h>

// Регистры общего назначения гостя. RSP, RIP и RFLAGS живут в VMCS.
// Раскладку знает vmx_entry.asm — менять только вместе с ним.
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t cr2;
    uint64_t host_rsp;          // кэш поля HOST_RSP (см. vmx_entry.asm)
} vmx_regs_t;

// Причины, по которым гость отдаётся модели устройств.
enum {
    HV_EXIT_IO = 1,     // команда IN/OUT
    HV_EXIT_HLT,        // гость выполнил HLT, и будить его нечем: у модели нет
                        // прерываний либо гость запретил их (IF=0)
    HV_EXIT_VMCALL,     // гость позвал гипервизор (регистры — в hv_regs)
    HV_EXIT_EPT,        // обращение к гостевой памяти, которой нет в EPT
    HV_EXIT_SHUTDOWN,   // тройной сбой гостя
    HV_EXIT_FAIL,       // вход не состоялся или выход, которого мы не умеем
};

typedef struct {
    int      reason;
    // HV_EXIT_IO. Для IN модель кладёт ответ в value (по умолчанию — все
    // единицы, как на шине без устройства).
    uint16_t port;
    uint8_t  size;              // 1, 2 или 4 байта
    bool     in;
    uint32_t value;
    // HV_EXIT_EPT
    uint64_t gpa;
    uint64_t qual;              // бит 0 чтение, 1 запись, 2 исполнение
    // HV_EXIT_FAIL
    uint32_t hw_reason;         // причина выхода процессора как есть
    uint32_t insn_error;        // код ошибки команды VMX (поле 0x4400)
} hv_exit_t;

// Ответ модели устройств.
enum { HV_STOP = 0, HV_RESUME = 1 };

struct hv_vm;
typedef struct hv_vm hv_vm_t;
typedef int (*hv_exit_fn)(hv_vm_t* vm, hv_exit_t* ex, void* ctx);   // на AP!

// [50.2] Модель устройств с прерываниями. Кроме обработчика выходов — три
// вопроса, которые hv задаёт перед каждым входом в гостя (всё на AP гостя):
//   irq_pending — «учти, что сейчас момент now (TSC), и скажи, ждёт ли гостя
//                 прерывание»; здесь модель двигает свои таймеры;
//   irq_ack     — гость прерывание принимает: вернуть вектор (как цикл INTA
//                 у контроллера 8259); отрицательное — уже нечего;
//   deadline    — TSC ближайшего события модели (0 — нет): до него hv
//                 ограничит квант таймером вытеснения, а спящего гостя
//                 разбудит (hv_tick_1ms).
// Доставку делает hv: если гость сейчас принять не может (IF=0, только что
// был STI/MOV SS), включается выход «окно прерываний». HLT при IF=1 — не
// конец гостя, а сон vCPU до прерывания: ядро на это время свободно для пула.
typedef struct {
    hv_exit_fn on_exit;
    bool     (*irq_pending)(hv_vm_t* vm, void* ctx, uint64_t now_tsc);
    int      (*irq_ack)(hv_vm_t* vm, void* ctx);
    uint64_t (*deadline)(hv_vm_t* vm, void* ctx);
} hv_model_t;

// С чего начинает гость.
enum { HV_START_REAL = 0, HV_START_PROT32 };
typedef struct {
    int      mode;
    uint16_t cs, ip;            // REAL: адрес первой команды cs:ip
    uint16_t sp;                // REAL: стек 0000:sp
    uint32_t edx;               // REAL: что лежит в EDX (BIOS передаёт номер диска в DL)
    bool     intr_on;           // REAL: прерывания разрешены с первой команды
    // PROT32 — состояние, которого ждёт 32-битный протокол загрузки Linux:
    // защищённый режим без страниц, плоские сегменты CS=0x10, DS/ES/SS=0x18.
    uint32_t eip, esi;
    uint32_t gdt_gpa;           // GDT гостя с этими двумя дескрипторами
} hv_start_t;

enum { HV_VCPU_NEW = 0, HV_VCPU_RUNNABLE, HV_VCPU_STOPPED, HV_VCPU_HALTED };

// Последние выходы — чтобы после сбоя было видно, чем гость занимался.
#define HV_TRACE_N 32
typedef struct {
    uint32_t reason;            // причина выхода процессора
    uint32_t arg;               // порт, номер MSR или уточнение — что было под рукой
} hv_trace_t;

typedef struct {
    uint64_t exits, slices;
    uint64_t io, cpuid, cr, msr, extint, ptimer, ept, vmcall, hlt, other;
    uint64_t guest_tsc;         // такты TSC внутри входа в гостя (гость + вход/выход)
    uint64_t msr_unknown;       // обращения к MSR, которых мы не знаем (ушли как #GP)
    uint32_t last_unknown_msr;
    uint32_t unknown_list[8];   // первые разные номера таких MSR
    int      unknown_n;
    uint64_t injected;          // прерываний доставлено гостю
    uint64_t windows;           // выходов «окно прерываний»
    uint64_t sleeps;            // раз гость засыпал по HLT
    uint64_t sleep_max_tsc;     // самый долгий такой сон, в тактах TSC
    uint64_t sleeps_open;       // снов, когда у модели не было заказано ни одного события
} hv_stats_t;

// Состояние гостя в момент остановки — для разбора сбоев (снимается на AP,
// печатается на BSP).
typedef struct {
    uint64_t rip, rsp, rflags, cr0, cr3, cr4, efer;
    uint64_t cs_base;
    uint32_t cs_sel, cs_ar, intr_info;
} hv_diag_t;

#define HV_VM_SMALL_PAGES  1    // EPT только 4-КБ страницами (проверка второго пути)

// ---- BSP ---------------------------------------------------------------------
// Память гостя — один кусок [0, ram_bytes), округляется вверх до 2 МБ.
// Включает VMX на ядрах (если это первая ВМ). 0 — отказ, причина в hv_last_error.
hv_vm_t*    hv_vm_create(uint64_t ram_bytes, uint32_t flags);
// Останавливает vCPU, освобождает всё, выключает VMX вместе с последней ВМ.
void        hv_vm_destroy(hv_vm_t* vm);
const char* hv_last_error(void);

// Указатель хоста на память гостя или 0, если диапазон не целиком в RAM гостя.
void*       hv_gpa(hv_vm_t* vm, uint64_t gpa, uint64_t len);
uint64_t    hv_ram_size(hv_vm_t* vm);
// Страница хоста сразу за памятью гостя, в EPT её нет: цела ли она.
bool        hv_guard_intact(hv_vm_t* vm);

// Запустить vCPU в реальном режиме с адреса cs:ip (как после загрузчика BIOS).
bool        hv_vcpu_start_real(hv_vm_t* vm, uint16_t cs, uint16_t ip, hv_exit_fn fn, void* ctx);
// [50.2] Общий запуск: начальное состояние + модель устройств с прерываниями.
bool        hv_vcpu_start(hv_vm_t* vm, const hv_start_t* st, const hv_model_t* model, void* ctx);
// У модели новость для спящего или исполняющегося гостя (пришёл символ…).
void        hv_vcpu_kick(hv_vm_t* vm);
// Время «на стене» в момент старта гостя (секунды Unix) — для его часов.
void        hv_set_wallclock(hv_vm_t* vm, uint64_t unix_sec);
// Из обработчика таймера ядра, раз в миллисекунду: будит ядро гостя, уснувшего
// по HLT, когда подошло событие его модели. Дёшево, когда гостей нет.
void        hv_tick_1ms(void);
// Последние выходы: массив из HV_TRACE_N записей, *head — куда ляжет следующая.
const hv_trace_t* hv_trace(hv_vm_t* vm, int* head);
// Попросить vCPU остановиться и дождаться этого (до ~3 с).
bool        hv_vcpu_stop(hv_vm_t* vm);
// Ждать, пока гость остановится сам. false — время вышло.
bool        hv_vcpu_wait(hv_vm_t* vm, uint32_t timeout_ms);

int               hv_vcpu_state(hv_vm_t* vm);
int               hv_vcpu_slot(hv_vm_t* vm);
const hv_stats_t* hv_stats(hv_vm_t* vm);
const hv_exit_t*  hv_last_exit(hv_vm_t* vm);
const hv_diag_t*  hv_diag(hv_vm_t* vm);
const char*       hv_vcpu_error(hv_vm_t* vm);
uint64_t          hv_tsc_per_ms(void);

// ---- AP, из обработчика выходов ------------------------------------------------
vmx_regs_t* hv_regs(hv_vm_t* vm);
// [50.3] Состояние гостя, которое живёт в VMCS (её видно только с ядра гостя):
// счётчик команд, стек, флаги, сегменты. Нужны службам BIOS: параметры там
// приходят через сегмент:смещение, а результат уходит флагом переноса.
enum {
    HV_G_RIP, HV_G_RSP, HV_G_RFLAGS,
    HV_G_CS_BASE, HV_G_DS_BASE, HV_G_ES_BASE, HV_G_SS_BASE,
    HV_G_CS_SEL, HV_G_DS_SEL, HV_G_ES_SEL, HV_G_SS_SEL,
    HV_G_COUNT
};
uint64_t hv_guest_get(hv_vm_t* vm, int what);
void     hv_guest_set(hv_vm_t* vm, int what, uint64_t value);

#endif
