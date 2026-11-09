// ============================================================================
// hv.c — ядро гипервизора phantom (этап 50.1). Контракт и слои — в hv.h.
// ----------------------------------------------------------------------------
// Что здесь решено и почему:
//   * Память гостя — один непрерывный кусок памяти хоста, отображённый через
//     EPT в гостевые адреса с нуля. Гость не может даже НАЗВАТЬ адрес хоста:
//     всё, чего нет в его таблицах EPT, кончается выходом EPT violation.
//   * Гость стартует в РЕАЛЬНОМ режиме (unrestricted guest) и сам переходит в
//     защищённый и длинный — так грузится настоящая машина, и так пойдёт
//     загрузка через BIOS. Биты CR0.PE/PG мы при этом держим у себя (маска
//     CR0): каждая смена режима — выход, на котором мы сами приводим в
//     соответствие EFER.LMA и бит «гость в IA-32e» в управлении входом. Так
//     делает и KVM: полагаться на то, что вложенный гипервизор узла без
//     ошибок эмулирует автоматику процессора для этого угла, незачем.
//   * Прерывания хоста: «внешнее прерывание = выход», подтверждение на
//     выходе выключено. Прерывание остаётся ждать в LAPIC, и после выхода
//     достаточно `sti` — его примет обычный обработчик из IDT.
//   * Квант гостя ограничен таймером вытеснения VMX (2 мс): по его концу
//     vCPU возвращает ядро циклу пула (smp_pool.h, «жилец»).
//   * FPU/SSE: ядро собрано с SSE, обработчики выходов портят xmm. Поэтому
//     вокруг каждого входа — fxsave/fxrstor хоста и гостя.
// Под вложенным KVM без теневой VMCS каждый VMREAD/VMWRITE — выход в
// гипервизор узла. Отсюда экономия на чтениях в частых путях (кэш HOST_RSP,
// состояние блокировки прерываний читается только на HLT).
// ============================================================================
#include "hv.h"
#include "vmx.h"
#include "vmx_defs.h"
#include "cpu.h"
#include "smp_pool.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "io.h"
#include <stddef.h>

#define HV_SLICE_MS       2
#define HV_EPT_MAX_TABLES 96
#define PAGE              4096ull
#define MB2               0x200000ull
#define GUARD_BYTE        0xA5

extern int  vmx_enter(vmx_regs_t* regs, int launched);
extern void vmx_exit_point(void);

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} desc_ptr_t;

struct hv_vm {
    // ---- память ----
    uint64_t ram_size;              // байт, кратно 2 МБ
    uint64_t ram_hpa;               // начало памяти гостя в памяти хоста
    uint64_t alloc_hpa;             // что вернул pmm (до выравнивания)
    uint64_t alloc_pages;
    uint64_t eptp;
    uint64_t ept_tab[HV_EPT_MAX_TABLES];
    int      ept_n;
    uint32_t flags;
    // ---- vCPU ----
    vmx_regs_t regs;
    uint8_t  fx_guest[512] __attribute__((aligned(16)));
    uint8_t  fx_host[512]  __attribute__((aligned(16)));
    uint64_t vmcs;
    volatile int state;
    volatile int stop_req;
    bool     attached;              // vCPU отдан ядру и ещё не снят (ведёт BSP)
    int      slot;
    int      launched;
    hv_model_t model;
    void*    ctx;
    hv_start_t start;
    uint32_t entry_ctls;
    uint32_t pin_ctls;
    uint32_t proc_ctls;
    bool     window_on;             // включён выход «окно прерываний»
    bool     event_queued;          // в VMCS уже лежит событие для ближайшего входа
    uint64_t clock_tsc0;            // TSC, принятый за «время гостя = 0» (kvmclock)
    uint64_t wall_sec;              // время на стене в этот момент
    uint64_t kvm_systime, kvm_wall; // что гость записал в MSR часов KVM
    // MSR, которые команды SYSCALL/SWAPGS гостя читают напрямую: пока vCPU
    // загружен на ядро, в них значения гостя, а значения ядра лежат здесь.
    uint64_t host_kgs, host_star, host_lstar, host_cstar, host_sfmask;
    bool     msrs_swapped;
    hv_trace_t trace[HV_TRACE_N];
    int      trace_head;
    uint64_t sleep_t0;              // TSC начала текущего сна по HLT (0 — не спит)
    bool     lma;                   // гость в длинном режиме (ведём сами)
    uint64_t cr8;
    desc_ptr_t host_gdtr, host_idtr;
    int      wr_fail;               // VMWRITE, отклонённые процессором
    uint32_t wr_fail_field;
    const char* err;
    hv_stats_t st;
    hv_exit_t  last;
    hv_diag_t  diag;
};

static const char* g_err = "";
static uint64_t g_tsc_per_ms;

// Кого и когда будить из таймера BSP (hv_tick_1ms): vCPU, уснувший по HLT.
static volatile uint64_t g_wake_tsc;        // 0 — будить некого
static volatile int      g_wake_slot;

const char* hv_last_error(void) { return g_err; }
uint64_t    hv_tsc_per_ms(void) { return g_tsc_per_ms; }

#define MSR_KERNEL_GS_BASE  0xC0000102
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083
#define MSR_SFMASK          0xC0000084
#define MSR_KVM_WALL_CLOCK  0x4B564D00
#define MSR_KVM_SYSTEM_TIME 0x4B564D01

// ----------------------------------------------------------------------------
// Мелочи процессора.
// ----------------------------------------------------------------------------
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t read_cr0(void) { uint64_t v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void relax(void) { __asm__ __volatile__("pause" ::: "memory"); }

static void zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

// Длина одной миллисекунды в тактах TSC. От неё зависит не только квант
// гостя: по ней идут таймер 8254 гостя и его часы kvmclock, то есть скорость
// времени в госте. Первый вариант мерил 5 мс по миллисекундному счётчику
// ядра и ошибался на ~1 % от запуска к запуску (2497 и 2520 МГц на одном
// узле). Теперь — как это делает сам Linux: канал 2 таймера 8254 отсчитывает
// 20 мс, конец виден в бите 5 порта 0x61. Для НАС чтение этого порта дёшево
// (таймер эмулирует ядро гипервизора узла, а на железе это настоящий порт) —
// в отличие от нашего гостя, которому такой замер не по карману.
static uint64_t tsc_calibrate_pit(void) {
    const uint32_t latch = 1193182u / 50;              // 20 мс
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));        // ворота открыть, динамик молчит
    outb(0x43, 0xB0);                                   // канал 2, два байта, режим 0
    outb(0x42, (uint8_t)latch);
    outb(0x42, (uint8_t)(latch >> 8));
    uint64_t t0 = rdtsc(), spins = 0;
    while (!(inb(0x61) & 0x20) && spins < 20000000ull) spins++;
    uint64_t t1 = rdtsc();
    outb(0x61, p61);
    // Меньше сотни чтений за 20 мс — нас надолго прерывали, замер негоден.
    if (spins < 100 || spins >= 20000000ull) return 0;
    return (t1 - t0) / 20;
}

static void tsc_calibrate(void) {
    if (g_tsc_per_ms) return;
    g_tsc_per_ms = tsc_calibrate_pit();
    if (g_tsc_per_ms) return;
    // Запасной путь — по часам ядра; если и они стоят, берём правдоподобные
    // 2 ГГц, чтобы квант не оказался ни нулём, ни вечностью.
    uint64_t m0 = timer_get_uptime_ms(), spins = 0;
    while (timer_get_uptime_ms() == m0 && spins < 400000000ull) { relax(); spins++; }
    uint64_t t0 = rdtsc(), m1 = timer_get_uptime_ms();
    while (timer_get_uptime_ms() - m1 < 5 && spins < 800000000ull) { relax(); spins++; }
    uint64_t dt = rdtsc() - t0, dm = timer_get_uptime_ms() - m1;
    g_tsc_per_ms = (dm && dt) ? dt / dm : 2000000ull;
}

// ----------------------------------------------------------------------------
// EPT.
// ----------------------------------------------------------------------------
#define EPT_RWX   7ull
#define EPT_WB    (6ull << 3)       // тип памяти листа: write-back
#define EPT_LARGE (1ull << 7)
#define EPT_ADDR  0x000FFFFFFFFFF000ull

static uint64_t ept_new_table(hv_vm_t* vm) {
    if (vm->ept_n >= HV_EPT_MAX_TABLES) return 0;
    uint64_t p = pmm_alloc_page();
    if (p == PMM_INVALID) return 0;
    zero((void*)(uintptr_t)p, PAGE);
    vm->ept_tab[vm->ept_n++] = p;
    return p;
}

static uint64_t* ept_next(hv_vm_t* vm, uint64_t* tab, int idx) {
    if (!(tab[idx] & EPT_RWX)) {
        uint64_t t = ept_new_table(vm);
        if (!t) return 0;
        tab[idx] = t | EPT_RWX;
    }
    return (uint64_t*)(uintptr_t)(tab[idx] & EPT_ADDR);
}

// Отобразить [gpa, gpa+size) гостя на [hpa, …) хоста. big — можно ли ставить
// 2-МБ листы там, где оба адреса выровнены.
static bool ept_map(hv_vm_t* vm, uint64_t gpa, uint64_t hpa, uint64_t size, bool big) {
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(vm->eptp & EPT_ADDR);
    for (uint64_t off = 0; off < size;) {
        uint64_t g = gpa + off, h = hpa + off;
        uint64_t* pdpt = ept_next(vm, pml4, (g >> 39) & 511);
        if (!pdpt) return false;
        uint64_t* pd = ept_next(vm, pdpt, (g >> 30) & 511);
        if (!pd) return false;
        if (big && !(g & (MB2 - 1)) && !(h & (MB2 - 1)) && size - off >= MB2) {
            pd[(g >> 21) & 511] = h | EPT_RWX | EPT_WB | EPT_LARGE;
            off += MB2;
        } else {
            uint64_t* pt = ept_next(vm, pd, (g >> 21) & 511);
            if (!pt) return false;
            pt[(g >> 12) & 511] = h | EPT_RWX | EPT_WB;
            off += PAGE;
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Создание и разрушение ВМ (BSP).
// ----------------------------------------------------------------------------
static void vm_free(hv_vm_t* vm) {
    for (int i = 0; i < vm->ept_n; i++) pmm_free_page(vm->ept_tab[i]);
    if (vm->vmcs) pmm_free_page(vm->vmcs);
    if (vm->alloc_hpa) pmm_free_big(vm->alloc_hpa, vm->alloc_pages);
    kfree(vm);
}

hv_vm_t* hv_vm_create(uint64_t ram_bytes, uint32_t flags) {
    g_err = "";
    if (!vmx_ready()) { g_err = "no usable VT-x on this machine"; return 0; }
    int slot = smp_pool_pick_resident();
    if (!slot) { g_err = "phantom needs a second CPU core for the guest"; return 0; }
    if (!ram_bytes) { g_err = "guest RAM size is zero"; return 0; }

    uint32_t on = vmx_enable_all();
    if (!(on & (1u << slot))) {
        vmx_disable_all();
        g_err = "VMXON failed on the guest core";
        return 0;
    }
    tsc_calibrate();

    hv_vm_t* vm = (hv_vm_t*)kmalloc_aligned(sizeof(hv_vm_t), 64);
    if (!vm) { vmx_disable_all(); g_err = "out of kernel heap"; return 0; }
    zero(vm, sizeof(*vm));
    vm->slot = slot;
    vm->flags = flags;
    vm->state = HV_VCPU_STOPPED;
    vm->ram_size = (ram_bytes + MB2 - 1) & ~(MB2 - 1);

    // +2 МБ — чтобы выровнять начало на границу 2 МБ (pmm выравнивает только
    // на страницу, а 2-МБ лист EPT требует выровненного адреса хоста);
    // +1 страница — сторож сразу за памятью гостя: в EPT его нет, и после
    // работы гостя он обязан остаться нетронутым.
    vm->alloc_pages = vm->ram_size / PAGE + MB2 / PAGE + 1;
    vm->alloc_hpa = pmm_alloc_big(vm->alloc_pages);
    if (vm->alloc_hpa == PMM_INVALID) {
        vm->alloc_hpa = 0;
        vm_free(vm); vmx_disable_all();
        g_err = "not enough contiguous RAM for the guest";
        return 0;
    }
    vm->ram_hpa = (vm->alloc_hpa + MB2 - 1) & ~(MB2 - 1);
    zero((void*)(uintptr_t)vm->ram_hpa, vm->ram_size);
    uint8_t* guard = (uint8_t*)(uintptr_t)(vm->ram_hpa + vm->ram_size);
    for (uint64_t i = 0; i < PAGE; i++) guard[i] = GUARD_BYTE;

    uint64_t root = ept_new_table(vm);
    vm->vmcs = pmm_alloc_page();
    if (!root || vm->vmcs == PMM_INVALID) {
        if (vm->vmcs == PMM_INVALID) vm->vmcs = 0;
        vm_free(vm); vmx_disable_all();
        g_err = "out of memory for EPT/VMCS";
        return 0;
    }
    // EPTP: адрес корня, длина обхода 4 (поле = 3), тип памяти таблиц WB.
    vm->eptp = root | (3ull << 3) | 6ull;
    bool big = !(flags & HV_VM_SMALL_PAGES) && ((vmx_caps()->ept_vpid >> 16) & 1);
    if (!ept_map(vm, 0, vm->ram_hpa, vm->ram_size, big)) {
        vm_free(vm); vmx_disable_all();
        g_err = "guest is too large for the EPT table budget";
        return 0;
    }
    zero((void*)(uintptr_t)vm->vmcs, PAGE);
    *(uint32_t*)(uintptr_t)vm->vmcs = vmx_caps()->revision;
    return vm;
}

void hv_vm_destroy(hv_vm_t* vm) {
    if (!vm) return;
    if (!hv_vcpu_stop(vm)) {
        // Ядро гостя не ответило: его память освобождать нельзя — vCPU может
        // ещё исполняться. Утечка лучше порчи чужих страниц.
        g_err = "guest core did not stop; guest memory was not freed";
        return;
    }
    vm_free(vm);
    vmx_disable_all();
}

void* hv_gpa(hv_vm_t* vm, uint64_t gpa, uint64_t len) {
    if (gpa >= vm->ram_size || len > vm->ram_size - gpa) return 0;
    return (void*)(uintptr_t)(vm->ram_hpa + gpa);
}

uint64_t hv_ram_size(hv_vm_t* vm) { return vm->ram_size; }

bool hv_guard_intact(hv_vm_t* vm) {
    const uint8_t* guard = (const uint8_t*)(uintptr_t)(vm->ram_hpa + vm->ram_size);
    for (uint64_t i = 0; i < PAGE; i++) if (guard[i] != GUARD_BYTE) return false;
    return true;
}

int               hv_vcpu_state(hv_vm_t* vm) { return vm->state; }
int               hv_vcpu_slot(hv_vm_t* vm)  { return vm->slot; }
const hv_stats_t* hv_stats(hv_vm_t* vm)      { return &vm->st; }
const hv_exit_t*  hv_last_exit(hv_vm_t* vm)  { return &vm->last; }
const hv_diag_t*  hv_diag(hv_vm_t* vm)       { return &vm->diag; }
const hv_trace_t* hv_trace(hv_vm_t* vm, int* head) { *head = vm->trace_head % HV_TRACE_N; return vm->trace; }
void              hv_set_wallclock(hv_vm_t* vm, uint64_t unix_sec) { vm->wall_sec = unix_sec; }
void              hv_vcpu_kick(hv_vm_t* vm)  { smp_pool_kick(vm->slot); }

void hv_tick_1ms(void) {
    uint64_t d = __atomic_load_n(&g_wake_tsc, __ATOMIC_ACQUIRE);
    if (!d || rdtsc() < d) return;
    __atomic_store_n(&g_wake_tsc, 0, __ATOMIC_RELEASE);
    smp_pool_kick(g_wake_slot);
}
const char*       hv_vcpu_error(hv_vm_t* vm) { return vm->err ? vm->err : ""; }
vmx_regs_t*       hv_regs(hv_vm_t* vm)       { return &vm->regs; }

static const uint32_t g_guest_field[HV_G_COUNT] = {
    VMCS_G_RIP, VMCS_G_RSP, VMCS_G_RFLAGS,
    VMCS_G_CS_BASE, VMCS_G_DS_BASE, VMCS_G_ES_BASE, VMCS_G_SS_BASE,
    VMCS_G_CS_SEL, VMCS_G_DS_SEL, VMCS_G_ES_SEL, VMCS_G_SS_SEL,
};

uint64_t hv_guest_get(hv_vm_t* vm, int what) {
    (void)vm;
    return (what >= 0 && what < HV_G_COUNT) ? vmx_vmread(g_guest_field[what]) : 0;
}

void hv_guest_set(hv_vm_t* vm, int what, uint64_t value) {
    (void)vm;
    if (what >= 0 && what < HV_G_COUNT) vmx_vmwrite(g_guest_field[what], value);
}

// ----------------------------------------------------------------------------
// VMCS (исполняется на AP гостя: писать можно только в ТЕКУЩУЮ VMCS ядра).
// ----------------------------------------------------------------------------
static void W(hv_vm_t* vm, uint32_t field, uint64_t value) {
    if (!vmx_vmwrite(field, value)) {
        if (!vm->wr_fail++) vm->wr_fail_field = field;
    }
}

// Свести желаемые биты с тем, что процессор требует и что разрешает.
static uint32_t ctl_adjust(uint32_t want, uint64_t msr) {
    return (want | (uint32_t)msr) & (uint32_t)(msr >> 32);
}

static void set_seg(hv_vm_t* vm, int n, uint16_t sel, uint64_t base, uint32_t limit, uint32_t ar) {
    // Поля сегментов идут в VMCS с шагом 2 в порядке ES, CS, SS, DS, FS, GS, LDTR, TR.
    W(vm, VMCS_G_ES_SEL   + n * 2, sel);
    W(vm, VMCS_G_ES_BASE  + n * 2, base);
    W(vm, VMCS_G_ES_LIMIT + n * 2, limit);
    W(vm, VMCS_G_ES_AR    + n * 2, ar);
}
enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_LDTR, SEG_TR };

static bool vmcs_setup(hv_vm_t* vm) {
    const vmx_caps_t* c = vmx_caps();

    uint32_t pin   = ctl_adjust(PIN_EXTINT_EXIT | PIN_NMI_EXIT | PIN_PREEMPT_TIMER, c->pin);
    // CR8: без «тени TPR» запись гостя в CR8 попала бы в настоящий TPR ядра.
    // MWAIT/MONITOR/RDPMC гостю не показываем (см. CPUID) — пусть выходят.
    uint32_t proc  = ctl_adjust(PROC_HLT_EXIT | PROC_IO_EXIT | PROC_SECONDARY |
                                PROC_CR8_LOAD_EXIT | PROC_CR8_STORE_EXIT |
                                PROC_MWAIT_EXIT | PROC_MONITOR_EXIT | PROC_RDPMC_EXIT, c->proc);
    uint32_t proc2 = ctl_adjust(PROC2_EPT | PROC2_UNRESTRICTED | PROC2_VPID, c->proc2);
    uint32_t ext   = ctl_adjust(EXIT_HOST_64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER |
                                EXIT_SAVE_PAT | EXIT_LOAD_PAT | EXIT_SAVE_PTIMER, c->exit);
    uint32_t entry = ctl_adjust(ENTRY_LOAD_EFER | ENTRY_LOAD_PAT, c->entry);

    // Без этого набора дальнейшее бессмысленно: лучше понятный отказ, чем
    // «неверное состояние гостя» на первом входе.
    if ((pin   & (PIN_EXTINT_EXIT | PIN_PREEMPT_TIMER)) != (PIN_EXTINT_EXIT | PIN_PREEMPT_TIMER) ||
        (proc  & (PROC_HLT_EXIT | PROC_IO_EXIT | PROC_SECONDARY)) != (PROC_HLT_EXIT | PROC_IO_EXIT | PROC_SECONDARY) ||
        (proc2 & (PROC2_EPT | PROC2_UNRESTRICTED)) != (PROC2_EPT | PROC2_UNRESTRICTED) ||
        (ext   & (EXIT_HOST_64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_PAT | EXIT_LOAD_PAT)) !=
                 (EXIT_HOST_64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_PAT | EXIT_LOAD_PAT) ||
        (entry & (ENTRY_LOAD_EFER | ENTRY_LOAD_PAT)) != (ENTRY_LOAD_EFER | ENTRY_LOAD_PAT)) {
        vm->err = "a required VMX control is not available";
        return false;
    }
    vm->pin_ctls = pin;
    vm->entry_ctls = entry;
    vm->proc_ctls = proc;
    vm->window_on = false;
    vm->event_queued = false;

    W(vm, VMCS_PIN_CTLS, pin);
    W(vm, VMCS_PROC_CTLS, proc);
    W(vm, VMCS_PROC2_CTLS, proc2);
    W(vm, VMCS_EXIT_CTLS, ext);
    W(vm, VMCS_ENTRY_CTLS, entry);
    W(vm, VMCS_EXC_BITMAP, 0);              // исключения гость обрабатывает сам
    W(vm, VMCS_PF_ERR_MASK, 0);
    W(vm, VMCS_PF_ERR_MATCH, 0);
    W(vm, VMCS_CR3_TARGET_COUNT, 0);
    W(vm, VMCS_EXIT_MSR_STORE_CNT, 0);
    W(vm, VMCS_EXIT_MSR_LOAD_CNT, 0);
    W(vm, VMCS_ENTRY_MSR_LOAD_CNT, 0);
    W(vm, VMCS_ENTRY_INTR_INFO, 0);
    W(vm, VMCS_EPTP, vm->eptp);
    // VPID != 0: смена «хост/гость» не сбрасывает TLB. Сам кэш трансляций
    // чистится при загрузке и снятии vCPU (vmx_flush_guest_tlb).
    if (proc2 & PROC2_VPID) W(vm, VMCS_VPID, 1);

    // ---- состояние хоста: значения ЭТОГО ядра ----
    desc_ptr_t gdtr, idtr;
    uint16_t tr;
    __asm__ __volatile__("sgdt %0" : "=m"(gdtr));
    __asm__ __volatile__("sidt %0" : "=m"(idtr));
    __asm__ __volatile__("str %0" : "=r"(tr));
    vm->host_gdtr = gdtr;
    vm->host_idtr = idtr;
    // База TSS — из его 16-байтного дескриптора в GDT.
    const uint8_t* d = (const uint8_t*)(uintptr_t)(gdtr.base + (tr & ~7u));
    uint64_t tr_base = (uint64_t)d[2] | ((uint64_t)d[3] << 8) | ((uint64_t)d[4] << 16) |
                       ((uint64_t)d[7] << 24) | ((uint64_t)(*(const uint32_t*)(d + 8)) << 32);
    uint16_t cs, ss, ds, es, fs, gs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    __asm__ __volatile__("mov %%ss, %0" : "=r"(ss));
    __asm__ __volatile__("mov %%ds, %0" : "=r"(ds));
    __asm__ __volatile__("mov %%es, %0" : "=r"(es));
    __asm__ __volatile__("mov %%fs, %0" : "=r"(fs));
    __asm__ __volatile__("mov %%gs, %0" : "=r"(gs));
    if (!tr || !cs) { vm->err = "host TR or CS selector is zero on the guest core"; return false; }

    W(vm, VMCS_H_CR0, read_cr0());
    W(vm, VMCS_H_CR3, read_cr3());
    W(vm, VMCS_H_CR4, read_cr4());
    // В полях хоста у селекторов обязаны быть нулевые RPL и TI.
    W(vm, VMCS_H_CS_SEL, cs & ~7u);
    W(vm, VMCS_H_SS_SEL, ss & ~7u);
    W(vm, VMCS_H_DS_SEL, ds & ~7u);
    W(vm, VMCS_H_ES_SEL, es & ~7u);
    W(vm, VMCS_H_FS_SEL, fs & ~7u);
    W(vm, VMCS_H_GS_SEL, gs & ~7u);
    W(vm, VMCS_H_TR_SEL, tr & ~7u);
    W(vm, VMCS_H_FS_BASE, rdmsr(MSR_IA32_FS_BASE));
    W(vm, VMCS_H_GS_BASE, rdmsr(MSR_IA32_GS_BASE));
    W(vm, VMCS_H_TR_BASE, tr_base);
    W(vm, VMCS_H_GDTR_BASE, gdtr.base);
    W(vm, VMCS_H_IDTR_BASE, idtr.base);
    W(vm, VMCS_H_SYSENTER_CS, 0);
    W(vm, VMCS_H_SYSENTER_ESP, 0);
    W(vm, VMCS_H_SYSENTER_EIP, 0);
    W(vm, VMCS_H_EFER, rdmsr(MSR_IA32_EFER));
    W(vm, VMCS_H_PAT, rdmsr(MSR_IA32_PAT));
    W(vm, VMCS_H_RIP, (uint64_t)(uintptr_t)&vmx_exit_point);
    vm->regs.host_rsp = 0;                  // HOST_RSP запишет vmx_enter

    // ---- состояние гостя ----
    // NE обязан быть 1 в VMX; гость видит свою версию через «тень».
    bool prot = vm->start.mode == HV_START_PROT32;
    W(vm, VMCS_G_CR0, CR0_NE | CR0_ET | (prot ? CR0_PE : 0));
    W(vm, VMCS_CR0_SHADOW, CR0_ET | (prot ? CR0_PE : 0));
    W(vm, VMCS_CR0_MASK, CR0_PE | CR0_PG | CR0_NE | CR0_NW | CR0_CD);
    // VMXE обязан быть 1 в настоящем CR4, а гостю показываем 0. Под маской —
    // всё, что гостю включать нельзя: VMX/SMX, XSAVE (скрыт в CPUID), PCID и
    // биты, которые процессор не разрешает в VMX вовсе.
    W(vm, VMCS_G_CR4, CR4_VMXE);
    W(vm, VMCS_CR4_SHADOW, 0);
    W(vm, VMCS_CR4_MASK, CR4_VMXE | CR4_SMXE | CR4_PCIDE | CR4_OSXSAVE | ~c->cr4_fixed1);
    W(vm, VMCS_G_CR3, 0);
    W(vm, VMCS_G_DR7, 0x400);

    if (prot) {
        // Плоские 4-ГБ сегменты: 0xC09B — код 32 бита, 0xC093 — данные.
        set_seg(vm, SEG_CS, 0x10, 0, 0xFFFFFFFFu, 0xC09B);
        set_seg(vm, SEG_ES, 0x18, 0, 0xFFFFFFFFu, 0xC093);
        set_seg(vm, SEG_SS, 0x18, 0, 0xFFFFFFFFu, 0xC093);
        set_seg(vm, SEG_DS, 0x18, 0, 0xFFFFFFFFu, 0xC093);
        set_seg(vm, SEG_FS, 0x18, 0, 0xFFFFFFFFu, 0xC093);
        set_seg(vm, SEG_GS, 0x18, 0, 0xFFFFFFFFu, 0xC093);
        W(vm, VMCS_G_GDTR_BASE, vm->start.gdt_gpa);
        W(vm, VMCS_G_GDTR_LIMIT, 0x1F);
    } else {
        // Права: 0x9B — код, 0x93 — данные, оба «присутствует, доступ был».
        set_seg(vm, SEG_CS, vm->start.cs, (uint64_t)vm->start.cs << 4, 0xFFFF, 0x9B);
        set_seg(vm, SEG_ES, 0, 0, 0xFFFF, 0x93);
        set_seg(vm, SEG_SS, 0, 0, 0xFFFF, 0x93);
        set_seg(vm, SEG_DS, 0, 0, 0xFFFF, 0x93);
        set_seg(vm, SEG_FS, 0, 0, 0xFFFF, 0x93);
        set_seg(vm, SEG_GS, 0, 0, 0xFFFF, 0x93);
        W(vm, VMCS_G_GDTR_BASE, 0);
        W(vm, VMCS_G_GDTR_LIMIT, 0xFFFF);
    }
    set_seg(vm, SEG_LDTR, 0, 0, 0xFFFF, 0x82);
    // TR обязан быть «занятым TSS» всегда; тип 11 годится и для входа в
    // длинный режим (тип 3 там уже не принимается).
    set_seg(vm, SEG_TR, 0, 0, 0xFFFF, 0x8B);
    W(vm, VMCS_G_IDTR_BASE, 0);
    W(vm, VMCS_G_IDTR_LIMIT, 0xFFFF);

    W(vm, VMCS_G_RIP, prot ? vm->start.eip : vm->start.ip);
    uint64_t rsp0 = prot ? 0 : vm->start.sp;
    uint64_t rfl0 = 2 | ((!prot && vm->start.intr_on) ? 0x200 : 0);
    W(vm, VMCS_G_RSP, rsp0);
    W(vm, VMCS_G_RFLAGS, rfl0);
    W(vm, VMCS_G_EFER, 0);
    W(vm, VMCS_G_PAT, 0x0007040600070406ull);       // значение после сброса
    W(vm, VMCS_G_DEBUGCTL, 0);
    W(vm, VMCS_G_SYSENTER_CS, 0);
    W(vm, VMCS_G_SYSENTER_ESP, 0);
    W(vm, VMCS_G_SYSENTER_EIP, 0);
    W(vm, VMCS_G_INTR_STATE, 0);
    W(vm, VMCS_G_ACTIVITY, 0);
    W(vm, VMCS_G_PENDING_DBG, 0);
    W(vm, VMCS_LINK_POINTER, ~0ull);

    zero(&vm->regs, sizeof(vm->regs));
    if (prot) vm->regs.rsi = vm->start.esi;
    else      vm->regs.rdx = vm->start.edx;
    vm->lma = false;
    vm->cr8 = 0;
    // MSR системных вызовов и GS ядра: у гостя — как после сброса.
    vm->host_kgs    = rdmsr(MSR_KERNEL_GS_BASE);
    vm->host_star   = rdmsr(MSR_STAR);
    vm->host_lstar  = rdmsr(MSR_LSTAR);
    vm->host_cstar  = rdmsr(MSR_CSTAR);
    vm->host_sfmask = rdmsr(MSR_SFMASK);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_STAR, 0);
    wrmsr(MSR_LSTAR, 0);
    wrmsr(MSR_CSTAR, 0);
    wrmsr(MSR_SFMASK, 0);
    vm->msrs_swapped = true;
    vm->clock_tsc0 = rdtsc();
    if (vm->wr_fail) { vm->err = "VMWRITE was rejected while building the VMCS"; return false; }
    return true;
}

static void diag_capture(hv_vm_t* vm) {
    hv_diag_t* d = &vm->diag;
    d->rip = vmx_vmread(VMCS_G_RIP);
    d->rsp = vmx_vmread(VMCS_G_RSP);
    d->rflags = vmx_vmread(VMCS_G_RFLAGS);
    d->cr0 = vmx_vmread(VMCS_G_CR0);
    d->cr3 = vmx_vmread(VMCS_G_CR3);
    d->cr4 = vmx_vmread(VMCS_G_CR4);
    d->efer = vmx_vmread(VMCS_G_EFER);
    d->cs_base = vmx_vmread(VMCS_G_CS_BASE);
    d->cs_sel = (uint32_t)vmx_vmread(VMCS_G_CS_SEL);
    d->cs_ar = (uint32_t)vmx_vmread(VMCS_G_CS_AR);
    d->intr_info = (uint32_t)vmx_vmread(VMCS_EXIT_INTR_INFO);
}

// ----------------------------------------------------------------------------
// То, что гипервизор эмулирует сам.
// ----------------------------------------------------------------------------
static uint64_t gpr_get(hv_vm_t* vm, int i) {
    vmx_regs_t* r = &vm->regs;
    switch (i) {
        case 0: return r->rax;  case 1: return r->rcx;  case 2: return r->rdx;  case 3: return r->rbx;
        case 4: return vmx_vmread(VMCS_G_RSP);
        case 5: return r->rbp;  case 6: return r->rsi;  case 7: return r->rdi;
        case 8: return r->r8;   case 9: return r->r9;   case 10: return r->r10; case 11: return r->r11;
        case 12: return r->r12; case 13: return r->r13; case 14: return r->r14; default: return r->r15;
    }
}

static void gpr_set(hv_vm_t* vm, int i, uint64_t v) {
    vmx_regs_t* r = &vm->regs;
    switch (i) {
        case 0: r->rax = v; break;  case 1: r->rcx = v; break;  case 2: r->rdx = v; break;  case 3: r->rbx = v; break;
        case 4: W(vm, VMCS_G_RSP, v); break;
        case 5: r->rbp = v; break;  case 6: r->rsi = v; break;  case 7: r->rdi = v; break;
        case 8: r->r8 = v; break;   case 9: r->r9 = v; break;   case 10: r->r10 = v; break; case 11: r->r11 = v; break;
        case 12: r->r12 = v; break; case 13: r->r13 = v; break; case 14: r->r14 = v; break; default: r->r15 = v; break;
    }
}

// Перешагнуть команду, вызвавшую выход. Вне длинного режима счётчик команд
// 16- или 32-битный и заворачивается на границе — как на настоящем процессоре.
static void skip_insn(hv_vm_t* vm) {
    uint64_t rip = vmx_vmread(VMCS_G_RIP) + vmx_vmread(VMCS_EXIT_INSN_LEN);
    if (!vm->lma) {
        uint32_t ar = (uint32_t)vmx_vmread(VMCS_G_CS_AR);
        rip &= (ar & (1u << 14)) ? 0xFFFFFFFFull : 0xFFFFull;
    }
    W(vm, VMCS_G_RIP, rip);
}

// CR0, каким его видит гость: наши биты — из «тени», остальные — настоящие.
static uint64_t cr0_view(hv_vm_t* vm) {
    (void)vm;                       // читается текущая VMCS ядра — она и есть эта ВМ
    const uint64_t mask = CR0_PE | CR0_PG | CR0_NE | CR0_NW | CR0_CD;
    return (vmx_vmread(VMCS_G_CR0) & ~mask) | (vmx_vmread(VMCS_CR0_SHADOW) & mask);
}

// Исключение гостю вместо исполнения команды (счётчик команд не двигаем).
static void inject_exc(hv_vm_t* vm, int vec, bool has_err) {
    uint32_t info = (1u << 31) | (3u << 8) | (uint32_t)vec;     // тип 3: аппаратное
    // В реальном режиме код ошибки в стек не кладётся — и VM entry с ним отклонят.
    if (has_err && (cr0_view(vm) & CR0_PE)) {
        info |= 1u << 11;
        W(vm, VMCS_ENTRY_EXC_ERR, 0);
    }
    W(vm, VMCS_ENTRY_INTR_INFO, info);
    vm->event_queued = true;
}

static void exit_cpuid(hv_vm_t* vm) {
    vmx_regs_t* r = &vm->regs;
    uint32_t leaf = (uint32_t)r->rax, sub = (uint32_t)r->rcx;
    uint32_t a = 0, b = 0, c = 0, d = 0;

    if (leaf >= 0x40000000u && leaf <= 0x400000FFu) {
        // Гостю мы представляемся как KVM — ради одного: часов kvmclock.
        // Свою частоту TSC Linux иначе меряет по таймеру 8254, читая порт в
        // цикле и требуя не меньше 1000 чтений за 10 мс. Под вложенным KVM
        // один выход стоит ~17 мкс — замер не сходится никогда, и ядро
        // остаётся без TSC-часов. По kvmclock гость узнаёт частоту без единого
        // выхода. Объявляем ТОЛЬКО часы: CLOCKSOURCE2 (бит 3), «часы
        // стабильны» (24) и «задержка outb 0x80 не нужна» (1) — прочих
        // паравиртуальных услуг KVM у нас нет.
        if (leaf == 0x40000000u) { a = 0x40000001u; b = 0x4B4D564Bu; c = 0x564B4D56u; d = 0x0000004Du; }  // "KVMKVMKVM"
        else if (leaf == 0x40000001u) { a = (1u << 1) | (1u << 3) | (1u << 24); }
    } else {
        cpuid(leaf, sub, &a, &b, &c, &d);
        // Правило листов: гость видит только то, про что мы решили явно.
        // Лист, отданный «как есть», однажды уже стоил падения гостя: KVM узла
        // показывает в 0x80000008 EBX защиты от Spectre в «стиле AMD» (IBPB,
        // IBRS), Linux поверил и на первой же смене задачи записал в MSR 0x49,
        // которого у нас нет, — #GP в ядре гостя и смерть его init.
        if (leaf > 0x80000008u && leaf <= 0x800000FFu) { a = b = c = d = 0; }
        if (leaf > 0xD && leaf < 0x40000000u) { a = b = c = d = 0; }
        switch (leaf) {
        case 0:
            if (a > 0xD) a = 0xD;                           // дальше листов нет
            break;
        case 5:  a = b = c = d = 0; break;                  // MONITOR/MWAIT скрыты
        case 0x80000000u:
            if (a > 0x80000008u) a = 0x80000008u;
            break;
        case 0x80000008u:
            b = 0;                                          // только размеры адресов (EAX)
            break;
        case 1:
            // Прячем то, чего гость не получит: VMX, SMX, MONITOR, PCID,
            // x2APIC, TSC-deadline, XSAVE/OSXSAVE/AVX/F16C/FMA (состояние FPU
            // мы переключаем через FXSAVE — только x87 и SSE).
            c &= ~((1u << 5) | (1u << 6) | (1u << 3) | (1u << 17) | (1u << 21) | (1u << 24) |
                   (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 12));
            // И управление питанием/отладочные хранилища: их MSR мы не ведём.
            c &= ~((1u << 2) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 14) | (1u << 15) | (1u << 18));
            c |= 1u << 31;                                  // «работаю под гипервизором»
            b = (b & 0x0000FFFFu) | (1u << 16);             // один логический процессор, APIC ID 0
            // [50.2] Локального APIC у гостя нет (бит 9): прерывания идут через
            // пару 8259, как на машине до APIC. Эмуляция APIC — это MMIO и
            // разбор команд, а для одного vCPU она ничего не даёт. Заодно
            // прячем MTRR (12), MCE/MCA (7, 14), термо (22, 29), DS (21).
            d &= ~((1u << 28) | (1u << 9) | (1u << 12) | (1u << 7) | (1u << 14) | (1u << 22) | (1u << 29) | (1u << 21));
            break;
        case 4:  a &= 0x00003FFFu; break;                   // одно ядро, кэши ни с кем не делим
        case 6:  a = b = c = d = 0; break;                  // термо/питание
        case 7:
            if (sub == 0) {
                // Оставляем только то, что не требует ни XSAVE, ни наших выходов.
                b &= (1u << 0) | (1u << 3) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 23);
                c = 0; d = 0;
            } else { a = b = c = d = 0; }
            break;
        case 0xA: case 0xB: case 0xD: case 0xF: case 0x10: case 0x12: case 0x14: case 0x1F:
            a = b = c = d = 0;
            break;
        case 0x80000001u:
            d &= ~(1u << 27);                               // RDTSCP выключен в управлении
            break;
        default: break;
        }
    }
    r->rax = a; r->rbx = b; r->rcx = c; r->rdx = d;
    skip_insn(vm);
}

static void load_pdptes(hv_vm_t* vm) {
    // 32-битный PAE: четыре указателя каталогов процессор держит в VMCS. При
    // обычной записи в CR0/CR3 он подгружает их сам, но запись в CR0 мы
    // перехватываем — значит, это наша работа.
    uint64_t* p = (uint64_t*)hv_gpa(vm, vmx_vmread(VMCS_G_CR3) & ~0x1Full, 32);
    if (!p) return;
    for (int i = 0; i < 4; i++) W(vm, VMCS_G_PDPTE0 + i * 2, p[i]);
}

static void set_cr0(hv_vm_t* vm, uint64_t val) {
    uint64_t old = cr0_view(vm);
    // Те же проверки, что делает процессор: иначе вход в гостя отклонят.
    if ((val >> 32) || ((val & CR0_PG) && !(val & CR0_PE)) || ((val & CR0_NW) && !(val & CR0_CD))) {
        inject_exc(vm, 13, true);
        return;
    }
    uint64_t efer = vmx_vmread(VMCS_G_EFER);
    if ((val & CR0_PG) && !(old & CR0_PG)) {
        uint64_t cr4 = vmx_vmread(VMCS_G_CR4);
        if (efer & EFER_LME) {
            if (!(cr4 & CR4_PAE)) { inject_exc(vm, 13, true); return; }
            efer |= EFER_LMA;                       // гость вошёл в длинный режим
            vm->entry_ctls |= ENTRY_IA32E_GUEST;
        } else if (cr4 & CR4_PAE) {
            load_pdptes(vm);
        }
    } else if (!(val & CR0_PG) && (old & CR0_PG) && (efer & EFER_LMA)) {
        efer &= ~EFER_LMA;
        vm->entry_ctls &= ~ENTRY_IA32E_GUEST;
    }
    vm->lma = (efer & EFER_LMA) != 0;
    W(vm, VMCS_G_EFER, efer);
    W(vm, VMCS_ENTRY_CTLS, vm->entry_ctls);
    // CD/NW в настоящий CR0 не пускаем: гость (BIOS, ядро при настройке MTRR)
    // выключил бы кэш процессору, на котором исполняется.
    W(vm, VMCS_G_CR0, (val & ~(CR0_CD | CR0_NW)) | CR0_NE | CR0_ET);
    W(vm, VMCS_CR0_SHADOW, val);
    skip_insn(vm);
}

// false — такого обращения мы не умеем.
static bool exit_cr(hv_vm_t* vm) {
    uint64_t q = vmx_vmread(VMCS_EXIT_QUAL);
    int cr = (int)(q & 15), type = (int)((q >> 4) & 3), reg = (int)((q >> 8) & 15);
    if (type == 0) {                                // запись в CR
        uint64_t val = gpr_get(vm, reg);
        if (cr == 0) { set_cr0(vm, val); return true; }
        if (cr == 4) {
            // Выход бывает, только когда гость ставит бит под маской, а там
            // одни запрещённые: настоящий процессор ответил бы #GP.
            inject_exc(vm, 13, true);
            return true;
        }
        if (cr == 8) { vm->cr8 = val & 15; skip_insn(vm); return true; }
    } else if (type == 1) {                         // чтение CR (CR0/CR4 выходов не дают)
        if (cr == 8) { gpr_set(vm, reg, vm->cr8); skip_insn(vm); return true; }
    } else if (type == 3) {                         // LMSW: младшие 4 бита, PE снять нельзя
        uint64_t cur = cr0_view(vm), src = (q >> 16) & 0xFFFF;
        set_cr0(vm, (cur & ~0xEull) | (src & 0xE) | ((cur | src) & 1));
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// [50.2] Часы kvmclock. Гость сообщает адрес структуры, мы заполняем её один
// раз: «в момент TSC = t0 время гостя было 0, один такт — столько-то
// наносекунд». Дальше гость считает время сам, командой RDTSC, без выходов.
// Обновлять структуру не нужно: vCPU привязан к ядру, а частота TSC постоянна
// (флаг «часы стабильны»).
// ----------------------------------------------------------------------------
static void kvmclock_scale(uint64_t tsc_hz, int8_t* pshift, uint32_t* pmul) {
    // Тот же расчёт, что в KVM (kvm_get_time_scale): ns = (такты << shift) * mul >> 32.
    uint64_t tps64 = tsc_hz, scaled64 = 1000000000ull;
    int shift = 0;
    while (tps64 > scaled64 * 2 || (tps64 >> 32)) { tps64 >>= 1; shift--; }
    uint32_t tps32 = (uint32_t)tps64;
    while (tps32 <= scaled64 || (scaled64 >> 32)) {
        if ((scaled64 >> 32) || (tps32 & 0x80000000u)) scaled64 >>= 1;
        else tps32 <<= 1;
        shift++;
    }
    *pshift = (int8_t)shift;
    *pmul = (uint32_t)((scaled64 << 32) / tps32);
}

static void kvmclock_fill(hv_vm_t* vm, uint64_t gpa) {
    uint8_t* p = (uint8_t*)hv_gpa(vm, gpa, 32);
    if (!p) return;
    int8_t shift; uint32_t mul;
    kvmclock_scale(g_tsc_per_ms * 1000, &shift, &mul);
    zero(p, 32);
    *(uint64_t*)(p + 8)  = vm->clock_tsc0;      // tsc_timestamp
    *(uint64_t*)(p + 16) = 0;                   // system_time, нс
    *(uint32_t*)(p + 24) = mul;                 // tsc_to_system_mul
    *(int8_t*)(p + 28)   = shift;               // tsc_shift
    p[29] = 1;                                  // флаг: часы стабильны
    // Версия — последней и чётная: нечётная значит «идёт обновление».
    *(volatile uint32_t*)p = 2;
}

static void kvm_wallclock_fill(hv_vm_t* vm, uint64_t gpa) {
    uint32_t* p = (uint32_t*)hv_gpa(vm, gpa, 12);
    if (!p) return;
    p[1] = (uint32_t)vm->wall_sec;              // время на стене, когда время гостя было 0
    p[2] = 0;
    *(volatile uint32_t*)p = 2;
}

static void exit_msr(hv_vm_t* vm, bool write) {
    vmx_regs_t* r = &vm->regs;
    uint32_t idx = (uint32_t)r->rcx;
    uint64_t val = ((r->rdx & 0xFFFFFFFFull) << 32) | (r->rax & 0xFFFFFFFFull);
    bool ok = true;

    if (write) {
        switch (idx) {
        case MSR_IA32_EFER: {
            // LMA гость не пишет: его ведём мы вместе с CR0.PG.
            uint64_t cur = vmx_vmread(VMCS_G_EFER);
            if (val & ~(EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE)) { ok = false; break; }
            W(vm, VMCS_G_EFER, (val & (EFER_SCE | EFER_LME | EFER_NXE)) | (cur & EFER_LMA));
            break;
        }
        case MSR_IA32_FS_BASE:      W(vm, VMCS_G_FS_BASE, val); break;
        case MSR_IA32_GS_BASE:      W(vm, VMCS_G_GS_BASE, val); break;
        case MSR_IA32_PAT:          W(vm, VMCS_G_PAT, val); break;
        case MSR_IA32_SYSENTER_CS:  W(vm, VMCS_G_SYSENTER_CS, val & 0xFFFF); break;
        case MSR_IA32_SYSENTER_ESP: W(vm, VMCS_G_SYSENTER_ESP, val); break;
        case MSR_IA32_SYSENTER_EIP: W(vm, VMCS_G_SYSENTER_EIP, val); break;
        // Эти MSR процессор гостя читает сам (SYSCALL, SWAPGS) — значение
        // должно лежать в настоящем регистре. На AP гостя ими больше никто
        // не пользуется: ядро делает системные вызовы только на BSP.
        case MSR_KERNEL_GS_BASE: case MSR_STAR: case MSR_LSTAR: case MSR_CSTAR: case MSR_SFMASK:
            wrmsr(idx, val);
            break;
        case MSR_KVM_SYSTEM_TIME:
            vm->kvm_systime = val;
            if (val & 1) kvmclock_fill(vm, val & ~1ull);
            break;
        case MSR_KVM_WALL_CLOCK:
            vm->kvm_wall = val;
            kvm_wallclock_fill(vm, val);
            break;
        case 0x8B:                  // подпись микрокода: ядро пишет 0 перед CPUID
        case 0x1A0:                 // MISC_ENABLE
        case 0x1D9:                 // DEBUGCTL
            break;
        default: ok = false; break;
        }
    } else {
        switch (idx) {
        case MSR_IA32_EFER:         val = vmx_vmread(VMCS_G_EFER); break;
        case MSR_IA32_FS_BASE:      val = vmx_vmread(VMCS_G_FS_BASE); break;
        case MSR_IA32_GS_BASE:      val = vmx_vmread(VMCS_G_GS_BASE); break;
        case MSR_IA32_PAT:          val = vmx_vmread(VMCS_G_PAT); break;
        case MSR_IA32_SYSENTER_CS:  val = vmx_vmread(VMCS_G_SYSENTER_CS); break;
        case MSR_IA32_SYSENTER_ESP: val = vmx_vmread(VMCS_G_SYSENTER_ESP); break;
        case MSR_IA32_SYSENTER_EIP: val = vmx_vmread(VMCS_G_SYSENTER_EIP); break;
        case MSR_KERNEL_GS_BASE: case MSR_STAR: case MSR_LSTAR: case MSR_CSTAR: case MSR_SFMASK:
            val = rdmsr(idx);
            break;
        case MSR_KVM_SYSTEM_TIME:   val = vm->kvm_systime; break;
        case MSR_KVM_WALL_CLOCK:    val = vm->kvm_wall; break;
        case 0x8B:                  val = 1ull << 32; break;    // «ревизия микрокода 1»
        case 0x1A0:                 val = 1; break;             // быстрые строковые операции
        case 0x1D9: case 0x17:      val = 0; break;
        case MSR_IA32_TSC: {
            uint32_t lo, hi;
            __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
            val = ((uint64_t)hi << 32) | lo;
            break;
        }
        default: ok = false; break;
        }
        if (ok) { r->rax = val & 0xFFFFFFFFull; r->rdx = val >> 32; }
    }

    if (!ok) {
        // Незнакомый MSR — как на процессоре, у которого его нет: #GP. Номер
        // запоминаем: при запуске настоящих ядер по нему видно, чего не хватает.
        vm->st.msr_unknown++;
        vm->st.last_unknown_msr = idx;
        // Первые восемь разных номеров — в список: по нему видно, чего гостю
        // не хватило, даже если он пережил отказ.
        int k = 0;
        while (k < vm->st.unknown_n && vm->st.unknown_list[k] != idx) k++;
        if (k == vm->st.unknown_n && k < 8) vm->st.unknown_list[vm->st.unknown_n++] = idx;
        inject_exc(vm, 13, true);
        return;
    }
    skip_insn(vm);
}

// Отдать выход модели устройств. Возвращает её ответ.
static int to_model(hv_vm_t* vm, hv_exit_t* ex) {
    vm->last = *ex;
    return vm->model.on_exit ? vm->model.on_exit(vm, ex, vm->ctx) : HV_STOP;
}

#define HV_SLEEP 2      // внутренний ответ handle_exit: гость уснул по HLT

static void trace(hv_vm_t* vm, uint32_t reason, uint32_t arg) {
    hv_trace_t* t = &vm->trace[vm->trace_head++ % HV_TRACE_N];
    t->reason = reason;
    t->arg = arg;
}

static int fail_exit(hv_vm_t* vm, uint32_t hw_reason, const char* why) {
    hv_exit_t ex;
    zero(&ex, sizeof(ex));
    ex.reason = HV_EXIT_FAIL;
    ex.hw_reason = hw_reason;
    ex.qual = vmx_vmread(VMCS_EXIT_QUAL);
    ex.insn_error = (uint32_t)vmx_vmread(VMCS_INSN_ERROR);
    vm->err = why;
    diag_capture(vm);
    to_model(vm, &ex);
    return HV_STOP;
}

static int handle_exit(hv_vm_t* vm, uint32_t reason) {
    hv_exit_t ex;
    if (reason & VMX_EXIT_ENTRY_FAIL)
        return fail_exit(vm, reason, "VM entry rejected: invalid guest state");

    switch (reason & 0xFFFF) {
    case VMX_EXIT_INTR_WINDOW:
        // Гость снова может принять прерывание — его доставит irq_service
        // перед ближайшим входом; там же окно и выключится.
        vm->st.windows++;
        return HV_RESUME;
    case VMX_EXIT_EXTINT:
        // Прерывание хоста: его уже принял обработчик из IDT (sti после выхода).
        vm->st.extint++;
        return HV_RESUME;
    case VMX_EXIT_PREEMPT_TIMER:
        vm->st.ptimer++;
        return HV_RESUME;
    case VMX_EXIT_CPUID:
        vm->st.cpuid++;
        exit_cpuid(vm);
        return HV_RESUME;
    case VMX_EXIT_CR_ACCESS:
        vm->st.cr++;
        if (!exit_cr(vm)) return fail_exit(vm, reason, "unsupported control-register access");
        return HV_RESUME;
    case VMX_EXIT_RDMSR:
    case VMX_EXIT_WRMSR:
        vm->st.msr++;
        trace(vm, reason, (uint32_t)vm->regs.rcx);
        exit_msr(vm, (reason & 0xFFFF) == VMX_EXIT_WRMSR);
        return HV_RESUME;
    case VMX_EXIT_IO: {
        vm->st.io++;
        uint64_t q = vmx_vmread(VMCS_EXIT_QUAL);
        if (q & (1u << 4)) return fail_exit(vm, reason, "string I/O (INS/OUTS) is not supported");
        zero(&ex, sizeof(ex));
        ex.reason = HV_EXIT_IO;
        ex.port = (uint16_t)(q >> 16);
        ex.size = (uint8_t)((q & 7) + 1);
        ex.in = (q >> 3) & 1;
        uint32_t mask = ex.size == 4 ? 0xFFFFFFFFu : ((1u << (ex.size * 8)) - 1);
        ex.value = ex.in ? mask : ((uint32_t)vm->regs.rax & mask);
        trace(vm, reason, ex.port);
        int act = to_model(vm, &ex);
        if (ex.in) {
            // IN в EAX обнуляет старшую половину RAX; в AL/AX — не трогает остальное.
            if (ex.size == 4) vm->regs.rax = ex.value;
            else vm->regs.rax = (vm->regs.rax & ~(uint64_t)mask) | (ex.value & mask);
        }
        skip_insn(vm);
        return act;
    }
    case VMX_EXIT_HLT: {
        vm->st.hlt++;
        skip_insn(vm);
        // `sti; hlt`: выход случился на HLT, пока действует «блокировка после
        // STI» на одну команду. Команду мы перешагнули — блокировка кончилась.
        uint64_t is = vmx_vmread(VMCS_G_INTR_STATE);
        if (is & 3) W(vm, VMCS_G_INTR_STATE, is & ~3ull);
        // [50.2] HLT с разрешёнными прерываниями — обычный простой гостя:
        // vCPU спит, пока модель не скажет, что прерывание есть.
        if (vm->model.irq_pending && (vmx_vmread(VMCS_G_RFLAGS) & 0x200)) {
            vm->st.sleeps++;
            return HV_SLEEP;
        }
        zero(&ex, sizeof(ex));
        ex.reason = HV_EXIT_HLT;
        diag_capture(vm);
        return to_model(vm, &ex);
    }
    case VMX_EXIT_VMCALL:
        vm->st.vmcall++;
        skip_insn(vm);
        zero(&ex, sizeof(ex));
        ex.reason = HV_EXIT_VMCALL;
        return to_model(vm, &ex);
    case VMX_EXIT_EPT_VIOLATION:
        vm->st.ept++;
        zero(&ex, sizeof(ex));
        ex.reason = HV_EXIT_EPT;
        ex.gpa = vmx_vmread(VMCS_GUEST_PHYS_ADDR);
        ex.qual = vmx_vmread(VMCS_EXIT_QUAL);
        diag_capture(vm);
        return to_model(vm, &ex);
    case VMX_EXIT_TRIPLE_FAULT:
        zero(&ex, sizeof(ex));
        ex.reason = HV_EXIT_SHUTDOWN;
        diag_capture(vm);
        to_model(vm, &ex);
        return HV_STOP;
    case VMX_EXIT_XSETBV:
    case VMX_EXIT_MWAIT:
    case VMX_EXIT_MONITOR:
    case VMX_EXIT_INVEPT:
    case VMX_EXIT_INVVPID:
        // Скрыто от гостя в CPUID — отвечаем как процессор без этой команды.
        vm->st.other++;
        inject_exc(vm, 6, false);
        return HV_RESUME;
    case VMX_EXIT_RDPMC:
        vm->st.other++;
        inject_exc(vm, 13, true);
        return HV_RESUME;
    case VMX_EXIT_EXC_NMI:
        // Исключения гостя мы не перехватываем, значит это NMI хоста.
        // Источников NMI у ядра нет; считаем и идём дальше.
        vm->st.other++;
        if (((vmx_vmread(VMCS_EXIT_INTR_INFO) >> 8) & 7) == 2) return HV_RESUME;
        return fail_exit(vm, reason, "unexpected exception exit");
    case VMX_EXIT_EPT_MISCONFIG:
        return fail_exit(vm, reason, "EPT misconfiguration");
    default:
        trace(vm, reason, 0);
        if ((reason & 0xFFFF) >= VMX_EXIT_VMX_FIRST && (reason & 0xFFFF) <= VMX_EXIT_VMX_LAST) {
            vm->st.other++;
            inject_exc(vm, 6, false);       // команды VMX: гостю VT-x не показываем
            return HV_RESUME;
        }
        return fail_exit(vm, reason, "unhandled VM exit");
    }
}

// ----------------------------------------------------------------------------
// vCPU — «жилец» ядра. Всё ниже исполняется на AP гостя.
// ----------------------------------------------------------------------------
static int vcpu_finish(hv_vm_t* vm) {
    // VMCLEAR: процессор сбрасывает своё кэшированное состояние VMCS в память
    // и забывает её — только после этого страницу можно освобождать.
    vmx_vmclear(vm->vmcs);
    vmx_flush_guest_tlb();
    if (vm->msrs_swapped) {
        // Возвращаем ядру его MSR (см. поля host_* в структуре).
        vm->msrs_swapped = false;
        wrmsr(MSR_KERNEL_GS_BASE, vm->host_kgs);
        wrmsr(MSR_STAR, vm->host_star);
        wrmsr(MSR_LSTAR, vm->host_lstar);
        wrmsr(MSR_CSTAR, vm->host_cstar);
        wrmsr(MSR_SFMASK, vm->host_sfmask);
    }
    __atomic_store_n(&g_wake_tsc, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&vm->state, HV_VCPU_STOPPED, __ATOMIC_RELEASE);
    return -1;
}

// [50.2] Доставка прерывания гостю перед входом. Если принять его сейчас
// нельзя — просим у процессора выход, как только станет можно.
static void irq_service(hv_vm_t* vm, uint64_t now) {
    if (!vm->model.irq_pending) return;
    bool window = false;
    if (vm->model.irq_pending(vm, vm->ctx, now)) {
        if (vm->event_queued) {
            window = true;                  // в этот вход уже уходит исключение
        } else {
            uint64_t rf = vmx_vmread(VMCS_G_RFLAGS), is = vmx_vmread(VMCS_G_INTR_STATE);
            if ((rf & 0x200) && !(is & 3)) {
                int vec = vm->model.irq_ack(vm, vm->ctx);
                if (vec >= 0) {
                    W(vm, VMCS_ENTRY_INTR_INFO, (1u << 31) | (uint32_t)vec);    // тип 0: внешнее
                    vm->event_queued = true;
                    vm->st.injected++;
                }
            } else {
                window = true;
            }
        }
    }
    if (window != vm->window_on) {
        vm->window_on = window;
        if (window) vm->proc_ctls |= PROC_INTR_WINDOW;
        else        vm->proc_ctls &= ~PROC_INTR_WINDOW;
        W(vm, VMCS_PROC_CTLS, vm->proc_ctls);
    }
}

static int vcpu_resident(void* arg) {
    hv_vm_t* vm = (hv_vm_t*)arg;

    if (vm->state == HV_VCPU_NEW) {
        bool ok = vmx_vmclear(vm->vmcs) && vmx_vmptrld(vm->vmcs);
        if (!ok) vm->err = "VMCLEAR/VMPTRLD failed";
        if (ok) {
            // ВМ могла получить те же страницы EPT, что были у прежней:
            // трансляции, оставшиеся в кэше процессора, чужие.
            vmx_flush_guest_tlb();
            ok = vmcs_setup(vm);
        }
        if (!ok) {
            vm->last.reason = HV_EXIT_FAIL;
            return vcpu_finish(vm);
        }
        vm->state = HV_VCPU_RUNNABLE;
    }
    if (vm->stop_req) return vcpu_finish(vm);
    if (vm->state == HV_VCPU_HALTED) {
        // Гость спит по HLT. Будить — когда у модели появится прерывание:
        // либо она сама «пнёт» ядро (пришёл символ), либо подойдёт её таймер —
        // тогда ядро разбудит таймер BSP (hv_tick_1ms).
        uint64_t now = rdtsc();
        if (!vm->model.irq_pending(vm, vm->ctx, now)) {
            uint64_t d = vm->model.deadline ? vm->model.deadline(vm, vm->ctx) : 0;
            if (!vm->sleep_t0) {
                vm->sleep_t0 = now;
                if (!d) vm->st.sleeps_open++;   // будить будет только «пинок» модели
            }
            g_wake_slot = vm->slot;
            __atomic_store_n(&g_wake_tsc, d, __ATOMIC_RELEASE);
            return 0;
        }
        __atomic_store_n(&g_wake_tsc, 0, __ATOMIC_RELEASE);
        if (vm->sleep_t0) {
            if (now - vm->sleep_t0 > vm->st.sleep_max_tsc) vm->st.sleep_max_tsc = now - vm->sleep_t0;
            vm->sleep_t0 = 0;
        }
        vm->state = HV_VCPU_RUNNABLE;
    }
    if (!smp_pool_guest_enter(vm->slot)) return 2;      // пулу ядро нужно сейчас: уступаем

    vm->st.slices++;
    const uint64_t slice = g_tsc_per_ms * HV_SLICE_MS;
    const uint64_t t0 = rdtsc();
    // Таймер вытеснения считает такты TSC, сдвинутые на MISC[4:0].
    const int pt_shift = (int)(vmx_caps()->misc & 31);
    W(vm, VMCS_PREEMPT_TIMER, slice >> pt_shift);

    int verdict = 1;
    for (;;) {
        cli();
        // Проверка — при IF=0: IPI «остановись», пришедший позже, останется
        // ждать и выбьет ядро из гостя сразу после входа.
        if (vm->stop_req) { sti(); break; }

        if (vm->model.irq_pending) {
            uint64_t now = rdtsc();
            irq_service(vm, now);
            // Гость должен выйти не позже конца кванта И не позже ближайшего
            // события модели (тик таймера): иначе прерывание опоздает на квант.
            uint64_t left = (now - t0 < slice) ? slice - (now - t0) : 0;
            uint64_t d = vm->model.deadline ? vm->model.deadline(vm, vm->ctx) : 0;
            if (d) {
                uint64_t dd = d > now ? d - now : 0;
                if (dd < left) left = dd;
            }
            W(vm, VMCS_PREEMPT_TIMER, left >> pt_shift);
        }

        __asm__ __volatile__("fxsave64 %0"  : "=m"(*(uint8_t(*)[512])vm->fx_host));
        __asm__ __volatile__("fxrstor64 %0" : : "m"(*(uint8_t(*)[512])vm->fx_guest));
        uint64_t e0 = rdtsc();
        int rc = vmx_enter(&vm->regs, vm->launched);
        vm->st.guest_tsc += rdtsc() - e0;
        __asm__ __volatile__("fxsave64 %0"  : "=m"(*(uint8_t(*)[512])vm->fx_guest));
        __asm__ __volatile__("fxrstor64 %0" : : "m"(*(uint8_t(*)[512])vm->fx_host));
        // После выхода процессор ставит пределы GDT и IDT в 0xFFFF — возвращаем свои.
        __asm__ __volatile__("lgdt %0" : : "m"(vm->host_gdtr));
        __asm__ __volatile__("lidt %0" : : "m"(vm->host_idtr));

        if (rc) {
            sti();
            fail_exit(vm, 0xFFFF0000u | (uint32_t)rc,
                      rc == 1 ? "VM entry failed: no current VMCS" : "VM entry failed: invalid control or host state");
            verdict = -1;
            break;
        }
        vm->launched = 1;
        uint32_t reason = (uint32_t)vmx_vmread(VMCS_EXIT_REASON);
        if (vm->event_queued) {
            // Событие могло не дойти: выход случился посреди его доставки.
            // Процессор сообщает об этом в поле IDT-vectoring — кладём то же
            // событие в следующий вход, иначе прерывание пропадёт навсегда
            // (контроллер уже считает его принятым).
            uint32_t iv = (uint32_t)vmx_vmread(VMCS_IDT_VEC_INFO);
            if (iv & (1u << 31)) {
                W(vm, VMCS_ENTRY_INTR_INFO, iv & ~(1u << 12));
                if (iv & (1u << 11)) W(vm, VMCS_ENTRY_EXC_ERR, vmx_vmread(0x440A));
            } else {
                vm->event_queued = false;
            }
        }
        sti();                              // здесь доходят прерывания хоста, прервавшие гостя
        vm->st.exits++;

        int act = handle_exit(vm, reason);
        if (act == HV_STOP) { verdict = -1; break; }
        if (act == HV_SLEEP) { verdict = 0; break; }
        if (rdtsc() - t0 >= slice) break;
        if (smp_pool_pending(vm->slot)) break;
    }
    smp_pool_guest_leave(vm->slot);
    if (verdict < 0) return vcpu_finish(vm);
    if (verdict == 0) {
        // Уснул. Возвращаем 2 («готов, но квант не засчитывать»): на
        // следующем обороте ветка HALTED сама решит, будить или спать.
        vm->state = HV_VCPU_HALTED;
        return 2;
    }
    return 1;
}

// ----------------------------------------------------------------------------
// Управление vCPU с BSP.
// ----------------------------------------------------------------------------
bool hv_vcpu_start_real(hv_vm_t* vm, uint16_t cs, uint16_t ip, hv_exit_fn fn, void* ctx) {
    hv_start_t st;
    hv_model_t m;
    zero(&st, sizeof(st));
    zero(&m, sizeof(m));
    st.mode = HV_START_REAL;
    st.cs = cs;
    st.ip = ip;
    m.on_exit = fn;
    return hv_vcpu_start(vm, &st, &m, ctx);
}

bool hv_vcpu_start(hv_vm_t* vm, const hv_start_t* st, const hv_model_t* model, void* ctx) {
    if (smp_pool_resident_active(vm->slot)) { g_err = "the guest core already runs a vCPU"; return false; }
    vm->start = *st;
    vm->model = *model;
    vm->ctx = ctx;
    vm->trace_head = 0;
    vm->sleep_t0 = 0;
    vm->kvm_systime = 0;
    vm->kvm_wall = 0;
    vm->stop_req = 0;
    vm->launched = 0;
    vm->wr_fail = 0;
    vm->err = 0;
    zero(&vm->st, sizeof(vm->st));
    zero(&vm->last, sizeof(vm->last));
    zero(&vm->diag, sizeof(vm->diag));
    // FPU гостя как после FNINIT: слово управления x87 0x037F, MXCSR 0x1F80.
    zero(vm->fx_guest, sizeof(vm->fx_guest));
    vm->fx_guest[0] = 0x7F; vm->fx_guest[1] = 0x03;
    vm->fx_guest[24] = 0x80; vm->fx_guest[25] = 0x1F;
    __atomic_store_n(&vm->state, HV_VCPU_NEW, __ATOMIC_RELEASE);
    if (!smp_pool_set_resident(vm->slot, vcpu_resident, vm)) {
        vm->state = HV_VCPU_STOPPED;
        g_err = "could not hand the vCPU to the guest core";
        return false;
    }
    vm->attached = true;
    return true;
}

bool hv_vcpu_wait(hv_vm_t* vm, uint32_t timeout_ms) {
    if (!vm->attached) return true;
    uint64_t end = timer_get_uptime_ms() + timeout_ms;
    // Ждём не только «гость остановлен», но и «ядро сняло жильца»: между
    // этими двумя событиями ядро ещё держит указатель на ВМ.
    while (vm->state != HV_VCPU_STOPPED || smp_pool_resident_active(vm->slot)) {
        if (timer_get_uptime_ms() > end) return false;
        relax();
    }
    vm->attached = false;
    return true;
}

bool hv_vcpu_stop(hv_vm_t* vm) {
    if (!vm->attached) return true;
    __atomic_store_n(&vm->stop_req, 1, __ATOMIC_SEQ_CST);
    smp_pool_kick(vm->slot);
    return hv_vcpu_wait(vm, 3000);
}
