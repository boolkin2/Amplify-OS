// ============================================================================
// crypto_cpu.c — детектирование hardware-возможностей крипты (этап 47.1).
// ----------------------------------------------------------------------------
// Использует CPUID для определения:
//   * AES-NI    (CPUID.01H:ECX.AESNI[bit 25])    — aesenc/aesenclast/...
//   * PCLMULQDQ (CPUID.01H:ECX.PCLMULQDQ[bit 1]) — carry-less multiply
//                                                  (нужен для быстрого GHASH в GCM)
//   * RDRAND    (CPUID.01H:ECX.RDRAND[bit 30])   — hardware RNG
//   * RDSEED    (CPUID.07H:EBX.RDSEED[bit 18])   — better hardware RNG
//   * SHA-NI    (CPUID.07H:EBX.SHA[bit 29])      — sha256rnds2/sha256msg1/...
//                                                  (на Goldmont+, Zen+; пока
//                                                  не используем — на TCG нет)
//
// Результаты кладутся в g_cpu, доступны через crypto_cpu().
//
// Connection с этапом 21 (старый cpu_features в i386): тот код стоит
// отдельно в kernel/arch/i386 — мы НЕ его трогаем. Этот модуль — независимая
// детекция именно для крипты, чтобы крипто-стек мог жить без зависимости от
// порядка инициализации x86_64.
// ============================================================================
#include "crypto.h"
#include "serial.h"

static crypto_cpu_features_t g_cpu;
static int g_cpu_initialized = 0;

const crypto_cpu_features_t* crypto_cpu(void) {
    return &g_cpu;
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(subleaf));
}

void crypto_cpu_init(void) {
    if (g_cpu_initialized) return;

    uint32_t a, b, c, d;
    // Сначала смотрим, какой максимальный leaf поддерживается.
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_basic_leaf = a;

    // Leaf 1: feature bits в ECX и EDX.
    if (max_basic_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        g_cpu.aesni     = (c & (1u << 25)) != 0;
        g_cpu.pclmulqdq = (c & (1u << 1))  != 0;
        g_cpu.rdrand    = (c & (1u << 30)) != 0;
    }

    // Leaf 7, subleaf 0: extended features в EBX.
    if (max_basic_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_cpu.rdseed    = (b & (1u << 18)) != 0;
        g_cpu.sha_ext   = (b & (1u << 29)) != 0;
    }

    g_cpu_initialized = 1;
}

void crypto_cpu_print(void) {
    serial_puts("[crypto] CPU features:");
    if (g_cpu.aesni)     serial_puts(" AES-NI");
    if (g_cpu.pclmulqdq) serial_puts(" PCLMULQDQ");
    if (g_cpu.rdrand)    serial_puts(" RDRAND");
    if (g_cpu.rdseed)    serial_puts(" RDSEED");
    if (g_cpu.sha_ext)   serial_puts(" SHA-NI");
    if (!g_cpu.aesni && !g_cpu.pclmulqdq && !g_cpu.rdrand) {
        serial_puts(" (none — software only)");
    }
    serial_puts("\n");
}

// ---- Утилиты, общие для всей крипты ----------------------------------------

// Безопасная очистка памяти: volatile-указатель гарантирует, что компилятор
// не выбросит этот цикл "как dead store" даже при -O2.
void crypto_wipe(void* p, size_t n) {
    volatile uint8_t* vp = (volatile uint8_t*)p;
    while (n--) *vp++ = 0;
}

// Constant-time memcmp: всегда обрабатывает все n байтов, разница битов
// XOR'ится в diff. Возвращает true при ПОЛНОМ совпадении. Используется при
// сравнении HMAC/GCM/Poly1305 tag'ов — иначе timing-attack может определить,
// сколько байтов совпало.
bool crypto_eq_const(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}
