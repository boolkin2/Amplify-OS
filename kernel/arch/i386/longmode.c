// ============================================================================
// longmode.c — Этап 33A: подготовка и демонстрация x86_64 Long Mode.
// ============================================================================

#include "longmode.h"
#include "../../debug_serial.h"
#include <stddef.h>

// Сигнатура, которую 64-битный код пишет в out_signature.
// 8 байт = 'A','M','P','L','I','F','6','4' (little-endian).
#define LM_SIGN_VALUE   ((0x34364649ULL << 32) | 0x4C504D41ULL)

// ----------------------------------------------------------------------------
// Кэш результатов
// ----------------------------------------------------------------------------
static cpu_info_t       g_cpu;
static lm_demo_result_t g_demo;
static bool             g_demo_done = false;

// ----------------------------------------------------------------------------
// Выровненные буферы page tables: PML4 (4 KB) + PDPT (4 KB) + 4×PD (16 KB).
// Всё в .bss, выравнивание 4 KB — обеспечивается атрибутом aligned(4096).
// ----------------------------------------------------------------------------
__attribute__((aligned(4096))) static uint64_t lm_pml4[512];
__attribute__((aligned(4096))) static uint64_t lm_pdpt[512];
__attribute__((aligned(4096))) static uint64_t lm_pd[4][512];

// Признак того, что таблицы построены.
static bool g_pml4_built = false;

// Функция из longmode_demo.asm: запускает демонстрацию.
// Принимает физический адрес PML4. Возвращает 0/1 — успех записи сигнатуры.
extern uint32_t lm_demo_trampoline(uint32_t pml4_phys, uint64_t* out_signature);

// ============================================================================
// CPUID — низкоуровневый враппер
// ============================================================================
static inline void cpuid_call(uint32_t leaf, uint32_t subleaf,
                              uint32_t* eax, uint32_t* ebx,
                              uint32_t* ecx, uint32_t* edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

// ============================================================================
// Заполнение CPU info
// ============================================================================
void lm_query_cpu(cpu_info_t* out) {
    if (!out) return;

    uint32_t a, b, c, d;

    // CPUID 0: max basic + vendor string
    cpuid_call(0, 0, &a, &b, &c, &d);
    out->max_basic_leaf = a;
    // EBX, EDX, ECX содержат 12 байт vendor-строки.
    *((uint32_t*)&out->vendor[0]) = b;
    *((uint32_t*)&out->vendor[4]) = d;
    *((uint32_t*)&out->vendor[8]) = c;
    out->vendor[12] = '\0';

    // CPUID 1: feature flags
    if (out->max_basic_leaf >= 1) {
        cpuid_call(1, 0, &a, &b, &c, &d);
        out->fpu  = (d >> 0)  & 1;
        out->pae  = (d >> 6)  & 1;
        out->apic = (d >> 9)  & 1;
        out->sse  = (d >> 25) & 1;
        out->sse2 = (d >> 26) & 1;
    } else {
        out->fpu = out->pae = out->apic = out->sse = out->sse2 = false;
    }

    // CPUID 0x80000000: max ext
    cpuid_call(0x80000000, 0, &a, &b, &c, &d);
    out->max_ext_leaf = a;

    // CPUID 0x80000001: ext feature flags
    if (out->max_ext_leaf >= 0x80000001) {
        cpuid_call(0x80000001, 0, &a, &b, &c, &d);
        out->nx            = (d >> 20) & 1;
        out->x64_1gb_pages = (d >> 26) & 1;
        out->rdtscp        = (d >> 27) & 1;
        out->long_mode     = (d >> 29) & 1;
    } else {
        out->nx = out->x64_1gb_pages = out->rdtscp = out->long_mode = false;
    }

    // CPUID 0x80000002..04: brand string (если поддерживается)
    if (out->max_ext_leaf >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            cpuid_call(0x80000002 + i, 0, &a, &b, &c, &d);
            uint32_t* p = (uint32_t*)&out->brand[i * 16];
            p[0] = a; p[1] = b; p[2] = c; p[3] = d;
        }
        out->brand[48] = '\0';
    } else {
        out->brand[0] = '\0';
    }
}

// ============================================================================
// Построение 4-уровневой таблицы страниц PML4
// ----------------------------------------------------------------------------
// Layout:
//   PML4[0]   -> PDPT (физ. адрес lm_pdpt)
//   PDPT[i]   -> PD i  (i = 0..3)
//   PD i [j]  -> 2 MB страница, физ. адрес = (i*1G + j*2M)
// Это identity-map первых 4 GB через 2 MB страницы (2-MB pages = bit 7 в PDE).
// Флаги: P=1, RW=1, US=0 (kernel-only).
// ============================================================================
uint32_t lm_build_pml4(void) {
    if (g_pml4_built) {
        return (uint32_t)(uintptr_t)lm_pml4;
    }

    // Нулевая инициализация (на всякий случай, .bss и так нулевой).
    for (int i = 0; i < 512; i++) lm_pml4[i] = 0;
    for (int i = 0; i < 512; i++) lm_pdpt[i] = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 512; j++) lm_pd[i][j] = 0;

    // PML4[0] -> PDPT
    uint64_t pml4e = (uint64_t)(uintptr_t)lm_pdpt;
    pml4e |= 0x3; // P=1, RW=1
    lm_pml4[0] = pml4e;

    // PDPT[0..3] -> PD0..PD3
    for (int i = 0; i < 4; i++) {
        uint64_t pdpte = (uint64_t)(uintptr_t)lm_pd[i];
        pdpte |= 0x3; // P=1, RW=1
        lm_pdpt[i] = pdpte;
    }

    // PD i [j] -> 2-MB страница (identity)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 512; j++) {
            uint64_t base = ((uint64_t)i * 0x40000000ULL)
                          + ((uint64_t)j * 0x00200000ULL);
            // P=1, RW=1, PS=1 (2-MB page)
            lm_pd[i][j] = base | 0x83ULL;
        }
    }

    g_pml4_built = true;
    g_demo.pml4_phys = (uint32_t)(uintptr_t)lm_pml4;
    return g_demo.pml4_phys;
}

// ============================================================================
// Запуск демонстрации Long Mode
// ============================================================================
bool lm_run_demo(lm_demo_result_t* out) {
    g_demo.attempted   = true;
    g_demo.supported   = g_cpu.long_mode;
    g_demo.pml4_built  = false;
    g_demo.entered_lm  = false;
    g_demo.returned_ok = false;
    g_demo.signature   = 0;

    if (!g_cpu.long_mode || !g_cpu.pae) {
        dbg_puts("[lm] CPU does not support Long Mode/PAE, skipping demo\n");
        if (out) *out = g_demo;
        g_demo_done = true;
        return false;
    }

    // 1. Построим PML4
    uint32_t pml4_phys = lm_build_pml4();
    g_demo.pml4_built = true;
    g_demo.pml4_phys  = pml4_phys;

    dbg_puts("[lm] PML4 built, running 32->64->32 trampoline...\n");

    // 2. Запускаем asm-трамплин с выключенными прерываниями.
    uint32_t old_eflags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(old_eflags));

    uint64_t sig = 0;
    uint32_t result = lm_demo_trampoline(pml4_phys, &sig);

    // Восстановить IF (если был выставлен).
    if (old_eflags & (1u << 9)) {
        __asm__ __volatile__("sti");
    }

    g_demo.entered_lm  = (result != 0);
    g_demo.returned_ok = true;  // Раз дошли сюда — мы вернулись в 32-bit.
    g_demo.signature   = sig;

    if (g_demo.entered_lm) {
        dbg_puts("[lm] OK: entered Long Mode, returned to 32-bit.\n");
    } else {
        dbg_puts("[lm] WARN: trampoline reported failure.\n");
    }

    g_demo_done = true;
    if (out) *out = g_demo;
    return g_demo.entered_lm;
}

// ============================================================================
// Удобный «всё-в-одном» init
// ============================================================================
void longmode_init(void) {
    lm_query_cpu(&g_cpu);

    dbg_puts("[lm] CPU vendor: ");
    dbg_puts(g_cpu.vendor);
    dbg_puts("\n");
    if (g_cpu.brand[0]) {
        dbg_puts("[lm] CPU brand: ");
        dbg_puts(g_cpu.brand);
        dbg_puts("\n");
    }
    dbg_puts("[lm] Long Mode: ");
    dbg_puts(g_cpu.long_mode ? "YES\n" : "NO\n");

    // Запускаем демо (1 раз).
    if (!g_demo_done) {
        lm_run_demo(NULL);
    }
}

const cpu_info_t* lm_cpu_info(void) {
    return &g_cpu;
}

const lm_demo_result_t* lm_last_result(void) {
    return &g_demo;
}
