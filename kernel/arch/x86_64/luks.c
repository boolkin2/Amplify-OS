// ============================================================================
// luks.c — LUKS1: создание, открытие, слоты ключей (этап 48, FDE).
// ----------------------------------------------------------------------------
// Реализация сверена с cryptsetup (lib/luks1/{luks.h,af.c,keyencryption.c}):
//   * AF-splitter: полосы 0..n-2 случайные, накопитель XOR'ится с полосой и
//     «диффундируется» SHA-256 блоками по 32 байта с 4-байтовым big-endian
//     счётчиком перед данными; последняя полоса = MK XOR накопитель.
//   * Материал слота шифруется тем же шифром тома (aes-xts-plain64) ключом
//     PBKDF2(пароль, соль слота); IV — номер сектора ВНУТРИ материала
//     (iv_offset = 0), а не абсолютный номер сектора на диске.
//   * mk_digest = PBKDF2(MK, mk_digest_salt, mk_digest_iter, 20 байт).
// ============================================================================
#include "luks.h"
#include "disk_io.h"
#include "crypto.h"
#include "serial.h"
#include "heap.h"
#include "timer.h"

#define SECTOR             512
#define LUKS_STRIPES       4000
#define LUKS_DIGESTSIZE    20
#define LUKS_SALTSIZE      32
#define LUKS_KEY_ENABLED   0x00AC71F3u
#define LUKS_KEY_DISABLED  0x0000DEADu
#define LUKS_MIN_ITER      1000          // LUKS_{SLOT,MKD}_ITERATIONS_MIN
#define LUKS_ALIGN_SLOT    8             // 4096 байт в секторах
#define LUKS_KM_SECTORS    ((LUKS_KEY_BYTES * LUKS_STRIPES + SECTOR - 1) / SECTOR)  // 500
#define LUKS_SLOT_STRIDE   (((LUKS_KM_SECTORS + LUKS_ALIGN_SLOT - 1) / LUKS_ALIGN_SLOT) * LUKS_ALIGN_SLOT)
#define LUKS_PHDR_SECTORS  2

typedef struct __attribute__((packed)) {
    uint32_t active;              // все целые — big-endian
    uint32_t iterations;
    uint8_t  salt[LUKS_SALTSIZE];
    uint32_t km_offset;           // в секторах от начала раздела
    uint32_t stripes;
} luks_slot_t;

typedef struct __attribute__((packed)) {
    uint8_t     magic[6];
    uint16_t    version;
    char        cipher_name[32];
    char        cipher_mode[32];
    char        hash_spec[32];
    uint32_t    payload_offset;
    uint32_t    key_bytes;
    uint8_t     mk_digest[LUKS_DIGESTSIZE];
    uint8_t     mk_digest_salt[LUKS_SALTSIZE];
    uint32_t    mk_digest_iter;
    char        uuid[LUKS_UUID_LEN];
    luks_slot_t slot[LUKS_NUMKEYS];
    uint8_t     padding[432];
} luks_phdr_t;

_Static_assert(sizeof(luks_phdr_t) == 1024, "LUKS1 phdr must be exactly 1024 bytes");
_Static_assert(LUKS_ALIGN_SLOT + LUKS_NUMKEYS * LUKS_SLOT_STRIDE <= LUKS_PAYLOAD_OFFSET,
               "key slots must end before the payload");

static const uint8_t k_magic[6] = { 'L', 'U', 'K', 'S', 0xBA, 0xBE };

// Единственный заголовок и буфер материала слота.  Модуль не реентерабелен —
// как и весь дисковый стек (кооперативный планировщик, один поток I/O).
static luks_phdr_t g_hdr;
// [38.1.0] Материал слота (250 КиБ) — из кучи при первой надобности, не в
// .bss (CLAUDE.md §5.50). Не освобождаем: буфер один на модуль, после каждой
// операции он затирается (crypto_wipe), как и раньше.
#define LUKS_KM_BYTES ((uint64_t)LUKS_KM_SECTORS * SECTOR)
static uint8_t*    g_km;
static bool km_ready(void) {
    if (!g_km) g_km = (uint8_t*)kmalloc_aligned(LUKS_KM_BYTES, 4096);
    return g_km != 0;
}

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

static void mem_zero(void* p, uint64_t n) { uint8_t* b = (uint8_t*)p; for (uint64_t i = 0; i < n; i++) b[i] = 0; }
static void mem_copy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
}
static int  s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static bool str_eq(const char* field, const char* want) {   // поле фиксированной длины
    int i = 0;
    for (; want[i]; i++) if (field[i] != want[i]) return false;
    return field[i] == 0;
}
static void put_str(char* field, int cap, const char* s) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) field[i] = s[i];
    for (; i < cap; i++) field[i] = 0;
}

static uint32_t slot_km_offset(int slot) {
    return (uint32_t)(LUKS_ALIGN_SLOT + slot * LUKS_SLOT_STRIDE);
}

static void progress(luks_progress_cb cb, void* user, int pct, const char* msg) {
    if (cb) cb(pct, msg, user);
}

// ----------------------------------------------------------------------------
// AF-splitter (anti-forensic): diffuse / split / merge.
// ----------------------------------------------------------------------------
static void diffuse(uint8_t* buf, uint32_t size) {
    const uint32_t ds = CRYPTO_SHA256_LEN;
    uint32_t blocks = size / ds, pad = size % ds;
    uint8_t  d[CRYPTO_SHA256_LEN];
    for (uint32_t i = 0; i < blocks + (pad ? 1u : 0u); i++) {
        uint32_t len = (i < blocks) ? ds : pad;
        uint8_t  iv[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        crypto_sha256_ctx_t h;
        crypto_sha256_init(&h);
        crypto_sha256_update(&h, iv, 4);
        crypto_sha256_update(&h, buf + i * ds, len);
        crypto_sha256_final(&h, d);
        mem_copy(buf + i * ds, d, len);
    }
    crypto_wipe(d, sizeof(d));
}

static void af_split(const uint8_t* key, uint8_t* dst, uint32_t bs, uint32_t stripes) {
    uint8_t acc[LUKS_KEY_BYTES];
    mem_zero(acc, bs);
    uint32_t i = 0;
    for (; i < stripes - 1; i++) {
        crypto_rand_bytes(dst + (uint64_t)i * bs, bs);
        for (uint32_t k = 0; k < bs; k++) acc[k] ^= dst[(uint64_t)i * bs + k];
        diffuse(acc, bs);
    }
    for (uint32_t k = 0; k < bs; k++) dst[(uint64_t)i * bs + k] = (uint8_t)(key[k] ^ acc[k]);
    crypto_wipe(acc, sizeof(acc));
}

static void af_merge(const uint8_t* src, uint8_t* key, uint32_t bs, uint32_t stripes) {
    uint8_t acc[LUKS_KEY_BYTES];
    mem_zero(acc, bs);
    uint32_t i = 0;
    for (; i < stripes - 1; i++) {
        for (uint32_t k = 0; k < bs; k++) acc[k] ^= src[(uint64_t)i * bs + k];
        diffuse(acc, bs);
    }
    for (uint32_t k = 0; k < bs; k++) key[k] = (uint8_t)(src[(uint64_t)i * bs + k] ^ acc[k]);
    crypto_wipe(acc, sizeof(acc));
}

// Зашифровать/расшифровать материал слота в g_km ключом, выведенным из пароля.
static void km_crypt(const uint8_t key[LUKS_KEY_BYTES], bool encrypt) {
    static crypto_xts_ctx_t xts;                   // ~1 КиБ, не на стеке
    crypto_cpu_init();
    crypto_xts256_init(&xts, key, key + 32);
    for (uint32_t s = 0; s < LUKS_KM_SECTORS; s++) {
        uint8_t* p = g_km + (uint64_t)s * SECTOR;
        if (encrypt) crypto_xts_encrypt(&xts, s, p, p, SECTOR);
        else         crypto_xts_decrypt(&xts, s, p, p, SECTOR);
    }
    crypto_wipe(&xts, sizeof(xts));
}

static void make_uuid(char out[LUKS_UUID_LEN]) {
    static const char* hx = "0123456789abcdef";
    uint8_t r[16];
    crypto_rand_bytes(r, sizeof(r));
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);        // версия 4
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);        // вариант RFC 4122
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hx[r[i] >> 4];
        out[o++] = hx[r[i] & 0xF];
    }
    while (o < LUKS_UUID_LEN) out[o++] = 0;
}

// ----------------------------------------------------------------------------
// Заголовок.
// ----------------------------------------------------------------------------
static int read_header(int dev, uint64_t lba) {
    if (!disk_present_dev(dev)) return LUKS_IO_ERROR;
    if (!disk_read_sectors_dev(dev, lba, LUKS_PHDR_SECTORS, &g_hdr)) return LUKS_IO_ERROR;
    for (int i = 0; i < 6; i++) if (g_hdr.magic[i] != k_magic[i]) return LUKS_BAD_HEADER;
    if (be16(g_hdr.version) != 1) return LUKS_BAD_HEADER;
    return LUKS_OK;
}

static bool header_supported(void) {
    return str_eq(g_hdr.cipher_name, "aes") &&
           str_eq(g_hdr.cipher_mode, "xts-plain64") &&
           str_eq(g_hdr.hash_spec, "sha256") &&
           be32(g_hdr.key_bytes) == LUKS_KEY_BYTES;
}

static bool write_header(int dev, uint64_t lba) {
    return disk_write_sectors_dev(dev, lba, LUKS_PHDR_SECTORS, &g_hdr);
}

bool luks_probe(int dev, uint64_t lba, luks_info_t* out) {
    if (out) mem_zero(out, sizeof(*out));
    if (read_header(dev, lba) != LUKS_OK) return false;
    if (!out) return true;

    out->valid = true;
    out->supported = header_supported();
    for (int i = 0; i < LUKS_UUID_LEN; i++) out->uuid[i] = g_hdr.uuid[i];
    out->uuid[LUKS_UUID_LEN] = 0;
    int p = 0;
    for (int i = 0; i < 32 && g_hdr.cipher_name[i]; i++) out->cipher[p++] = g_hdr.cipher_name[i];
    out->cipher[p++] = '-';
    for (int i = 0; i < 32 && g_hdr.cipher_mode[i]; i++) out->cipher[p++] = g_hdr.cipher_mode[i];
    out->cipher[p] = 0;
    for (int i = 0; i < 32 && g_hdr.hash_spec[i]; i++) out->hash[i] = g_hdr.hash_spec[i];
    out->payload_offset = be32(g_hdr.payload_offset);
    out->key_bytes      = be32(g_hdr.key_bytes);
    out->mk_digest_iter = be32(g_hdr.mk_digest_iter);
    for (int s = 0; s < LUKS_NUMKEYS; s++) {
        if (be32(g_hdr.slot[s].active) == LUKS_KEY_ENABLED) {
            out->active_slots++;
            out->slot_iter[s] = be32(g_hdr.slot[s].iterations);
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Бенчмарк PBKDF2: удваиваем число итераций, пока замер не станет
// достаточно длинным для таймера с шагом 10 мс.
// ----------------------------------------------------------------------------
uint32_t luks_benchmark(uint32_t key_len) {
    static const uint8_t salt[LUKS_SALTSIZE] = { 0 };
    uint8_t  out[LUKS_KEY_BYTES];
    if (key_len > sizeof(out)) key_len = sizeof(out);
    uint32_t iters = 1000;
    uint64_t ms = 0;
    for (;;) {
        uint64_t t0 = timer_get_uptime_ms();
        crypto_pbkdf2_hmac_sha256("amplify-benchmark", 17, salt, sizeof(salt),
                                  iters, out, key_len);
        ms = timer_get_uptime_ms() - t0;
        if (ms >= 120 || iters >= (1u << 26)) break;
        iters *= (ms < 15) ? 8u : 2u;
    }
    crypto_wipe(out, sizeof(out));
    if (ms == 0) ms = 1;
    uint64_t rate = (uint64_t)iters * 1000u / ms;
    if (rate > 0xFFFFFFFFull) rate = 0xFFFFFFFFull;
    return (uint32_t)rate;
}

static uint32_t iterations_for(uint32_t rate, uint32_t ms) {
    uint64_t it = (uint64_t)rate * ms / 1000u;
    if (it < LUKS_MIN_ITER) it = LUKS_MIN_ITER;
    if (it > 0x7FFFFFFFull) it = 0x7FFFFFFFull;
    return (uint32_t)it;
}

// ----------------------------------------------------------------------------
// Запись одного слота: соль, AF-split MK, шифрование ключом из пароля.
// Заголовок в памяти (g_hdr) обновляется, на диск его пишет вызывающий —
// ПОСЛЕ материала, чтобы прерванная операция не оставила активный слот
// без материала.
// ----------------------------------------------------------------------------
static bool write_slot(int dev, uint64_t lba, int slot, const uint8_t mk[LUKS_KEY_BYTES],
                       const char* pass, uint32_t iters,
                       luks_progress_cb cb, void* user, int p0, int p1) {
    luks_slot_t* s = &g_hdr.slot[slot];
    uint8_t key[LUKS_KEY_BYTES];

    if (!km_ready()) return false;
    crypto_rand_bytes(s->salt, LUKS_SALTSIZE);
    progress(cb, user, p0, "Splitting the master key (AF, 4000 stripes)");
    af_split(mk, g_km, LUKS_KEY_BYTES, LUKS_STRIPES);

    progress(cb, user, p0 + (p1 - p0) / 4, "Deriving the key slot key (PBKDF2-SHA256)");
    crypto_pbkdf2_hmac_sha256(pass, (size_t)s_len(pass), s->salt, LUKS_SALTSIZE,
                              iters, key, LUKS_KEY_BYTES);

    progress(cb, user, p0 + (p1 - p0) * 3 / 4, "Writing the key slot");
    km_crypt(key, true);
    crypto_wipe(key, sizeof(key));
    bool ok = disk_write_sectors_dev(dev, lba + be32(s->km_offset), LUKS_KM_SECTORS, g_km);
    crypto_wipe(g_km, LUKS_KM_BYTES);
    if (!ok) return false;

    s->active     = be32(LUKS_KEY_ENABLED);
    s->iterations = be32(iters);
    return true;
}

// ----------------------------------------------------------------------------
int luks_format(int dev, uint64_t lba, uint64_t sectors, const char* passphrase,
                uint32_t iter_ms, uint8_t mk_out[LUKS_KEY_BYTES],
                luks_progress_cb cb, void* user) {
    if (!disk_present_dev(dev) || !passphrase || !passphrase[0]) return LUKS_IO_ERROR;
    if (sectors < (uint64_t)LUKS_PAYLOAD_OFFSET + 2048) return LUKS_IO_ERROR;
    crypto_cpu_init();

    uint8_t mk[LUKS_KEY_BYTES];
    progress(cb, user, 2, "Generating the master key");
    crypto_rand_bytes(mk, sizeof(mk));

    uint32_t slot_iter = LUKS_MIN_ITER, mk_iter = LUKS_MIN_ITER;
    if (iter_ms) {
        progress(cb, user, 5, "Measuring PBKDF2 speed");
        uint32_t rate = luks_benchmark(LUKS_KEY_BYTES);
        slot_iter = iterations_for(rate, iter_ms);
        // Digest — 20 байт (один блок SHA-256), т.е. вдвое дешевле 64-байтного
        // ключа слота; cryptsetup отводит на него 125 мс.
        mk_iter = iterations_for(rate * 2u > rate ? rate * 2u : rate, 125);
    }

    mem_zero(&g_hdr, sizeof(g_hdr));
    for (int i = 0; i < 6; i++) g_hdr.magic[i] = k_magic[i];
    g_hdr.version = be16(1);
    put_str(g_hdr.cipher_name, 32, "aes");
    put_str(g_hdr.cipher_mode, 32, "xts-plain64");
    put_str(g_hdr.hash_spec,   32, "sha256");
    g_hdr.payload_offset = be32(LUKS_PAYLOAD_OFFSET);
    g_hdr.key_bytes      = be32(LUKS_KEY_BYTES);
    crypto_rand_bytes(g_hdr.mk_digest_salt, LUKS_SALTSIZE);
    g_hdr.mk_digest_iter = be32(mk_iter);
    progress(cb, user, 10, "Computing the master key digest");
    crypto_pbkdf2_hmac_sha256(mk, LUKS_KEY_BYTES, g_hdr.mk_digest_salt, LUKS_SALTSIZE,
                              mk_iter, g_hdr.mk_digest, LUKS_DIGESTSIZE);
    make_uuid(g_hdr.uuid);
    // Как cryptsetup: у ВСЕХ слотов заранее прописаны смещение и число
    // полос, выключенные помечены 0x0000DEAD.
    for (int s = 0; s < LUKS_NUMKEYS; s++) {
        g_hdr.slot[s].active     = be32(LUKS_KEY_DISABLED);
        g_hdr.slot[s].iterations = 0;
        g_hdr.slot[s].km_offset  = be32(slot_km_offset(s));
        g_hdr.slot[s].stripes    = be32(LUKS_STRIPES);
    }

    // Старые сигнатуры (FAT32 BPB, прежний LUKS) в области заголовка
    // стираем целиком — иначе blkid мог бы опознать раздел по остаткам.
    progress(cb, user, 15, "Wiping the header area");
    if (!disk_zero_sectors_dev(dev, lba, LUKS_PAYLOAD_OFFSET)) { crypto_wipe(mk, sizeof(mk)); return LUKS_IO_ERROR; }

    if (!write_slot(dev, lba, 0, mk, passphrase, slot_iter, cb, user, 20, 90) ||
        !write_header(dev, lba) || !disk_flush_dev(dev)) {
        crypto_wipe(mk, sizeof(mk));
        return LUKS_IO_ERROR;
    }
    progress(cb, user, 100, "Sealed");

    serial_puts("[luks] formatted dev ");
    serial_print_dec((uint64_t)dev);
    serial_puts(" @ lba ");
    serial_print_dec(lba);
    serial_puts(": aes-xts-plain64/sha256, payload @ +");
    serial_print_dec(LUKS_PAYLOAD_OFFSET);
    serial_puts(", slot iter ");
    serial_print_dec(slot_iter);
    serial_puts(", mk iter ");
    serial_print_dec(mk_iter);
    serial_puts("\n");

    if (mk_out) mem_copy(mk_out, mk, LUKS_KEY_BYTES);
    crypto_wipe(mk, sizeof(mk));
    return LUKS_OK;
}

// ----------------------------------------------------------------------------
// Проверка кандидата в MK по digest'у из заголовка.
static bool mk_matches(const uint8_t mk[LUKS_KEY_BYTES]) {
    uint8_t d[LUKS_DIGESTSIZE];
    crypto_pbkdf2_hmac_sha256(mk, LUKS_KEY_BYTES, g_hdr.mk_digest_salt, LUKS_SALTSIZE,
                              be32(g_hdr.mk_digest_iter), d, LUKS_DIGESTSIZE);
    bool ok = crypto_eq_const(d, g_hdr.mk_digest, LUKS_DIGESTSIZE);
    crypto_wipe(d, sizeof(d));
    return ok;
}

int luks_open(int dev, uint64_t lba, const char* passphrase,
              uint8_t mk_out[LUKS_KEY_BYTES], int* slot_out) {
    int r = read_header(dev, lba);
    if (r != LUKS_OK) return r;
    if (!header_supported()) return LUKS_BAD_HEADER;
    crypto_cpu_init();
    if (!km_ready()) return LUKS_IO_ERROR;

    uint8_t key[LUKS_KEY_BYTES], cand[LUKS_KEY_BYTES];
    for (int s = 0; s < LUKS_NUMKEYS; s++) {
        const luks_slot_t* ks = &g_hdr.slot[s];
        if (be32(ks->active) != LUKS_KEY_ENABLED) continue;
        if (be32(ks->stripes) != LUKS_STRIPES) continue;          // не наш формат слота

        if (!disk_read_sectors_dev(dev, lba + be32(ks->km_offset), LUKS_KM_SECTORS, g_km))
            return LUKS_IO_ERROR;
        crypto_pbkdf2_hmac_sha256(passphrase, (size_t)s_len(passphrase), ks->salt, LUKS_SALTSIZE,
                                  be32(ks->iterations), key, LUKS_KEY_BYTES);
        km_crypt(key, false);
        af_merge(g_km, cand, LUKS_KEY_BYTES, LUKS_STRIPES);
        crypto_wipe(g_km, LUKS_KM_BYTES);
        crypto_wipe(key, sizeof(key));

        if (mk_matches(cand)) {
            if (mk_out) mem_copy(mk_out, cand, LUKS_KEY_BYTES);
            if (slot_out) *slot_out = s;
            crypto_wipe(cand, sizeof(cand));
            serial_puts("[luks] key slot ");
            serial_print_dec((uint64_t)s);
            serial_puts(" unlocked\n");
            return LUKS_OK;
        }
    }
    crypto_wipe(cand, sizeof(cand));
    return LUKS_BAD_PASS;
}

int luks_add_key(int dev, uint64_t lba, const uint8_t mk[LUKS_KEY_BYTES],
                 const char* passphrase, uint32_t iter_ms) {
    if (!passphrase || !passphrase[0]) return -1;
    if (read_header(dev, lba) != LUKS_OK || !header_supported()) return -1;
    if (!mk_matches(mk)) return -1;                    // чужой ключ — отказ

    int slot = -1;
    for (int s = 0; s < LUKS_NUMKEYS; s++)
        if (be32(g_hdr.slot[s].active) != LUKS_KEY_ENABLED) { slot = s; break; }
    if (slot < 0) return -1;

    uint32_t iters = iter_ms ? iterations_for(luks_benchmark(LUKS_KEY_BYTES), iter_ms)
                             : LUKS_MIN_ITER;
    // luks_benchmark не трогает g_hdr, но на всякий случай перечитываем:
    // заголовок в памяти должен совпадать с диском перед записью.
    if (read_header(dev, lba) != LUKS_OK) return -1;
    if (!write_slot(dev, lba, slot, mk, passphrase, iters, 0, 0, 0, 0)) return -1;
    if (!write_header(dev, lba) || !disk_flush_dev(dev)) return -1;
    serial_puts("[luks] added key slot ");
    serial_print_dec((uint64_t)slot);
    serial_puts("\n");
    return slot;
}

int luks_kill_slot(int dev, uint64_t lba, int slot) {
    if (slot < 0 || slot >= LUKS_NUMKEYS) return -1;
    if (read_header(dev, lba) != LUKS_OK) return -1;
    int active = 0;
    for (int s = 0; s < LUKS_NUMKEYS; s++)
        if (be32(g_hdr.slot[s].active) == LUKS_KEY_ENABLED) active++;
    if (be32(g_hdr.slot[slot].active) != LUKS_KEY_ENABLED || active <= 1) return -1;

    // Сначала уничтожаем материал (без него пароль бесполезен), потом слот.
    if (!disk_zero_sectors_dev(dev, lba + be32(g_hdr.slot[slot].km_offset), LUKS_KM_SECTORS))
        return -1;
    g_hdr.slot[slot].active     = be32(LUKS_KEY_DISABLED);
    g_hdr.slot[slot].iterations = 0;
    mem_zero(g_hdr.slot[slot].salt, LUKS_SALTSIZE);
    if (!write_header(dev, lba) || !disk_flush_dev(dev)) return -1;
    serial_puts("[luks] destroyed key slot ");
    serial_print_dec((uint64_t)slot);
    serial_puts("\n");
    return 0;
}
