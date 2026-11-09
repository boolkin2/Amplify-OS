// ============================================================================
// iso9660.c — ISO9660 + Rock Ridge только на чтение. Контракт — в iso9660.h.
// ----------------------------------------------------------------------------
// Раскладка: сектор 2048 байт; Primary Volume Descriptor — в секторе 16
// (тип 1, «CD001»), в нём по смещению 156 — запись корневого каталога.
// Запись каталога: [0] длина, [2..5] LBA экстента (LE), [10..13] размер (LE),
// [25] флаги (бит 1 — каталог), [32] длина имени, [33..] имя, затем System
// Use Area с записями SUSP. Длинное имя Rock Ridge — запись «NM» (grub-mkrescue
// собирает ISO через xorriso с Rock Ridge, ISO-имя там — урезанное 8.3).
// ============================================================================
#include "iso9660.h"
#include "ata.h"
#include "serial.h"

static int         g_dev = -1;
static bool        g_tried;
static iso_entry_t g_root;
static uint8_t     g_sec[2048];
static int64_t     g_sec_lba = -1;

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t* sector(uint32_t lba) {
    if (g_sec_lba != (int64_t)lba) {
        if (!ata_atapi_read(g_dev, lba, 1, g_sec)) { g_sec_lba = -1; return 0; }
        g_sec_lba = lba;
    }
    return g_sec;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool name_eq(const char* a, const char* b, int blen) {
    int i = 0;
    for (; i < blen; i++) {
        if (!a[i] || lower(a[i]) != lower(b[i])) return false;
    }
    return a[i] == 0;
}

// Имя записи: Rock Ridge NM, иначе ISO-имя без версии «;1» и хвостовой точки.
static void rec_name(const uint8_t* r, char* out, int cap) {
    uint8_t len = r[0], nl = r[32];
    const uint8_t* nm = r + 33;
    int o = 0;
    bool rr = false;
    int su = 33 + nl + ((nl & 1) ? 0 : 1);          // выравнивание на чётный байт
    while (su + 4 <= len) {
        const uint8_t* e = r + su;
        uint8_t el = e[2];
        if (el < 4 || su + el > len) break;
        if (e[0] == 'N' && e[1] == 'M' && el >= 5) {
            // e[4] — флаги (0x01 CONTINUE: имя продолжится в следующей NM).
            for (int k = 5; k < el && o + 1 < cap; k++) out[o++] = (char)e[k];
            rr = true;
        }
        su += el;
    }
    if (rr) { out[o] = 0; return; }
    for (int k = 0; k < nl && nm[k] != ';' && o + 1 < cap; k++) out[o++] = (char)nm[k];
    while (o > 0 && out[o - 1] == '.') o--;
    out[o] = 0;
}

typedef int (*rec_fn)(const char* name, const iso_entry_t* e, void* user);

static int dir_iter(const iso_entry_t* dir, rec_fn fn, void* user) {
    static uint8_t buf[2048];
    static char name[256];
    uint32_t nsec = (dir->size + 2047) / 2048;
    for (uint32_t s = 0; s < nsec; s++) {
        const uint8_t* b = sector(dir->lba + s);
        if (!b) return -1;
        for (int i = 0; i < 2048; i++) buf[i] = b[i];   // cb может читать другие сектора
        uint32_t off = 0;
        while (off < 2048) {
            uint8_t len = buf[off];
            if (len == 0 || off + len > 2048) break;       // остаток сектора — заполнитель
            const uint8_t* r = buf + off;
            off += len;
            if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;   // «.» и «..»
            iso_entry_t e;
            e.lba = le32(r + 2);
            e.size = le32(r + 10);
            e.is_dir = (r[25] & 0x02) != 0;
            rec_name(r, name, (int)sizeof(name));
            int rc = fn(name, &e, user);
            if (rc) return rc;
        }
    }
    return 0;
}

typedef struct {
    const char*  want;
    int          want_len;
    iso_entry_t* out;
} find_ctx_t;

static int find_cb(const char* name, const iso_entry_t* e, void* user) {
    find_ctx_t* c = (find_ctx_t*)user;
    if (!name_eq(name, c->want, c->want_len)) return 0;
    *c->out = *e;
    return 1;
}

bool iso_lookup(const char* path, iso_entry_t* out) {
    if (g_dev < 0 || !path) return false;
    iso_entry_t cur = g_root;
    const char* p = path;
    while (*p == '/') p++;
    while (*p) {
        const char* s = p;
        while (*p && *p != '/') p++;
        int n = (int)(p - s);
        while (*p == '/') p++;
        if (n == 0) continue;
        if (!cur.is_dir) return false;
        iso_entry_t next;
        find_ctx_t c = { s, n, &next };
        if (dir_iter(&cur, find_cb, &c) != 1) return false;
        cur = next;
    }
    *out = cur;
    return true;
}

int iso_list(const char* path, iso_dir_cb cb, void* user) {
    iso_entry_t d;
    if (!iso_lookup(path, &d) || !d.is_dir) return -1;
    int rc = dir_iter(&d, (rec_fn)cb, user);
    return rc < 0 ? -1 : 0;
}

int64_t iso_read(const iso_entry_t* f, uint64_t off, void* buf, uint64_t len) {
    if (g_dev < 0 || !f || f->is_dir) return -1;
    if (off >= f->size) return 0;
    if (off + len > f->size) len = f->size - off;
    uint8_t* dst = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lba = f->lba + (uint32_t)(pos / 2048);
        uint32_t in = (uint32_t)(pos % 2048);
        uint64_t left = len - done;
        if (in == 0 && left >= 2048) {
            // Целые сектора — прямо в буфер вызывающего (ata режет по 32).
            uint32_t n = (uint32_t)(left / 2048);
            if (n > 512) n = 512;
            if (!ata_atapi_read(g_dev, lba, n, dst + done)) return -1;
            done += (uint64_t)n * 2048;
            continue;
        }
        const uint8_t* s = sector(lba);
        if (!s) return -1;
        uint64_t take = 2048 - in;
        if (take > left) take = left;
        for (uint64_t k = 0; k < take; k++) dst[done + k] = s[in + k];
        done += take;
    }
    return (int64_t)done;
}

static bool try_device(int dev) {
    g_dev = dev;
    g_sec_lba = -1;
    const uint8_t* pvd = sector(16);
    if (!pvd || pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' ||
        pvd[4] != '0' || pvd[5] != '1') {
        g_dev = -1;
        return false;
    }
    g_root.lba = le32(pvd + 156 + 2);
    g_root.size = le32(pvd + 156 + 10);
    g_root.is_dir = true;
    iso_entry_t k;
    if (!iso_lookup("/boot/kernel-x86_64.bin", &k)) { g_dev = -1; return false; }
    return true;
}

bool iso_mount_boot(void) {
    if (g_tried) return g_dev >= 0;
    g_tried = true;
    for (int i = 0; i < ata_device_count(); i++) {
        const ata_info_t* d = ata_device(i);
        if (!d || !d->present || !d->atapi) continue;
        if (try_device(i)) {
            serial_puts("[iso] AMPLIFY-OS medium on ");
            serial_puts(ata_device_bus_name(i));
            serial_puts("\n");
            return true;
        }
    }
    serial_puts("[iso] no AMPLIFY-OS installation medium found\n");
    return false;
}

bool iso_present(void) { return g_dev >= 0; }
int  iso_device(void) { return g_dev; }
