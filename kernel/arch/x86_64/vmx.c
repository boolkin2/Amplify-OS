// ============================================================================
// vmx.c — Intel VT-x (VMX): разведка возможностей (50.0), VMXON/VMXOFF (50.1).
// ----------------------------------------------------------------------------
// Что здесь есть и почему именно так — в шапке vmx.h. Правила чтения MSR:
//   * любой MSR IA32_VMX_* на процессоре без VMX даёт #GP, а безопасного
//     rdmsr у ядра нет — поэтому сначала CPUID.1:ECX[5], и только потом MSR;
//   * часть MSR существует лишь при определённых битах в уже прочитанных:
//     PROCBASED_CTLS2 — если разрешён бит 31 «вторичные управления»,
//     EPT_VPID_CAP — если разрешены EPT или VPID, TRUE_* — если BASIC[55],
//     VMFUNC — если разрешён бит 13 вторичных. Порядок чтения ниже следует
//     этим зависимостям, иначе первая же загрузка на «бедном» процессоре
//     кончится #GP до экрана входа.
// ============================================================================
#include "vmx.h"
#include "memory.h"
#include "cpu.h"
#include "cpu_stats.h"
#include "serial.h"
#include "smp_pool.h"
#include "vmx_defs.h"

#define MSR_FEATURE_CONTROL   0x03A
#define MSR_VMX_BASIC         0x480
#define MSR_VMX_PIN           0x481
#define MSR_VMX_PROC          0x482
#define MSR_VMX_EXIT          0x483
#define MSR_VMX_ENTRY         0x484
#define MSR_VMX_MISC          0x485
#define MSR_VMX_CR0_FIXED0    0x486
#define MSR_VMX_CR0_FIXED1    0x487
#define MSR_VMX_CR4_FIXED0    0x488
#define MSR_VMX_CR4_FIXED1    0x489
#define MSR_VMX_VMCS_ENUM     0x48A
#define MSR_VMX_PROC2         0x48B
#define MSR_VMX_EPT_VPID      0x48C
#define MSR_VMX_TRUE_PIN      0x48D
#define MSR_VMX_TRUE_PROC     0x48E
#define MSR_VMX_TRUE_EXIT     0x48F
#define MSR_VMX_TRUE_ENTRY    0x490
#define MSR_VMX_VMFUNC        0x491

#define FC_LOCK               (1ull << 0)
#define FC_VMXON_OUTSIDE_SMX  (1ull << 2)

static vmx_caps_t g_caps;

// Состояние каждого ядра. Пишет само ядро при старте, читает BSP позже —
// блокировка не нужна: запись заканчивается задолго до первого чтения.
typedef struct {
    volatile uint8_t seen;
    uint8_t          present;
    uint64_t         feature_control;
} vmx_cpu_t;
static vmx_cpu_t g_cpu[MAX_CPUS];

// ----------------------------------------------------------------------------
// Таблица распознаваемых битов.
// Одна таблица служит и печати, и вердикту «готов/не готов», и сверке в тесте
// (тест сравнивает эти имена со списком возможностей, который QEMU сообщает
// по QMP). Имена короткие и без общих хвостов: тест ищет « имя=».
// ----------------------------------------------------------------------------
enum { SRC_PIN, SRC_PROC, SRC_PROC2, SRC_EXIT, SRC_ENTRY, SRC_EPT, SRC_MISC, SRC_BASIC };
// NEED_MUST — без этого выбранная схема этапа 50 не работает вовсе;
// NEED_WANT — есть обходной путь, но медленнее или сложнее.
enum { NEED_NO = 0, NEED_MUST, NEED_WANT };

typedef struct {
    const char* name;
    uint8_t     src;
    uint8_t     bit;
    uint8_t     need;
} vmx_capdef_t;

static const vmx_capdef_t g_defs[] = {
    // --- pin-based ---
    { "extint",    SRC_PIN,    0, NEED_MUST },  // прерывания хоста выводят из гостя
    { "nmiexit",   SRC_PIN,    3, NEED_NO   },
    { "vnmi",      SRC_PIN,    5, NEED_NO   },
    { "ptimer",    SRC_PIN,    6, NEED_WANT },  // таймер вытеснения: квант гостя
    { "pi",        SRC_PIN,    7, NEED_NO   },
    // --- первичные процессорные ---
    { "intwin",    SRC_PROC,   2, NEED_MUST },  // «окно прерываний»: инъекция IRQ
    { "tscoff",    SRC_PROC,   3, NEED_NO   },
    { "hltexit",   SRC_PROC,   7, NEED_MUST },  // HLT гостя освобождает ядро
    { "ioexit",    SRC_PROC,  24, NEED_MUST },  // порты — основа модели устройств
    { "iobmp",     SRC_PROC,  25, NEED_WANT },
    { "mtf",       SRC_PROC,  27, NEED_NO   },
    { "msrbmp",    SRC_PROC,  28, NEED_WANT },
    { "sec",       SRC_PROC,  31, NEED_MUST },
    // --- вторичные процессорные ---
    { "ept",       SRC_PROC2,  1, NEED_MUST },
    { "desc",      SRC_PROC2,  2, NEED_NO   },
    { "rdtscp",    SRC_PROC2,  3, NEED_NO   },
    { "vpid",      SRC_PROC2,  5, NEED_WANT },
    // Unrestricted guest: без него гость не может исполняться в реальном
    // режиме, а загрузка через BIOS (вариант B) начинается именно там.
    { "ug",        SRC_PROC2,  7, NEED_MUST },
    { "apicv",     SRC_PROC2,  9, NEED_NO   },
    { "ple",       SRC_PROC2, 10, NEED_NO   },
    { "invpcid",   SRC_PROC2, 12, NEED_NO   },
    { "vmfunc",    SRC_PROC2, 13, NEED_NO   },
    { "shadow",    SRC_PROC2, 14, NEED_NO   },
    { "pml",       SRC_PROC2, 17, NEED_NO   },
    { "xsaves",    SRC_PROC2, 20, NEED_NO   },
    { "tscscale",  SRC_PROC2, 25, NEED_NO   },
    // --- выход ---
    { "host64",    SRC_EXIT,   9, NEED_MUST },  // хост 64-битный
    { "ackintr",   SRC_EXIT,  15, NEED_NO   },
    // Гость сам ходит между реальным, защищённым и длинным режимом, значит
    // у него свой EFER: его надо сохранять и подменять на входе и выходе.
    { "xsvefer",   SRC_EXIT,  20, NEED_MUST },
    { "xldefer",   SRC_EXIT,  21, NEED_MUST },
    { "xsvptimer", SRC_EXIT,  22, NEED_NO   },
    // --- вход ---
    { "ia32e",     SRC_ENTRY,  9, NEED_MUST },
    { "eldefer",   SRC_ENTRY, 15, NEED_MUST },
    // --- EPT/VPID ---
    { "eptx",      SRC_EPT,    0, NEED_NO   },
    { "walk4",     SRC_EPT,    6, NEED_MUST },
    { "eptwb",     SRC_EPT,   14, NEED_MUST },
    { "ept2m",     SRC_EPT,   16, NEED_WANT },  // без 2-МБ страниц таблицы EPT толще
    { "ept1g",     SRC_EPT,   17, NEED_NO   },
    { "invept",    SRC_EPT,   20, NEED_MUST },
    { "eptad",     SRC_EPT,   21, NEED_NO   },
    { "invvpid",   SRC_EPT,   32, NEED_NO   },
    // --- прочее ---
    { "acthlt",    SRC_MISC,   6, NEED_NO   },
    { "insouts",   SRC_BASIC, 54, NEED_NO   },
    { "truectl",   SRC_BASIC, 55, NEED_NO   },
};
#define NDEFS ((int)(sizeof(g_defs) / sizeof(g_defs[0])))

static bool cap_value(const vmx_capdef_t* d) {
    uint64_t m;
    switch (d->src) {
        case SRC_PIN:   m = g_caps.pin;   break;
        case SRC_PROC:  m = g_caps.proc;  break;
        case SRC_PROC2: m = g_caps.proc2; break;
        case SRC_EXIT:  m = g_caps.exit;  break;
        case SRC_ENTRY: m = g_caps.entry; break;
        // Эти три — обычные битовые поля, без деления на «обязан/разрешено».
        case SRC_EPT:   return (g_caps.ept_vpid >> d->bit) & 1;
        case SRC_MISC:  return (g_caps.misc     >> d->bit) & 1;
        default:        return (g_caps.basic    >> d->bit) & 1;
    }
    // Управляющий MSR: «можно ли поставить 1» — старшая половина.
    return (m >> (32 + d->bit)) & 1;
}

// ----------------------------------------------------------------------------
// Разведка.
// ----------------------------------------------------------------------------
static bool cpu_has_vmx(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (c >> 5) & 1;
}

// FEATURE_CONTROL годится, если VMXON уже разрешён и заперт — либо регистр
// ещё не заперт: тогда нужные биты поставим сами перед VMXON (подэтап 50.1).
// Не годится одно: заперт прошивкой с выключенным VMX.
static bool fc_usable(uint64_t fc) {
    if (!(fc & FC_LOCK)) return true;
    return (fc & FC_VMXON_OUTSIDE_SMX) != 0;
}

void vmx_probe_this_cpu(int slot) {
    if (slot < 0 || slot >= MAX_CPUS) return;
    vmx_cpu_t* c = &g_cpu[slot];
    c->present = cpu_has_vmx() ? 1 : 0;
    c->feature_control = c->present ? rdmsr(MSR_FEATURE_CONTROL) : 0;
    c->seen = 1;
}

// «Обязан быть 1» не может требовать того, что «не разрешено быть 1».
static bool ctl_consistent(uint64_t m) {
    uint32_t must = (uint32_t)m, may = (uint32_t)(m >> 32);
    return (must & ~may) == 0;
}

void vmx_probe(void) {
    vmx_caps_t* c = &g_caps;
    vmx_probe_this_cpu(0);

    c->present = g_cpu[0].present;
    c->sane = true;
    if (c->present) {
        c->feature_control = g_cpu[0].feature_control;
        c->fc_locked = (c->feature_control & FC_LOCK) != 0;
        c->fc_vmxon  = (c->feature_control & FC_VMXON_OUTSIDE_SMX) != 0;

        c->basic       = rdmsr(MSR_VMX_BASIC);
        c->revision    = (uint32_t)(c->basic & 0x7FFFFFFFu);
        c->region_size = (uint32_t)((c->basic >> 32) & 0x1FFF);
        c->true_ctls   = (c->basic >> 55) & 1;

        uint64_t pin = rdmsr(MSR_VMX_PIN),  proc  = rdmsr(MSR_VMX_PROC);
        uint64_t ext = rdmsr(MSR_VMX_EXIT), entry = rdmsr(MSR_VMX_ENTRY);
        c->pin = pin; c->proc = proc; c->exit = ext; c->entry = entry;
        if (c->true_ctls) {
            // TRUE_* честнее: в обычных MSR часть битов «обязана быть 1»
            // только по историческим причинам (например, выход на запись CR3).
            c->pin   = rdmsr(MSR_VMX_TRUE_PIN);
            c->proc  = rdmsr(MSR_VMX_TRUE_PROC);
            c->exit  = rdmsr(MSR_VMX_TRUE_EXIT);
            c->entry = rdmsr(MSR_VMX_TRUE_ENTRY);
            // TRUE может только ОСЛАБИТЬ обязательные биты, но не добавить.
            if (((uint32_t)c->pin  & ~(uint32_t)pin)  || ((uint32_t)c->proc  & ~(uint32_t)proc) ||
                ((uint32_t)c->exit & ~(uint32_t)ext)  || ((uint32_t)c->entry & ~(uint32_t)entry))
                c->sane = false;
        }
        if ((c->proc >> (32 + 31)) & 1) c->proc2 = rdmsr(MSR_VMX_PROC2);
        bool ept_ok  = (c->proc2 >> (32 + 1)) & 1;
        bool vpid_ok = (c->proc2 >> (32 + 5)) & 1;
        if (ept_ok || vpid_ok)           c->ept_vpid = rdmsr(MSR_VMX_EPT_VPID);
        if ((c->proc2 >> (32 + 13)) & 1) c->vmfunc   = rdmsr(MSR_VMX_VMFUNC);

        c->misc       = rdmsr(MSR_VMX_MISC);
        c->vmcs_enum  = rdmsr(MSR_VMX_VMCS_ENUM);
        c->cr0_fixed0 = rdmsr(MSR_VMX_CR0_FIXED0);
        c->cr0_fixed1 = rdmsr(MSR_VMX_CR0_FIXED1);
        c->cr4_fixed0 = rdmsr(MSR_VMX_CR4_FIXED0);
        c->cr4_fixed1 = rdmsr(MSR_VMX_CR4_FIXED1);

        // Самопроверка: если хоть одно из этого неверно, мы читаем не те MSR
        // или не так их понимаем — дальше строить на этом нельзя.
        if (!ctl_consistent(c->pin) || !ctl_consistent(c->proc) || !ctl_consistent(c->proc2) ||
            !ctl_consistent(c->exit) || !ctl_consistent(c->entry))   c->sane = false;
        if (c->region_size == 0 || c->region_size > 4096)             c->sane = false;
        if (c->cr0_fixed0 & ~c->cr0_fixed1)                           c->sane = false;
        if (c->cr4_fixed0 & ~c->cr4_fixed1)                           c->sane = false;
        if (!(c->cr4_fixed0 & (1ull << 13)))                          c->sane = false;  // VMXE
        if (!(c->cr0_fixed0 & (1ull << 5)))                           c->sane = false;  // NE
    }

    c->cpus_seen = 0; c->cpus_ok = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!g_cpu[i].seen) continue;
        c->cpus_seen++;
        if (g_cpu[i].present && fc_usable(g_cpu[i].feature_control)) c->cpus_ok++;
    }

    bool ready = c->present && c->sane && fc_usable(c->feature_control);
    for (int i = 0; ready && i < NDEFS; i++)
        if (g_defs[i].need == NEED_MUST && !cap_value(&g_defs[i])) ready = false;
    // Гость исполняется на AP: ядро без VMX или с запертым FEATURE_CONTROL
    // означало бы отказ уже после выбора ядра, поэтому требуем все сразу.
    if (c->cpus_ok != c->cpus_seen) ready = false;
    c->ready  = ready;
    c->probed = true;
}

const vmx_caps_t* vmx_caps(void) { return &g_caps; }
bool vmx_ready(void)             { return g_caps.probed && g_caps.ready; }

// ----------------------------------------------------------------------------
// Печать.
// ----------------------------------------------------------------------------
static void put_hex(vmx_sink_t out, uint64_t v, int digits) {
    char b[20]; int n = 0;
    b[n++] = '0'; b[n++] = 'x';
    for (int i = digits - 1; i >= 0; i--) b[n++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
    b[n] = 0;
    out(b);
}

static void put_dec(vmx_sink_t out, uint64_t v) {
    char b[24]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char r[24]; int k = 0;
    while (n) r[k++] = b[--n];
    r[k] = 0;
    out(r);
}

static void put_msr(vmx_sink_t out, const char* name, uint64_t v) {
    out(" "); out(name); out("="); put_hex(out, v, 16);
}

static const char* fc_text(uint64_t fc) {
    if (!(fc & FC_LOCK))           return "unlocked: phantom sets it before VMXON";
    if (fc & FC_VMXON_OUTSIDE_SMX) return "locked, VMXON allowed";
    return "locked OFF by firmware";
}

// Список недостающего: need = NEED_MUST или NEED_WANT. Возвращает число.
static int put_missing(vmx_sink_t out, int need) {
    int n = 0;
    for (int i = 0; i < NDEFS; i++) {
        if (g_defs[i].need != need || cap_value(&g_defs[i])) continue;
        out(n ? "," : ""); out(g_defs[i].name); n++;
    }
    if (!n) out("none");
    return n;
}

void vmx_describe(vmx_sink_t out, const char* prefix, bool full) {
    const vmx_caps_t* c = &g_caps;
    const cpu_info_t* ci = cpu_info();

    out(prefix); out("cpu: "); out(ci->vendor);
    out(", CPUID.1:ECX.VMX="); out(c->present ? "1" : "0");
    if (ci->hypervisor) { out(", hypervisor="); out(ci->hv_vendor); }
    out("\n");

    if (!c->present) {
        out(prefix); out("verdict: ABSENT (no VT-x on this CPU, phantom is off)\n");
        return;
    }

    out(prefix); out("feature_control="); put_hex(out, c->feature_control, 16);
    out(" ("); out(fc_text(c->feature_control)); out(")\n");

    out(prefix); out("vmcs revision="); put_hex(out, c->revision, 8);
    out(" region="); put_dec(out, c->region_size);
    out(" true_ctls="); out(c->true_ctls ? "1" : "0"); out("\n");

    if (full) {
        out(prefix); out("msr");
        put_msr(out, "basic", c->basic); put_msr(out, "pin", c->pin);
        put_msr(out, "proc", c->proc);   out("\n");
        out(prefix); out("msr");
        put_msr(out, "proc2", c->proc2); put_msr(out, "exit", c->exit);
        put_msr(out, "entry", c->entry); out("\n");
        out(prefix); out("msr");
        put_msr(out, "ept_vpid", c->ept_vpid); put_msr(out, "misc", c->misc);
        put_msr(out, "vmcs_enum", c->vmcs_enum); out("\n");
        out(prefix); out("msr");
        put_msr(out, "cr0_fixed0", c->cr0_fixed0); put_msr(out, "cr0_fixed1", c->cr0_fixed1);
        out("\n");
        out(prefix); out("msr");
        put_msr(out, "cr4_fixed0", c->cr4_fixed0); put_msr(out, "cr4_fixed1", c->cr4_fixed1);
        put_msr(out, "vmfunc", c->vmfunc); out("\n");
    }

    // Биты — по восемь в строке: и в окне терминала помещается, и grep'ом
    // по serial собирается (каждая строка начинается со слова caps).
    int col = 0;
    for (int i = 0; i < NDEFS; i++) {
        if (!full && g_defs[i].need == NEED_NO) continue;
        if (col == 0) { out(prefix); out("caps"); }
        out(" "); out(g_defs[i].name); out(cap_value(&g_defs[i]) ? "=1" : "=0");
        if (++col == 8) { out("\n"); col = 0; }
    }
    if (col) out("\n");

    out(prefix); out("cpus="); put_dec(out, (uint64_t)c->cpus_ok);
    out("/"); put_dec(out, (uint64_t)c->cpus_seen);
    out(" (cores with VT-x and usable feature control / cores online)\n");

    out(prefix);
    if (c->ready) {
        out("verdict: READY (missing optional: ");
        put_missing(out, NEED_WANT);
        out(")\n");
    } else {
        out("verdict: NOT USABLE (");
        if (!c->sane)                               out("capability MSRs are inconsistent; ");
        if (!fc_usable(c->feature_control))         out("VT-x is locked off by firmware; ");
        if (c->cpus_ok != c->cpus_seen)             out("not every core has VT-x; ");
        out("missing required: ");
        put_missing(out, NEED_MUST);
        out(")\n");
    }
}

void vmx_log_status(void) {
    const vmx_caps_t* c = &g_caps;
    serial_puts("[phantom] status vmx=");
    serial_puts(c->present ? "1" : "0");
    serial_puts(" ready=");
    serial_puts(c->ready ? "1" : "0");
    serial_puts(" cpus=");
    serial_print_dec((uint64_t)c->cpus_ok);
    serial_puts("/");
    serial_print_dec((uint64_t)c->cpus_seen);
    serial_puts("\n");
}

// ----------------------------------------------------------------------------
// [50.1] VMXON / VMXOFF.
// ----------------------------------------------------------------------------
// Область VMXON у каждого ядра своя (процессор хранит в ней собственные
// данные). Адрес нужен ФИЗИЧЕСКИЙ; ядро отображено один-в-один, поэтому адрес
// статического массива и есть физический.
static uint8_t g_vmxon_region[MAX_CPUS][4096] __attribute__((aligned(4096)));
static volatile uint32_t g_on_mask;        // ядра в режиме VMX root
static int g_users;                        // сколько ВМ держат VMX включённым

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}
static inline void write_cr4(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(v));
}

typedef struct { int slot; } vmx_onoff_t;

// Исполняется НА том ядре, которое включаем.
static void cpu_on(void* arg) {
    int slot = ((vmx_onoff_t*)arg)->slot;
    if (!cpu_has_vmx()) return;
    // Ядро могло остаться в VMX с прошлого раза (дирижёр был занят в момент
    // выключения): повторный VMXON там — ошибка, а не «уже включено».
    if (g_on_mask & (1u << slot)) return;

    uint64_t fc = rdmsr(MSR_FEATURE_CONTROL);
    if (!(fc & FC_LOCK)) {
        // Прошивка регистр не заперла — запираем сами с разрешённым VMXON:
        // пока замок открыт, VMXON даёт #GP.
        wrmsr(MSR_FEATURE_CONTROL, fc | FC_LOCK | FC_VMXON_OUTSIDE_SMX);
    } else if (!(fc & FC_VMXON_OUTSIDE_SMX)) {
        return;
    }

    write_cr4(read_cr4() | CR4_VMXE);
    uint32_t* r = (uint32_t*)g_vmxon_region[slot];
    for (int i = 0; i < 1024; i++) r[i] = 0;
    r[0] = g_caps.revision;
    uint64_t phys = virt_to_phys(r);
    uint8_t bad;
    __asm__ __volatile__("vmxon %1; setna %0" : "=qm"(bad) : "m"(phys) : "cc", "memory");
    if (bad) { write_cr4(read_cr4() & ~CR4_VMXE); return; }
    __atomic_fetch_or(&g_on_mask, 1u << slot, __ATOMIC_SEQ_CST);
}

static void cpu_off(void* arg) {
    int slot = ((vmx_onoff_t*)arg)->slot;
    if (!(g_on_mask & (1u << slot))) return;
    __asm__ __volatile__("vmxoff" ::: "cc");
    write_cr4(read_cr4() & ~CR4_VMXE);
    __atomic_fetch_and(&g_on_mask, ~(1u << slot), __ATOMIC_SEQ_CST);
}

uint32_t vmx_enable_all(void) {
    if (!vmx_ready()) return 0;
    g_users++;
    // Проходим по ядрам при каждом новом пользователе, а не только при
    // первом: ядро, пропущенное из-за занятого дирижёра, включится позже.
    for (int s = 0; s < MAX_CPUS; s++) {
        if (g_on_mask & (1u << s)) continue;
        vmx_onoff_t a = { s };
        smp_pool_call(s, cpu_on, &a);      // false — ядра нет или дирижёр занят
    }
    return g_on_mask;
}

void vmx_disable_all(void) {
    if (g_users <= 0) return;
    if (--g_users > 0) return;
    for (int s = 0; s < MAX_CPUS; s++) {
        if (!(g_on_mask & (1u << s))) continue;
        vmx_onoff_t a = { s };
        // Отказ (дирижёр занят) не беда: ядро останется в VMX root до
        // следующего включения/выключения, работать ему это не мешает.
        smp_pool_call(s, cpu_off, &a);
    }
}

uint32_t vmx_on_mask(void) { return g_on_mask; }

void vmx_flush_guest_tlb(void) {
    // Дескриптор: для «всех контекстов» его содержимое не читается, но
    // операнд-память обязателен.
    struct { uint64_t a, b; } d = { 0, 0 };
    uint64_t all = 2;
    if ((g_caps.ept_vpid >> 26) & 1)
        __asm__ __volatile__("invept %0, %1" : : "m"(d), "r"(all) : "cc", "memory");
    if (((g_caps.ept_vpid >> 32) & 1) && ((g_caps.ept_vpid >> 42) & 1))
        __asm__ __volatile__("invvpid %0, %1" : : "m"(d), "r"(all) : "cc", "memory");
}
