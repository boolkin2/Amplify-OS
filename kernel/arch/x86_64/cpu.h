// ============================================================================
// cpu.h — CPU detection (cpuid wrapper, vendor, brand, features).
// ----------------------------------------------------------------------------
// Логика взята из kernel/arch/i386/longmode.c:lm_query_cpu, но:
//   - адаптировано под 64-bit (uint64_t для feature flag combos)
//   - добавлено: APIC bit, x2APIC bit, MSR bit
//   - выделено в отдельный модуль, чтобы lapic.c/smp.c могли его дёргать
//   - расширено детальной таблицей инструкций (MMX/SSE3..SSE4.2/AVX/AVX2/
//     AES/AVX512/SHA/BMI1/BMI2/POPCNT/RDRAND/RDSEED и т.д.) для отображения
//     в приложении "System Info".
// ============================================================================
#ifndef AOS_X86_64_CPU_H
#define AOS_X86_64_CPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char     vendor[13];
    char     brand[49];
    uint32_t max_basic_leaf;
    uint32_t max_ext_leaf;

    // ---- Базовые / системные (CPUID.1 EDX/ECX, CPUID.80000001.EDX) ----
    bool fpu;            // 1.EDX[0]
    bool tsc;            // 1.EDX[4]
    bool msr;            // 1.EDX[5]
    bool pae;            // 1.EDX[6]
    bool apic;           // 1.EDX[9]
    bool cmov;           // 1.EDX[15]
    bool pat;            // 1.EDX[16]
    bool clflush;        // 1.EDX[19]
    bool acpi_thermal;   // 1.EDX[22]
    bool mmx;            // 1.EDX[23]
    bool fxsr;           // 1.EDX[24]
    bool sse;            // 1.EDX[25]
    bool sse2;           // 1.EDX[26]
    bool htt;            // 1.EDX[28] — HyperThreading

    bool sse3;           // 1.ECX[0]
    bool pclmulqdq;      // 1.ECX[1]
    bool ssse3;          // 1.ECX[9]
    bool fma;            // 1.ECX[12]
    bool cmpxchg16b;     // 1.ECX[13]
    bool sse41;          // 1.ECX[19]
    bool sse42;          // 1.ECX[20]
    bool x2apic;         // 1.ECX[21]
    bool movbe;          // 1.ECX[22]
    bool popcnt;         // 1.ECX[23]
    bool aes;            // 1.ECX[25]
    bool xsave;          // 1.ECX[26]
    bool osxsave;        // 1.ECX[27]
    bool avx;            // 1.ECX[28]
    bool f16c;           // 1.ECX[29]
    bool rdrand;         // 1.ECX[30]
    bool hypervisor;     // 1.ECX[31]

    // ---- CPUID.7.0 EBX/ECX/EDX (extended features) ----
    bool fsgsbase;       // 7.0.EBX[0]
    bool bmi1;           // 7.0.EBX[3]
    bool avx2;           // 7.0.EBX[5]
    bool smep;           // 7.0.EBX[7]
    bool bmi2;           // 7.0.EBX[8]
    bool erms;           // 7.0.EBX[9]  — Enhanced REP MOVSB/STOSB
    bool rdseed;         // 7.0.EBX[18]
    bool adx;            // 7.0.EBX[19]
    bool smap;           // 7.0.EBX[20]
    bool avx512f;        // 7.0.EBX[16]
    bool avx512dq;       // 7.0.EBX[17]
    bool avx512cd;       // 7.0.EBX[28]
    bool avx512bw;       // 7.0.EBX[30]
    bool avx512vl;       // 7.0.EBX[31]
    bool sha;            // 7.0.EBX[29]

    // ---- CPUID.80000001 (расширенные) ----
    bool nx;             // 80000001.EDX[20]
    bool x64_1gb_pages;  // 80000001.EDX[26]
    bool rdtscp;         // 80000001.EDX[27]
    bool long_mode;      // 80000001.EDX[29]
    bool syscall;        // 80000001.EDX[11]
    bool lzcnt;          // 80000001.ECX[5]
    bool prefetchw;      // 80000001.ECX[8]
    bool sse4a;          // 80000001.ECX[6]   — AMD only
    bool svm;            // 80000001.ECX[2]   — AMD-V

    // ---- Vendor-string гипервизора (если CPUID.1.ECX[31] == 1) ----
    char     hv_vendor[13];

    // ---- Производные строки для UI ----
    uint8_t  cpu_model;        // CPUID.1.EAX bits 4..7 + ext model
    uint8_t  cpu_family;
    uint8_t  cpu_stepping;
    uint8_t  logical_cpus;     // CPUID.1.EBX[23..16] (количество логических CPU в пакете)
} cpu_info_t;

void               cpu_detect(void);
const cpu_info_t*  cpu_info(void);

// CPUID wrapper — то же что в longmode.c.
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf));
}

// MSR helpers — нужны для LAPIC (APIC_BASE MSR 0x1B), EFER (0xC0000080), и т.д.
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#endif
