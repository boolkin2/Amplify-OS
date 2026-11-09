// ============================================================================
// cpu.c — CPUID-based detection.
// ----------------------------------------------------------------------------
// Этап 45+ (расширение): теперь читаем не только Long Mode и SSE2, но и
// полный список наборов инструкций (MMX, SSE3..SSE4.2, AVX/AVX2, AES,
// AVX-512 F/DQ/CD/BW/VL, SHA, BMI1/BMI2, POPCNT, RDRAND, RDSEED, FMA,
// LZCNT, RDTSCP, NX, XSAVE и т.д.) — чтобы приложение "System Info"
// могло их показать пользователю.
// ============================================================================
#include "cpu.h"

static cpu_info_t g_cpu;

void cpu_detect(void) {
    uint32_t a, b, c, d;

    // ---- CPUID 0: max_basic + vendor string ("GenuineIntel" / "AuthenticAMD") ----
    cpuid(0, 0, &a, &b, &c, &d);
    g_cpu.max_basic_leaf = a;
    *((uint32_t*)&g_cpu.vendor[0]) = b;
    *((uint32_t*)&g_cpu.vendor[4]) = d;
    *((uint32_t*)&g_cpu.vendor[8]) = c;
    g_cpu.vendor[12] = 0;

    // ---- CPUID 1: feature flags (EDX/ECX) + family/model/stepping ----
    if (g_cpu.max_basic_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);

        // family/model/stepping (упрощённое: ext-model добавляется при family==6 или 15)
        uint8_t base_family = (a >> 8) & 0xF;
        uint8_t base_model  = (a >> 4) & 0xF;
        uint8_t ext_family  = (a >> 20) & 0xFF;
        uint8_t ext_model   = (a >> 16) & 0xF;
        g_cpu.cpu_stepping  = a & 0xF;
        g_cpu.cpu_family    = base_family + ((base_family == 0xF) ? ext_family : 0);
        g_cpu.cpu_model     = base_model  | ((base_family == 0xF || base_family == 6) ? (ext_model << 4) : 0);
        g_cpu.logical_cpus  = (b >> 16) & 0xFF;

        // EDX:
        g_cpu.fpu          = (d >> 0)  & 1;
        g_cpu.tsc          = (d >> 4)  & 1;
        g_cpu.msr          = (d >> 5)  & 1;
        g_cpu.pae          = (d >> 6)  & 1;
        g_cpu.apic         = (d >> 9)  & 1;
        g_cpu.cmov         = (d >> 15) & 1;
        g_cpu.pat          = (d >> 16) & 1;
        g_cpu.clflush      = (d >> 19) & 1;
        g_cpu.acpi_thermal = (d >> 22) & 1;
        g_cpu.mmx          = (d >> 23) & 1;
        g_cpu.fxsr         = (d >> 24) & 1;
        g_cpu.sse          = (d >> 25) & 1;
        g_cpu.sse2         = (d >> 26) & 1;
        g_cpu.htt          = (d >> 28) & 1;

        // ECX:
        g_cpu.sse3         = (c >> 0)  & 1;
        g_cpu.pclmulqdq    = (c >> 1)  & 1;
        g_cpu.ssse3        = (c >> 9)  & 1;
        g_cpu.fma          = (c >> 12) & 1;
        g_cpu.cmpxchg16b   = (c >> 13) & 1;
        g_cpu.sse41        = (c >> 19) & 1;
        g_cpu.sse42        = (c >> 20) & 1;
        g_cpu.x2apic       = (c >> 21) & 1;
        g_cpu.movbe        = (c >> 22) & 1;
        g_cpu.popcnt       = (c >> 23) & 1;
        g_cpu.aes          = (c >> 25) & 1;
        g_cpu.xsave        = (c >> 26) & 1;
        g_cpu.osxsave      = (c >> 27) & 1;
        g_cpu.avx          = (c >> 28) & 1;
        g_cpu.f16c         = (c >> 29) & 1;
        g_cpu.rdrand       = (c >> 30) & 1;
        g_cpu.hypervisor   = (c >> 31) & 1;
    }

    // ---- CPUID 7.0: extended feature flags (BMI/AVX2/AVX512/SHA/...) ----
    if (g_cpu.max_basic_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_cpu.fsgsbase  = (b >> 0)  & 1;
        g_cpu.bmi1      = (b >> 3)  & 1;
        g_cpu.avx2      = (b >> 5)  & 1;
        g_cpu.smep      = (b >> 7)  & 1;
        g_cpu.bmi2      = (b >> 8)  & 1;
        g_cpu.erms      = (b >> 9)  & 1;
        g_cpu.avx512f   = (b >> 16) & 1;
        g_cpu.avx512dq  = (b >> 17) & 1;
        g_cpu.rdseed    = (b >> 18) & 1;
        g_cpu.adx       = (b >> 19) & 1;
        g_cpu.smap      = (b >> 20) & 1;
        g_cpu.avx512cd  = (b >> 28) & 1;
        g_cpu.sha       = (b >> 29) & 1;
        g_cpu.avx512bw  = (b >> 30) & 1;
        g_cpu.avx512vl  = (b >> 31) & 1;
    }

    // ---- CPUID 0x80000000: max_ext ----
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    g_cpu.max_ext_leaf = a;

    // ---- CPUID 0x80000001: long mode, NX, RDTSCP, 1G pages, SYSCALL, LZCNT, ... ----
    if (g_cpu.max_ext_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        g_cpu.syscall       = (d >> 11) & 1;
        g_cpu.nx            = (d >> 20) & 1;
        g_cpu.x64_1gb_pages = (d >> 26) & 1;
        g_cpu.rdtscp        = (d >> 27) & 1;
        g_cpu.long_mode     = (d >> 29) & 1;
        g_cpu.svm           = (c >> 2)  & 1;
        g_cpu.lzcnt         = (c >> 5)  & 1;
        g_cpu.sse4a         = (c >> 6)  & 1;
        g_cpu.prefetchw     = (c >> 8)  & 1;
    }

    // ---- CPUID 0x80000002..04: brand string (48 байт + 0) ----
    if (g_cpu.max_ext_leaf >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            uint32_t* p = (uint32_t*)&g_cpu.brand[i * 16];
            p[0] = a; p[1] = b; p[2] = c; p[3] = d;
        }
        g_cpu.brand[48] = 0;
    } else {
        g_cpu.brand[0] = 0;
    }

    // ---- CPUID 0x40000000: hypervisor vendor (если флаг 1.ECX[31] выставлен) ----
    g_cpu.hv_vendor[0] = 0;
    if (g_cpu.hypervisor) {
        cpuid(0x40000000, 0, &a, &b, &c, &d);
        // Под гипервизором обычно EAX = max_hv_leaf, EBX/ECX/EDX = "TCGTCGTCGTCG" (QEMU/TCG),
        // "KVMKVMKVM" (KVM), "Microsoft Hv" (Hyper-V), "VMwareVMware", и т.д.
        *((uint32_t*)&g_cpu.hv_vendor[0]) = b;
        *((uint32_t*)&g_cpu.hv_vendor[4]) = c;
        *((uint32_t*)&g_cpu.hv_vendor[8]) = d;
        g_cpu.hv_vendor[12] = 0;
    }
}

const cpu_info_t* cpu_info(void) { return &g_cpu; }
