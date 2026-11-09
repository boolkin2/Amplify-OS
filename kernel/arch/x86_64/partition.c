// ============================================================================
// partition.c — чтение и запись GPT (+ разбор MBR для совместимости).
// ============================================================================
#include "partition.h"
#include "disk_io.h"
#include "luks.h"
#include "crypto.h"
#include "serial.h"

#define SECTOR            512
#define GPT_ENTRY_COUNT   128
#define GPT_ENTRY_SIZE    128
#define GPT_ENTRY_SECTORS ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) / SECTOR)   // 32
#define GPT_RESERVED      (1 + 1 + GPT_ENTRY_SECTORS)                      // MBR+hdr+entries
#define ALIGN_SECTORS     2048                                             // 1 MiB

// GUID'ы в GPT хранятся в «смешанном» порядке: первые три группы —
// little-endian, последние две — как есть.  Здесь уже разложено по байтам.
static const uint8_t GUID_ESP[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
static const uint8_t GUID_MS_BASIC[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};
// CA7D7CCB-63ED-4C53-861C-1742536059CC — «Linux LUKS» (UAPI Discoverable
// Partitions).  Зашифрованный системный раздел помечаем так, а не Basic
// Data: иначе Windows увидела бы «неотформатированный том» и предложила
// бы его отформатировать.
static const uint8_t GUID_LUKS[16] = {
    0xCB,0x7C,0x7D,0xCA, 0xED,0x63, 0x53,0x4C,
    0x86,0x1C, 0x17,0x42,0x53,0x60,0x59,0xCC
};

typedef struct __attribute__((packed)) {
    char     signature[8];        // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entries_count;
    uint32_t entry_size;
    uint32_t entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];            // UTF-16LE
} gpt_entry_t;

// ----------------------------------------------------------------------------
// CRC32 (IEEE 802.3, отражённый полином) — таблица строится при первом вызове.
// ----------------------------------------------------------------------------
static uint32_t crc_table[256];
static bool     crc_ready = false;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = true;
}

uint32_t part_crc32(const void* data, uint64_t len) {
    if (!crc_ready) crc_init();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ----------------------------------------------------------------------------
static void mem_zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void mem_copy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
}

static bool guid_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return false;
    return true;
}

// Версия 4 (random) GUID: биты версии/варианта проставляются поверх случайных.
static void guid_random(uint8_t out[16]) {
    crypto_rand_bytes(out, 16);
    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

// Имя раздела — UTF-16LE внутри упакованной записи.  Работаем с байтами,
// а не с uint16_t*: указатель на поле packed-структуры может быть
// невыровненным (GCC справедливо предупреждает -Waddress-of-packed-member).
#define GPT_NAME_OFFSET 56      // offsetof(gpt_entry_t, name)
_Static_assert(__builtin_offsetof(gpt_entry_t, name) == GPT_NAME_OFFSET,
               "GPT entry layout changed");

static void name_to_utf16(gpt_entry_t* e, const char* src) {
    uint8_t* dst = (uint8_t*)e + GPT_NAME_OFFSET;
    int i = 0;
    for (; src[i] && i < 35; i++) { dst[i*2] = (uint8_t)src[i]; dst[i*2+1] = 0; }
    for (; i < 36; i++) { dst[i*2] = 0; dst[i*2+1] = 0; }
}

static void name_from_utf16(char* dst, const gpt_entry_t* e) {
    const uint8_t* src = (const uint8_t*)e + GPT_NAME_OFFSET;
    int i = 0;
    for (; i < PART_NAME_MAX; i++) {
        uint16_t ch = (uint16_t)(src[i*2] | (src[i*2+1] << 8));
        if (!ch) break;
        dst[i] = (ch < 128) ? (char)ch : '?';
    }
    dst[i] = 0;
}

// ----------------------------------------------------------------------------
// Признак FAT32 в первом секторе раздела.  Проверяем ровно те поля, на
// которые опирается наш драйвер, — иначе mount свалится уже после выбора.
// ----------------------------------------------------------------------------
static bool sector_is_fat32(const uint8_t* s) {
    uint16_t bps      = (uint16_t)(s[11] | (s[12] << 8));
    uint8_t  spc      = s[13];
    uint8_t  num_fats = s[16];
    uint32_t fatsz32  = (uint32_t)(s[36] | (s[37] << 8) | (s[38] << 16) | (s[39] << 24));
    if (bps != SECTOR) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;
    if (num_fats == 0 || num_fats > 2) return false;
    if (fatsz32 == 0) return false;
    if (s[510] != 0x55 || s[511] != 0xAA) return false;
    return true;
}

static bool probe_fat32_at(int dev, uint64_t lba) {
    uint8_t sec[SECTOR];
    if (!disk_read_sectors_dev(dev, lba, 1, sec)) return false;
    return sector_is_fat32(sec);
}

// ----------------------------------------------------------------------------
int part_probe(int dev, part_table_t* out) {
    if (!out) return -1;
    out->gpt = false;
    out->count = 0;
    if (!disk_present_dev(dev)) return -1;

    uint8_t sec[SECTOR];
    if (!disk_read_sectors_dev(dev, 1, 1, sec)) return -1;

    const gpt_header_t* h = (const gpt_header_t*)sec;
    bool sig_ok = h->signature[0]=='E' && h->signature[1]=='F' && h->signature[2]=='I' &&
                  h->signature[3]==' ' && h->signature[4]=='P' && h->signature[5]=='A' &&
                  h->signature[6]=='R' && h->signature[7]=='T';

    if (sig_ok && h->entry_size == GPT_ENTRY_SIZE && h->entries_count <= 256) {
        out->gpt = true;
        uint64_t lba = h->entries_lba;
        uint32_t remaining = h->entries_count;
        uint8_t buf[SECTOR];
        for (uint32_t s = 0; remaining > 0 && out->count < PART_MAX_ENTRIES; s++) {
            if (!disk_read_sectors_dev(dev, lba + s, 1, buf)) break;
            for (int k = 0; k < SECTOR / GPT_ENTRY_SIZE && remaining > 0; k++, remaining--) {
                const gpt_entry_t* e = (const gpt_entry_t*)(buf + k * GPT_ENTRY_SIZE);
                bool empty = true;
                for (int i = 0; i < 16; i++) if (e->type_guid[i]) { empty = false; break; }
                if (empty) continue;
                if (out->count >= PART_MAX_ENTRIES) break;
                part_entry_t* p = &out->entry[out->count++];
                p->first_lba = e->first_lba;
                p->last_lba  = e->last_lba;
                p->kind = guid_eq(e->type_guid, GUID_ESP)      ? PART_KIND_ESP :
                          guid_eq(e->type_guid, GUID_MS_BASIC) ? PART_KIND_DATA :
                          guid_eq(e->type_guid, GUID_LUKS)     ? PART_KIND_LUKS :
                                                                 PART_KIND_UNKNOWN;
                name_from_utf16(p->name, e);
                p->fat32 = probe_fat32_at(dev, p->first_lba);
                p->luks  = !p->fat32 && luks_probe(dev, p->first_lba, 0);
            }
        }
        return 0;
    }

    // Отката на MBR: читаем LBA 0 и разбираем 4 записи по 16 байт.
    if (!disk_read_sectors_dev(dev, 0, 1, sec)) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    for (int i = 0; i < 4 && out->count < PART_MAX_ENTRIES; i++) {
        const uint8_t* e = sec + 446 + i * 16;
        uint8_t  type  = e[4];
        uint32_t start = (uint32_t)(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24));
        uint32_t count = (uint32_t)(e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24));
        if (type == 0 || type == 0xEE || count == 0) continue;
        part_entry_t* p = &out->entry[out->count++];
        p->first_lba = start;
        p->last_lba  = (uint64_t)start + count - 1;
        p->kind = (type == 0xEF) ? PART_KIND_ESP :
                  (type == 0x0B || type == 0x0C) ? PART_KIND_DATA : PART_KIND_UNKNOWN;
        p->name[0] = 0;
        p->fat32 = probe_fat32_at(dev, p->first_lba);
        p->luks  = !p->fat32 && luks_probe(dev, p->first_lba, 0);
    }
    return 0;
}

// ----------------------------------------------------------------------------
int64_t part_find_system_fat32(int dev) {
    if (!disk_present_dev(dev)) return -1;

    // Голый FAT32 без таблицы разделов — так собран dev-образ build/disk-*.img.
    if (probe_fat32_at(dev, 0)) return 0;

    part_table_t tbl;
    if (part_probe(dev, &tbl) != 0) return -1;

    // Системный раздел приоритетнее ESP: на ESP лежит только загрузчик.
    for (int i = 0; i < tbl.count; i++)
        if (tbl.entry[i].fat32 && tbl.entry[i].kind == PART_KIND_DATA)
            return (int64_t)tbl.entry[i].first_lba;
    // Зашифрованный системный раздел: подставлять ESP нельзя — /disk
    // оказался бы загрузочным разделом, и система писала бы users.txt туда.
    for (int i = 0; i < tbl.count; i++)
        if (tbl.entry[i].luks) return -1;
    for (int i = 0; i < tbl.count; i++)
        if (tbl.entry[i].fat32)
            return (int64_t)tbl.entry[i].first_lba;
    return -1;
}

bool part_find_luks(int dev, uint64_t* lba, uint64_t* sectors) {
    part_table_t tbl;
    if (part_probe(dev, &tbl) != 0) return false;
    for (int i = 0; i < tbl.count; i++) {
        if (!tbl.entry[i].luks) continue;
        if (lba)     *lba     = tbl.entry[i].first_lba;
        if (sectors) *sectors = tbl.entry[i].last_lba - tbl.entry[i].first_lba + 1;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
static uint64_t align_up(uint64_t v, uint64_t a) {
    uint64_t r = v % a;
    return r ? v + (a - r) : v;
}

int part_plan(uint64_t total_sectors, bool vault, part_layout_t* out) {
    if (!out || total_sectors < PART_MIN_DISK_SECTORS) return -1;
    mem_zero(out, sizeof(*out));

    const uint64_t GIB = (1024ULL * 1024 * 1024) / SECTOR;
    // ESP: 512 МиБ на дисках от 16 ГиБ, иначе 256 МиБ — на маленьком диске
    // полгигабайта под загрузчик были бы расточительством.
    uint64_t esp = (total_sectors >= 16 * GIB) ? (512 * 1024 * 1024 / SECTOR)
                                               : (256 * 1024 * 1024 / SECTOR);

    uint64_t first = align_up(GPT_RESERVED, ALIGN_SECTORS);       // 2048
    uint64_t last  = total_sectors - GPT_RESERVED;                 // резерв под backup GPT
    if (last <= first) return -1;

    out->esp_lba     = first;
    out->esp_sectors = esp;

    uint64_t sys_start = align_up(out->esp_lba + esp, ALIGN_SECTORS);
    if (sys_start >= last) return -1;
    uint64_t avail = last - sys_start;

    out->vault = vault;
    if (vault) {
        // Системе — до 24 ГиБ, но не больше половины свободного места;
        // остальное уходит под /vault с пользовательскими данными.
        uint64_t sys = avail / 2;
        if (sys > 24 * GIB) sys = 24 * GIB;
        uint64_t vault_start = align_up(sys_start + sys, ALIGN_SECTORS);
        if (vault_start >= last || sys < GIB) {
            out->vault = false;                                    // места на два раздела нет
        } else {
            out->sys_lba       = sys_start;
            out->sys_sectors   = vault_start - sys_start;
            out->vault_lba     = vault_start;
            out->vault_sectors = last - vault_start;
            return 0;
        }
    }
    out->sys_lba     = sys_start;
    out->sys_sectors = avail;
    return 0;
}

// ----------------------------------------------------------------------------
static void write_protective_mbr(uint8_t* sec, uint64_t total_sectors) {
    mem_zero(sec, SECTOR);
    uint8_t* e = sec + 446;
    e[0] = 0x00;                       // не загрузочный
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;   // CHS начала = 0/0/2
    e[4] = 0xEE;                       // GPT protective
    e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;   // CHS конца = максимум
    e[8] = 1; e[9] = 0; e[10] = 0; e[11] = 0;
    uint64_t size = total_sectors - 1;
    if (size > 0xFFFFFFFFull) size = 0xFFFFFFFFull;
    e[12] = (uint8_t)(size & 0xFF);
    e[13] = (uint8_t)((size >> 8) & 0xFF);
    e[14] = (uint8_t)((size >> 16) & 0xFF);
    e[15] = (uint8_t)((size >> 24) & 0xFF);
    sec[510] = 0x55; sec[511] = 0xAA;
}

int part_write_gpt(int dev, uint64_t total_sectors, const part_layout_t* l) {
    if (!l || !disk_present_dev(dev)) return -1;
    if (total_sectors < PART_MIN_DISK_SECTORS) return -1;

    // Массив записей целиком — 16 КиБ, держим в static, чтобы не грузить стек.
    static uint8_t entries[GPT_ENTRY_COUNT * GPT_ENTRY_SIZE];
    mem_zero(entries, sizeof(entries));

    struct { uint64_t lba, sectors; const uint8_t* type; const char* name; } parts[3] = {
        { l->esp_lba,   l->esp_sectors,   GUID_ESP,      "AMPLIFY-ESP"    },
        { l->sys_lba,   l->sys_sectors,   l->sys_luks ? GUID_LUKS : GUID_MS_BASIC,
                                          "AMPLIFY-SYSTEM" },
        { l->vault_lba, l->vault_sectors, l->sys_luks ? GUID_LUKS : GUID_MS_BASIC,
                                          "AMPLIFY-VAULT"  },
    };
    int n = l->vault ? 3 : 2;
    for (int i = 0; i < n; i++) {
        gpt_entry_t* e = (gpt_entry_t*)(entries + i * GPT_ENTRY_SIZE);
        mem_copy(e->type_guid, parts[i].type, 16);
        guid_random(e->unique_guid);
        e->first_lba  = parts[i].lba;
        e->last_lba   = parts[i].lba + parts[i].sectors - 1;
        e->attributes = 0;
        name_to_utf16(e, parts[i].name);
    }
    uint32_t entries_crc = part_crc32(entries, sizeof(entries));

    uint8_t disk_guid[16];
    guid_random(disk_guid);

    uint64_t backup_lba        = total_sectors - 1;
    uint64_t backup_entries_lba = total_sectors - 1 - GPT_ENTRY_SECTORS;

    // Основной и резервный заголовки отличаются только current/backup/entries.
    uint8_t hdr_sec[SECTOR];
    for (int pass = 0; pass < 2; pass++) {
        mem_zero(hdr_sec, SECTOR);
        gpt_header_t* h = (gpt_header_t*)hdr_sec;
        const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
        mem_copy(h->signature, sig, 8);
        h->revision         = 0x00010000;
        h->header_size      = sizeof(gpt_header_t);
        h->header_crc32     = 0;
        h->reserved         = 0;
        h->current_lba      = (pass == 0) ? 1 : backup_lba;
        h->backup_lba       = (pass == 0) ? backup_lba : 1;
        h->first_usable_lba = GPT_RESERVED;
        h->last_usable_lba  = backup_entries_lba - 1;
        mem_copy(h->disk_guid, disk_guid, 16);
        h->entries_lba      = (pass == 0) ? 2 : backup_entries_lba;
        h->entries_count    = GPT_ENTRY_COUNT;
        h->entry_size       = GPT_ENTRY_SIZE;
        h->entries_crc32    = entries_crc;
        h->header_crc32     = part_crc32(h, sizeof(gpt_header_t));

        uint64_t at = (pass == 0) ? 1 : backup_lba;
        if (!disk_write_sectors_dev(dev, at, 1, hdr_sec)) return -1;
    }

    if (!disk_write_sectors_dev(dev, 2, GPT_ENTRY_SECTORS, entries)) return -1;
    if (!disk_write_sectors_dev(dev, backup_entries_lba, GPT_ENTRY_SECTORS, entries)) return -1;

    uint8_t mbr[SECTOR];
    write_protective_mbr(mbr, total_sectors);
    if (!disk_write_sectors_dev(dev, 0, 1, mbr)) return -1;
    if (!disk_flush_dev(dev)) return -1;

    serial_puts("[part] GPT written: esp@");
    serial_print_dec(l->esp_lba);
    serial_puts(" sys@");
    serial_print_dec(l->sys_lba);
    if (l->vault) { serial_puts(" vault@"); serial_print_dec(l->vault_lba); }
    serial_puts("\n");
    return 0;
}
