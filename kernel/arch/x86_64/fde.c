// ============================================================================
// fde.c — системный зашифрованный том: поиск, разблокировка, самотест
// (этап 48).  Формат — luks.c, шифрование секторов — blkcrypt.c.
// ============================================================================
#include "fde.h"
#include "luks.h"
#include "blkcrypt.h"
#include "disk_io.h"
#include "partition.h"
#include "fat32.h"
#include "ata.h"
#include "crypto.h"
#include "serial.h"

static struct {
    bool        found;        // на диске есть LUKS-раздел системы
    bool        unlocked;
    int         dev;
    uint64_t    lba;          // начало раздела
    uint64_t    sectors;      // размер раздела целиком (с заголовком)
    luks_info_t info;
    int         vdev;         // blkcrypt-отображение после разблокировки
    int         slot;         // каким слотом открыли
} g_fde = { .dev = -1, .vdev = -1, .slot = -1 };

static char g_sso[64];
static bool g_sso_valid = false;

// ----------------------------------------------------------------------------
static int  s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int  s_append(char* b, int p, int cap, const char* s) {
    for (int i = 0; s && s[i] && p < cap - 1; i++) b[p++] = s[i];
    b[p] = 0;
    return p;
}
static int  s_append_u64(char* b, int p, int cap, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < cap - 1) b[p++] = t[--n];
    b[p] = 0;
    return p;
}
static int  s_append_size(char* b, int p, int cap, uint64_t sectors) {
    uint64_t mb = sectors / 2048;
    if (mb < 1024) { p = s_append_u64(b, p, cap, mb); return s_append(b, p, cap, " MB"); }
    p = s_append_u64(b, p, cap, mb / 1024);
    uint64_t tenth = ((mb % 1024) * 10) / 1024;
    if (tenth) { p = s_append(b, p, cap, "."); p = s_append_u64(b, p, cap, tenth); }
    return s_append(b, p, cap, " GB");
}

// ----------------------------------------------------------------------------
void fde_scan(void) {
    if (fat32_is_mounted() || g_fde.found) return;
    for (int dev = 0; dev < DISK_ATA_DEVICES; dev++) {
        if (!disk_present_dev(dev)) continue;
        uint64_t lba = 0, sectors = 0;
        if (!part_find_luks(dev, &lba, &sectors)) continue;
        if (!luks_probe(dev, lba, &g_fde.info)) continue;

        serial_puts("[fde] LUKS1 volume on dev ");
        serial_print_dec((uint64_t)dev);
        serial_puts(" @ lba ");
        serial_print_dec(lba);
        serial_puts(" uuid=");
        serial_puts(g_fde.info.uuid);
        serial_puts(" cipher=");
        serial_puts(g_fde.info.cipher);
        serial_puts(" slots=");
        serial_print_dec((uint64_t)g_fde.info.active_slots);
        if (!g_fde.info.supported) {
            serial_puts(" — unsupported parameters, ignored\n");
            continue;
        }
        serial_puts(" — system volume is LOCKED\n");
        g_fde.found   = true;
        g_fde.dev     = dev;
        g_fde.lba     = lba;
        g_fde.sectors = sectors;
        return;
    }
}

bool fde_pending(void)  { return g_fde.found && !g_fde.unlocked; }
bool fde_unlocked(void) { return g_fde.unlocked; }

const char* fde_status_str(void) {
    if (!g_fde.found)   return "not encrypted";
    return g_fde.unlocked ? "unlocked" : "locked";
}

const char* fde_volume_label(void) {
    static char label[96];
    int p = 0;
    const ata_info_t* d = ata_device(g_fde.dev);
    p = s_append(label, p, (int)sizeof(label), (d && d->model[0]) ? d->model : "Disk");
    p = s_append(label, p, (int)sizeof(label), "  \xB7  system partition  \xB7  ");
    p = s_append_size(label, p, (int)sizeof(label), g_fde.sectors);
    return label;
}

int fde_unlock(const char* passphrase) {
    if (!g_fde.found) return LUKS_IO_ERROR;
    if (g_fde.unlocked) return LUKS_OK;
    if (!passphrase || !passphrase[0]) return LUKS_BAD_PASS;

    uint8_t mk[LUKS_KEY_BYTES];
    int slot = -1;
    int r = luks_open(g_fde.dev, g_fde.lba, passphrase, mk, &slot);
    if (r != LUKS_OK) {
        serial_puts(r == LUKS_BAD_PASS ? "[fde] wrong passphrase\n" : "[fde] unlock I/O error\n");
        return r;
    }

    uint64_t off = g_fde.info.payload_offset;
    int vdev = blkcrypt_map("system", g_fde.dev, g_fde.lba + off, g_fde.sectors - off, mk);
    crypto_wipe(mk, sizeof(mk));
    if (vdev < 0) return LUKS_IO_ERROR;

    if (fat32_mount_volume(vdev, 0) != 0) {
        serial_puts("[fde] unlocked, but the payload is not a FAT32 volume\n");
        blkcrypt_unmap(vdev);
        return LUKS_IO_ERROR;
    }

    g_fde.vdev     = vdev;
    g_fde.slot     = slot;
    g_fde.unlocked = true;

    int n = s_len(passphrase);
    if (n < (int)sizeof(g_sso)) {
        for (int i = 0; i <= n; i++) g_sso[i] = passphrase[i];
        g_sso_valid = true;
    }
    serial_puts("[fde] system volume unlocked, /disk mounted from vdev ");
    serial_print_dec((uint64_t)vdev);
    serial_puts("\n");
    return LUKS_OK;
}

bool fde_take_sso(char* out, int cap) {
    if (!g_sso_valid || !out || cap <= 0) return false;
    int n = s_len(g_sso);
    bool ok = n < cap;
    if (ok) for (int i = 0; i <= n; i++) out[i] = g_sso[i];
    crypto_wipe(g_sso, sizeof(g_sso));
    g_sso_valid = false;
    return ok;
}

int fde_change_passphrase(const char* old_pass, const char* new_pass) {
    if (!g_fde.unlocked) return LUKS_IO_ERROR;
    if (!new_pass || s_len(new_pass) < 4) return LUKS_BAD_PASS;
    uint8_t mk[LUKS_KEY_BYTES];
    int old_slot = -1;
    int r = luks_open(g_fde.dev, g_fde.lba, old_pass, mk, &old_slot);
    if (r != LUKS_OK) return r;
    int new_slot = luks_add_key(g_fde.dev, g_fde.lba, mk, new_pass, FDE_ITER_MS);
    crypto_wipe(mk, sizeof(mk));
    if (new_slot < 0) return LUKS_IO_ERROR;
    // Старый слот уничтожаем только после того, как новый записан: сбой
    // посередине оставляет ОБА пароля рабочими, а не ни одного.
    if (luks_kill_slot(g_fde.dev, g_fde.lba, old_slot) != 0) return LUKS_IO_ERROR;
    luks_probe(g_fde.dev, g_fde.lba, &g_fde.info);
    g_fde.slot = new_slot;
    return LUKS_OK;
}

void fde_describe(char* out, int cap) {
    int p = 0;
    out[0] = 0;
    if (!g_fde.found) {
        p = s_append(out, p, cap, "System volume: not encrypted\n");
    } else {
        p = s_append(out, p, cap, "System volume: ");
        p = s_append(out, p, cap, fde_status_str());
        p = s_append(out, p, cap, "\n  device    : dev ");
        p = s_append_u64(out, p, cap, (uint64_t)g_fde.dev);
        p = s_append(out, p, cap, " @ lba ");
        p = s_append_u64(out, p, cap, g_fde.lba);
        p = s_append(out, p, cap, " (");
        p = s_append_size(out, p, cap, g_fde.sectors);
        p = s_append(out, p, cap, ")\n  format    : LUKS1, ");
        p = s_append(out, p, cap, g_fde.info.cipher);
        p = s_append(out, p, cap, ", ");
        p = s_append(out, p, cap, g_fde.info.hash);
        p = s_append(out, p, cap, ", 512-bit key\n  uuid      : ");
        p = s_append(out, p, cap, g_fde.info.uuid);
        p = s_append(out, p, cap, "\n  payload   : +");
        p = s_append_u64(out, p, cap, g_fde.info.payload_offset);
        p = s_append(out, p, cap, " sectors\n");
        for (int s = 0; s < LUKS_NUMKEYS; s++) {
            if (!g_fde.info.slot_iter[s]) continue;
            p = s_append(out, p, cap, "  key slot ");
            p = s_append_u64(out, p, cap, (uint64_t)s);
            p = s_append(out, p, cap, ": ENABLED, PBKDF2 ");
            p = s_append_u64(out, p, cap, g_fde.info.slot_iter[s]);
            p = s_append(out, p, cap, " iterations");
            if (g_fde.unlocked && s == g_fde.slot) p = s_append(out, p, cap, " (used at boot)");
            p = s_append(out, p, cap, "\n");
        }
    }
    for (int i = 0; i < DISK_VDEV_CRYPT_MAX; i++) {
        blkcrypt_info_t bi;
        if (!blkcrypt_info(i, &bi)) continue;
        p = s_append(out, p, cap, "Mapping vdev ");
        p = s_append_u64(out, p, cap, (uint64_t)bi.vdev);
        p = s_append(out, p, cap, " '");
        p = s_append(out, p, cap, bi.name);
        p = s_append(out, p, cap, "': ");
        p = s_append_u64(out, p, cap, bi.reads);
        p = s_append(out, p, cap, " sectors decrypted, ");
        p = s_append_u64(out, p, cap, bi.writes);
        p = s_append(out, p, cap, " encrypted\n");
    }
}

// ----------------------------------------------------------------------------
// Self-test на RAM-диске.
// ----------------------------------------------------------------------------
static bool check(const char* what, bool ok) {
    serial_puts("[smoke 48]   ");
    serial_puts(ok ? "PASS " : "FAIL ");
    serial_puts(what);
    serial_puts("\n");
    return ok;
}

bool fde_self_test(void) {
    const uint64_t secs = LUKS_PAYLOAD_OFFSET + 2048 + 64;       // заголовок + ~1 МиБ данных
    int dev = disk_ramdisk_create(secs);
    if (dev < 0) return check("RAM disk for the self-test", false);

    bool all = true;
    uint8_t mk[LUKS_KEY_BYTES], mk2[LUKS_KEY_BYTES];
    luks_info_t info;
    int slot = -1;

    all &= check("luks_format (1000 iterations)",
                 luks_format(dev, 0, secs, "amplify-selftest", 0, mk, 0, 0) == LUKS_OK);
    all &= check("luks_probe: LUKS1 aes-xts-plain64/sha256, 1 slot, payload 4096",
                 luks_probe(dev, 0, &info) && info.supported && info.active_slots == 1 &&
                 info.payload_offset == LUKS_PAYLOAD_OFFSET && info.key_bytes == 64);
    all &= check("wrong passphrase rejected",
                 luks_open(dev, 0, "not-the-passphrase", mk2, &slot) == LUKS_BAD_PASS);
    all &= check("right passphrase recovers the master key",
                 luks_open(dev, 0, "amplify-selftest", mk2, &slot) == LUKS_OK && slot == 0 &&
                 crypto_eq_const(mk, mk2, LUKS_KEY_BYTES));

    int vdev = blkcrypt_map("selftest", dev, LUKS_PAYLOAD_OFFSET, secs - LUKS_PAYLOAD_OFFSET, mk);
    all &= check("blkcrypt mapping", vdev >= 0);
    if (vdev >= 0) {
        static uint8_t pt[4 * 512], back[4 * 512];
        for (int i = 0; i < (int)sizeof(pt); i++) pt[i] = (uint8_t)(i * 7 + 3);
        // Сектора 10 и 11 — одинаковый plaintext: у XTS шифротексты обязаны
        // различаться (tweak = номер сектора).
        for (int i = 0; i < 512; i++) pt[512 + i] = pt[i];
        bool wr = disk_write_sectors_dev(vdev, 10, 4, pt);
        bool rd = disk_read_sectors_dev(vdev, 10, 4, back);
        all &= check("write/read through the mapping round-trips",
                     wr && rd && crypto_eq_const(pt, back, sizeof(pt)));

        const uint8_t* raw = disk_ramdisk_raw() + (LUKS_PAYLOAD_OFFSET + 10) * 512;
        bool differs = !crypto_eq_const(raw, pt, 512);
        bool tweaked = !crypto_eq_const(raw, raw + 512, 512);
        all &= check("ciphertext on the raw device differs from plaintext", differs);
        all &= check("identical sectors encrypt differently (XTS tweak)", tweaked);
        blkcrypt_unmap(vdev);
    }

    all &= check("add a second key slot", luks_add_key(dev, 0, mk, "second-passphrase", 0) == 1);
    all &= check("second passphrase opens slot 1",
                 luks_open(dev, 0, "second-passphrase", mk2, &slot) == LUKS_OK && slot == 1);
    all &= check("destroy slot 0", luks_kill_slot(dev, 0, 0) == 0);
    all &= check("old passphrase no longer opens the volume",
                 luks_open(dev, 0, "amplify-selftest", mk2, &slot) == LUKS_BAD_PASS);
    all &= check("the last key slot cannot be destroyed", luks_kill_slot(dev, 0, 1) != 0);

    crypto_wipe(mk, sizeof(mk));
    crypto_wipe(mk2, sizeof(mk2));
    disk_ramdisk_destroy();
    return all;
}
