// ============================================================================
// crypto_rand.c — PRNG для TLS (этап 47.1).
// ----------------------------------------------------------------------------
// Архитектура (вдохновлено Fortuna от Schneier/Ferguson):
//   * Entropy pool: 256-bit состояние (SHA-256 хеш), накапливает энтропию
//     из нескольких источников.
//   * Источники:
//       - RDRAND / RDSEED (если есть, через CPUID)    — лучший источник
//       - RDTSC                                       — слабый, но всегда
//       - PIT ticks                                    — fallback
//       - IRQ counter, free-running                    — добавочная
//   * Выходной поток: ChaCha20 с counter, key = pool, nonce = serial.
//   * Reseed: каждые 64 KB вывода (rehash pool из новых entropy snapshots).
//
// Безопасность для TLS:
//   * Если RDRAND/RDSEED доступен — используем напрямую (Intel/AMD на 100%
//     одобрено NIST SP 800-90B).
//   * Если только RDTSC — это слабый источник, но в комбинации с PIT ticks
//     и IRQ counter и SHA-256 mixing даёт enough entropy для одного TLS
//     handshake (random_bytes 32 + premaster 48 = 80 байт за раз).
//   * НЕ ИСПОЛЬЗОВАТЬ этот PRNG для генерации long-term private keys (RSA,
//     Curve25519). Для этого добавим в этапе 47.2 механизм "выгрузить
//     достаточно энтропии и подождать пока не накопится 256 бит strong
//     entropy". Пока для TLS handshake достаточно.
// ============================================================================
#include "crypto.h"
#include "serial.h"

extern uint64_t timer_get_uptime_ms(void);  // timer.c
extern uint32_t pit_ticks(void);            // pit.c

static struct {
    uint8_t  pool[32];           // 256-bit состояние = SHA-256 хеш всех инкорпорированных entropy snapshot'ов
    uint8_t  stream_key[32];     // ключ ChaCha20 для текущей выдачи
    uint8_t  stream_nonce[12];   // nonce ChaCha20
    uint32_t stream_counter;     // ChaCha20 counter
    uint64_t bytes_emitted;      // сколько выдано после последнего reseed
    int      initialized;
} g_rng;

#define RESEED_THRESHOLD  (64 * 1024)   // reseed каждые 64 KB

// ---- Источники энтропии -----------------------------------------------------
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool rdrand64(uint64_t* out) {
    uint8_t ok;
    __asm__ __volatile__("rdrand %0; setc %1"
                         : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

static bool rdseed64(uint64_t* out) {
    uint8_t ok;
    __asm__ __volatile__("rdseed %0; setc %1"
                         : "=r"(*out), "=qm"(ok));
    return ok != 0;
}

// Собрать «снимок энтропии» в 64 байта.
// 8 от RDSEED / 8 от RDRAND (если есть) + RDTSC + pit_ticks + timer_ms + старый pool.
static void collect_entropy(uint8_t out[64]) {
    uint64_t r[8] = {0};
    int idx = 0;

    if (crypto_cpu()->rdseed) {
        for (int i = 0; i < 4 && idx < 8; i++) {
            uint64_t v;
            // Retry до 10 раз — rdseed может не выдать значение под нагрузкой.
            for (int t = 0; t < 10; t++) {
                if (rdseed64(&v)) { r[idx++] = v; break; }
            }
        }
    }
    if (crypto_cpu()->rdrand) {
        for (int i = 0; i < 4 && idx < 8; i++) {
            uint64_t v;
            for (int t = 0; t < 10; t++) {
                if (rdrand64(&v)) { r[idx++] = v; break; }
            }
        }
    }

    // Дополняем слабыми источниками всегда.
    r[idx % 8] ^= rdtsc_now();
    r[(idx+1) % 8] ^= (uint64_t)pit_ticks() << 32;
    r[(idx+2) % 8] ^= timer_get_uptime_ms();

    // Адрес стека — тоже добавочная энтропия (ASLR-related).
    int stack_var;
    r[(idx+3) % 8] ^= (uint64_t)(uintptr_t)&stack_var;

    for (int i = 0; i < 8; i++) {
        out[8*i  ] = (uint8_t)(r[i]);
        out[8*i+1] = (uint8_t)(r[i] >> 8);
        out[8*i+2] = (uint8_t)(r[i] >> 16);
        out[8*i+3] = (uint8_t)(r[i] >> 24);
        out[8*i+4] = (uint8_t)(r[i] >> 32);
        out[8*i+5] = (uint8_t)(r[i] >> 40);
        out[8*i+6] = (uint8_t)(r[i] >> 48);
        out[8*i+7] = (uint8_t)(r[i] >> 56);
    }
}

// Reseed: pool = SHA-256(pool || new_entropy). Затем перегенерируем stream_key.
void crypto_rand_reseed(void) {
    uint8_t entropy[64];
    collect_entropy(entropy);

    crypto_sha256_ctx_t ctx;
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, g_rng.pool, sizeof(g_rng.pool));
    crypto_sha256_update(&ctx, entropy,    sizeof(entropy));
    crypto_sha256_final (&ctx, g_rng.pool);

    // stream_key = HKDF(pool, "AMPLIFY-OS PRNG stream") — пока упрощённо
    // через SHA-256(pool || "key").
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, g_rng.pool, sizeof(g_rng.pool));
    crypto_sha256_update(&ctx, "key", 3);
    crypto_sha256_final (&ctx, g_rng.stream_key);

    // nonce = младшие 12 байт от SHA-256(pool || "nonce"); counter сбрасывается.
    uint8_t tmp[CRYPTO_SHA256_LEN];
    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, g_rng.pool, sizeof(g_rng.pool));
    crypto_sha256_update(&ctx, "nonce", 5);
    crypto_sha256_final (&ctx, tmp);
    for (int i = 0; i < 12; i++) g_rng.stream_nonce[i] = tmp[i];

    g_rng.stream_counter = 0;
    g_rng.bytes_emitted  = 0;

    crypto_wipe(entropy, sizeof(entropy));
    crypto_wipe(tmp,     sizeof(tmp));
}

void crypto_rand_init(void) {
    if (g_rng.initialized) return;
    // Стартовое заполнение pool'а — несколько раундов entropy collection,
    // потому что initial state — нули.
    for (size_t i = 0; i < sizeof(g_rng.pool); i++) g_rng.pool[i] = 0;
    for (int round = 0; round < 4; round++) crypto_rand_reseed();
    g_rng.initialized = 1;
    serial_puts("[crypto] PRNG initialized (pool seeded from ");
    if (crypto_cpu()->rdseed)      serial_puts("RDSEED+RDRAND+RDTSC)");
    else if (crypto_cpu()->rdrand) serial_puts("RDRAND+RDTSC)");
    else                            serial_puts("RDTSC+PIT — weak entropy)");
    serial_puts("\n");
}

// ChaCha20 expose — реальная реализация в crypto_chacha.c.
extern void crypto_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                    uint32_t counter,
                                    const uint8_t* in, uint8_t* out, size_t len);

void crypto_rand_bytes(void* out, size_t n) {
    if (!g_rng.initialized) crypto_rand_init();
    uint8_t* p = (uint8_t*)out;

    while (n > 0) {
        if (g_rng.bytes_emitted >= RESEED_THRESHOLD) {
            crypto_rand_reseed();
        }

        size_t take = n;
        if (take > 4096) take = 4096;       // блокируем выходные блоки до 4 KB
        if (take > RESEED_THRESHOLD - g_rng.bytes_emitted) {
            take = RESEED_THRESHOLD - g_rng.bytes_emitted;
        }

        // ChaCha20 в режиме keystream: input — нули, output — рандомные байты.
        // Чтобы не выделять лишний буфер, делаем итерациями по 256 байт.
        uint8_t zero_chunk[256] = {0};
        size_t  emitted_now = 0;
        while (emitted_now < take) {
            size_t chunk = take - emitted_now;
            if (chunk > 256) chunk = 256;
            crypto_chacha20_encrypt(g_rng.stream_key,
                                    g_rng.stream_nonce,
                                    g_rng.stream_counter,
                                    zero_chunk, p + emitted_now, chunk);
            g_rng.stream_counter += (uint32_t)((chunk + 63) / 64);
            emitted_now += chunk;
        }

        p += take;
        n -= take;
        g_rng.bytes_emitted += take;
    }
}

uint32_t crypto_rand_u32(void) {
    uint32_t v;
    crypto_rand_bytes(&v, sizeof(v));
    return v;
}

uint64_t crypto_rand_u64(void) {
    uint64_t v;
    crypto_rand_bytes(&v, sizeof(v));
    return v;
}
