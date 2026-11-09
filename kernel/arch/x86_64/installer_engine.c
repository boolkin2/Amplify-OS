// ============================================================================
// installer_engine.c — реальная установка AMPLIFY-OS на диск (этап 57).
// ----------------------------------------------------------------------------
// Payload берём из initrd: host-сборка кладёт туда `kernel-x86_64.bin` и
// `BOOTX64.EFI`, а сам образ initrd копируется на ESP целиком.  Благодаря
// этому установщику не нужен доступ к ISO9660 — всё уже в памяти.
// ============================================================================
#include "installer_engine.h"

#include "ata.h"
#include "disk_io.h"
#include "partition.h"
#include "fat32.h"
#include "fat32_mkfs.h"
#include "initrd.h"
#include "luks.h"
#include "blkcrypt.h"
#include "fde.h"
#include "crypto.h"
#include "users.h"
#include "ns.h"
#include "ui_theme.h"
#include "serial.h"
#include "timer.h"
#include "iso9660.h"
#include "mind_model.h"
#include "phantom.h"
#include "pmm.h"
#include "memory.h"

// ----------------------------------------------------------------------------
// Состояние.
// ----------------------------------------------------------------------------
static install_target_t g_targets[INSTALL_MAX_TARGETS];
static int              g_target_count = 0;
static char             g_error[128]   = {0};

// Прогресс текущего запуска — нужен, чтобы пробросить callback в mkfs.
static install_progress_cb g_cb      = 0;
static void*               g_cb_user = 0;
static install_step_t      g_cb_step = INSTALL_STEP_PLAN;

// [этап 48] Куда писать системный том.  Без шифрования — сам раздел
// (dev, sys_lba); с шифрованием — виртуальное устройство blkcrypt поверх
// payload'а LUKS (vdev, 0).  Остальной код установщика этого не различает.
static int      g_sys_dev    = -1;
static uint64_t g_sys_base   = 0;
static int      g_vault_vdev = -1;

// ----------------------------------------------------------------------------
// Мелкие string-утилиты (kernel-mode, без libc).
// ----------------------------------------------------------------------------
static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void s_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void set_error(const char* msg) {
    s_copy(g_error, msg, (int)sizeof(g_error));
    serial_puts("[install] ERROR: ");
    serial_puts(g_error);
    serial_puts("\n");
}

static int append(char* buf, int pos, int cap, const char* s) {
    for (int i = 0; s[i] && pos < cap - 1; i++) buf[pos++] = s[i];
    buf[pos] = 0;
    return pos;
}

static int append_u64(char* buf, int pos, int cap, uint64_t v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && pos < cap - 1) buf[pos++] = tmp[--n];
    buf[pos] = 0;
    return pos;
}

static int append_hex64(char* buf, int pos, int cap, uint64_t v) {
    const char* hx = "0123456789abcdef";
    for (int shift = 60; shift >= 0 && pos < cap - 1; shift -= 4)
        buf[pos++] = hx[(v >> shift) & 0xF];
    buf[pos] = 0;
    return pos;
}

static void report(install_step_t step, int percent, const char* message) {
    if (!g_cb) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g_cb(step, percent, message, g_cb_user);
}

// Тик от fat32_mkfs → в общий прогресс текущего шага.
static void mkfs_tick(int percent, void* user) {
    (void)user;
    report(g_cb_step, percent, 0);
}

// Тик от luks_format (бенчмарк PBKDF2, AF-split, запись слота).
static void luks_tick(int percent, const char* message, void* user) {
    (void)user;
    report(g_cb_step, percent, message);
}

// ----------------------------------------------------------------------------
// Перечисление кандидатов.
// ----------------------------------------------------------------------------
int install_scan_targets(void) {
    g_target_count = 0;

    for (int dev = 0; dev < ATA_MAX_DEVICES && g_target_count < INSTALL_MAX_TARGETS; dev++) {
        const ata_info_t* info = ata_device(dev);
        if (!info || !info->present || info->atapi) continue;
        if (info->total_sectors < PART_MIN_DISK_SECTORS) continue;

        install_target_t* t = &g_targets[g_target_count];
        t->dev            = dev;
        t->total_sectors  = info->total_sectors;
        t->size_mb        = info->size_mb;
        t->bus            = ata_device_bus_name(dev);
        s_copy(t->model, info->model, (int)sizeof(t->model));
        if (!t->model[0]) s_copy(t->model, "Unknown drive", (int)sizeof(t->model));

        part_table_t tbl;
        t->has_partitions = false;
        t->amplify        = false;
        if (part_probe(dev, &tbl) == 0 && tbl.count > 0) {
            t->has_partitions = true;
            for (int i = 0; i < tbl.count; i++) {
                const char* n = tbl.entry[i].name;
                if (n[0] == 'A' && n[1] == 'M' && n[2] == 'P' && n[3] == 'L') t->amplify = true;
            }
        }
        g_target_count++;
    }

    serial_puts("[install] targets found: ");
    serial_print_dec((uint64_t)g_target_count);
    serial_puts("\n");
    return g_target_count;
}

int install_target_count(void) { return g_target_count; }

const install_target_t* install_target_at(int index) {
    if (index < 0 || index >= g_target_count) return 0;
    return &g_targets[index];
}

uint64_t install_required_mb(void) { return PART_MIN_DISK_SECTORS / 2048; }

const char* install_last_error(void) { return g_error[0] ? g_error : "No error"; }

const char* install_step_name(install_step_t step) {
    switch (step) {
        case INSTALL_STEP_PLAN:          return "Planning layout";
        case INSTALL_STEP_PARTITION:     return "Writing GPT";
        case INSTALL_STEP_FORMAT_ESP:    return "Formatting boot partition";
        case INSTALL_STEP_ENCRYPT:       return "Sealing system partition";
        case INSTALL_STEP_FORMAT_SYSTEM: return "Formatting system partition";
        case INSTALL_STEP_FORMAT_VAULT:  return "Formatting vault partition";
        case INSTALL_STEP_BOOTLOADER:    return "Installing bootloader";
        case INSTALL_STEP_KERNEL:        return "Copying kernel and initrd";
        case INSTALL_STEP_SYSTEM_TREE:   return "Creating system tree";
        case INSTALL_STEP_IDENTITY:      return "Writing identity";
        case INSTALL_STEP_MIND:          return "Copying the MIND model";
        case INSTALL_STEP_VERIFY:        return "Verifying installation";
        default:                         return "Working";
    }
}

void install_preview_layout(int dev, bool vault,
                            uint64_t* esp_mb, uint64_t* sys_mb, uint64_t* vault_mb) {
    if (esp_mb)   *esp_mb   = 0;
    if (sys_mb)   *sys_mb   = 0;
    if (vault_mb) *vault_mb = 0;

    const ata_info_t* info = ata_device(dev);
    if (!info || !info->present) return;

    part_layout_t l;
    if (part_plan(info->total_sectors, vault, &l) != 0) return;

    if (esp_mb)   *esp_mb   = l.esp_sectors   / 2048;
    if (sys_mb)   *sys_mb   = l.sys_sectors   / 2048;
    if (vault_mb) *vault_mb = l.vault ? l.vault_sectors / 2048 : 0;
}

// ----------------------------------------------------------------------------
// Копирование файла на смонтированный том.
// ----------------------------------------------------------------------------
static bool put_file(const char* path, const void* data, uint64_t size) {
    if (fat32_create_file(path, data, size) < 0) {
        serial_puts("[install] write failed: ");
        serial_puts(path);
        serial_puts("\n");
        return false;
    }
    serial_puts("[install]   + ");
    serial_puts(path);
    serial_puts(" (");
    serial_print_dec(size);
    serial_puts(" bytes)\n");
    return true;
}

// ----------------------------------------------------------------------------
// Шаг: ESP — загрузчик, ядро, initrd.
// ----------------------------------------------------------------------------
static bool populate_esp(const part_layout_t* l, int dev) {
    if (fat32_bind(dev, l->esp_lba) != 0) {
        set_error("Cannot mount the new boot partition");
        return false;
    }

    uint64_t efi_size = 0, kern_size = 0, initrd_sz = 0;
    const uint8_t* efi  = initrd_get_file("BOOTX64.EFI", &efi_size);
    const uint8_t* kern = initrd_get_file("kernel-x86_64.bin", &kern_size);
    const uint8_t* img  = initrd_image(&initrd_sz);

    if (!efi || !efi_size) {
        set_error("Boot payload BOOTX64.EFI is missing from the live image");
        return false;
    }
    if (!kern || !kern_size) {
        set_error("Kernel payload is missing from the live image");
        return false;
    }
    if (!img || !initrd_sz) {
        set_error("Initrd image is not available");
        return false;
    }

    report(INSTALL_STEP_BOOTLOADER, 10, "Creating /EFI/BOOT");
    if (fat32_mkdir_p("/EFI/BOOT") != 0) {
        set_error("Cannot create /EFI/BOOT on the boot partition");
        return false;
    }

    report(INSTALL_STEP_BOOTLOADER, 40, "Writing BOOTX64.EFI");
    if (!put_file("/EFI/BOOT/BOOTX64.EFI", efi, efi_size)) {
        set_error("Cannot write the UEFI bootloader");
        return false;
    }
    report(INSTALL_STEP_BOOTLOADER, 100, 0);

    report(INSTALL_STEP_KERNEL, 10, "Writing kernel.bin");
    if (!put_file("/kernel.bin", kern, kern_size)) {
        set_error("Cannot write kernel.bin");
        return false;
    }

    report(INSTALL_STEP_KERNEL, 60, "Writing initrd.img");
    if (!put_file("/initrd.img", img, initrd_sz)) {
        set_error("Cannot write initrd.img");
        return false;
    }
    report(INSTALL_STEP_KERNEL, 100, 0);
    return true;
}

// ----------------------------------------------------------------------------
// Шаг: системный раздел — дерево каталогов и конфигурация.
// ----------------------------------------------------------------------------
static bool populate_system(const part_layout_t* l, const install_config_t* cfg) {
    if (fat32_bind(g_sys_dev, g_sys_base) != 0) {
        set_error("Cannot mount the new system partition");
        return false;
    }

    static const char* dirs[] = {
        "/Sanctum", "/Sanctum/Amply", "/nexus", "/vault", "/forge", "/fonts"
    };
    const int dir_count = (int)(sizeof(dirs) / sizeof(dirs[0]));

    for (int i = 0; i < dir_count; i++) {
        report(INSTALL_STEP_SYSTEM_TREE, (i * 80) / dir_count, dirs[i]);
        int r = fat32_mkdir_p(dirs[i]);
        if (r != 0) {
            set_error("Cannot create the system directory tree");
            return false;
        }
    }

    // Конфиг Sanctum — читается загрузкой установленной системы.
    static char conf[512];
    int p = 0;
    p = append(conf, p, (int)sizeof(conf), "# AMPLIFY-OS system configuration\n");
    p = append(conf, p, (int)sizeof(conf), "hostname=");
    p = append(conf, p, (int)sizeof(conf), cfg->hostname[0] ? cfg->hostname : "amplify");
    p = append(conf, p, (int)sizeof(conf), "\ntheme=");
    p = append(conf, p, (int)sizeof(conf), cfg->dark ? "dark" : "light");
    p = append(conf, p, (int)sizeof(conf), "\nvault=");
    p = append(conf, p, (int)sizeof(conf), l->vault ? "partition" : "folder");
    p = append(conf, p, (int)sizeof(conf), "\nencryption=");
    p = append(conf, p, (int)sizeof(conf), cfg->encrypt ? "luks1-aes-xts-plain64" : "none");
    p = append(conf, p, (int)sizeof(conf), "\narch=x86_64\nfirmware=uefi\ninstalled=1\n");

    report(INSTALL_STEP_SYSTEM_TREE, 90, "/Sanctum/system.conf");
    if (!put_file("/Sanctum/system.conf", conf, (uint64_t)p)) {
        set_error("Cannot write the system configuration");
        return false;
    }

    // Оформление — в том же файле и формате, что пишет приложение Facet
    // (app_facet.c): строки "theme=<индекс акцента>" и "dark=<0|1>".  Так выбор,
    // сделанный в установщике, Facet покажет как текущий, а загрузка
    // установленной системы применит его (install_apply_system_config).
    report(INSTALL_STEP_SYSTEM_TREE, 85, "/Sanctum/system/ui-config.ini");
    if (fat32_mkdir_p("/Sanctum/system") != 0) {
        set_error("Cannot create /Sanctum/system");
        return false;
    }
    {
        static char ui[32];
        int up = 0;
        up = append(ui, up, (int)sizeof(ui), "theme=0\ndark=");
        up = append(ui, up, (int)sizeof(ui), cfg->dark ? "1" : "0");
        up = append(ui, up, (int)sizeof(ui), "\n");
        if (!put_file("/Sanctum/system/ui-config.ini", ui, (uint64_t)up)) {
            set_error("Cannot write the appearance settings");
            return false;
        }
    }

    // Маркер, по которому verify убеждается, что том читается обратно.
    static const char stamp[] = "AMPLIFY-OS installed\n";
    if (!put_file("/Sanctum/install.stamp", stamp, (uint64_t)(sizeof(stamp) - 1))) {
        set_error("Cannot write the install marker");
        return false;
    }
    report(INSTALL_STEP_SYSTEM_TREE, 100, 0);

    // Идентичность: users.txt в формате users.c — "<name>:<hash hex16>\n".
    report(INSTALL_STEP_IDENTITY, 20, "Creating the administrator account");
    if (cfg->username[0]) {
        static char udb[128];
        int  up = 0;
        up = append(udb, up, (int)sizeof(udb), cfg->username);
        up = append(udb, up, (int)sizeof(udb), ":");
        up = append_hex64(udb, up, (int)sizeof(udb), users_hash(cfg->passphrase));
        up = append(udb, up, (int)sizeof(udb), "\n");
        if (!put_file("/users.txt", udb, (uint64_t)up)) {
            set_error("Cannot write the user database");
            return false;
        }
    }
    report(INSTALL_STEP_IDENTITY, 100, 0);
    return true;
}

// ----------------------------------------------------------------------------
// Шаг: верификация.
// ----------------------------------------------------------------------------
static bool verify_install(int dev, const part_layout_t* l, const install_config_t* cfg) {
    report(INSTALL_STEP_VERIFY, 20, "Re-reading the partition table");

    part_table_t tbl;
    if (part_probe(dev, &tbl) != 0 || !tbl.gpt || tbl.count < 2) {
        set_error("The partition table did not read back correctly");
        return false;
    }

    // Загрузочный раздел: прошивка должна найти \EFI\BOOT\BOOTX64.EFI, а
    // наш загрузчик — \kernel.bin рядом с ним.  Сверяем размеры с payload'ом.
    report(INSTALL_STEP_VERIFY, 35, "Checking the boot partition");
    if (fat32_bind(dev, l->esp_lba) != 0) {
        set_error("The boot partition cannot be mounted");
        return false;
    }
    {
        static const char* files[] = { "/EFI/BOOT/BOOTX64.EFI", "/kernel.bin", "/initrd.img" };
        static const char* names[] = { "BOOTX64.EFI", "kernel-x86_64.bin", 0 };
        static uint8_t chunk[64 * 1024];
        for (int i = 0; i < 3; i++) {
            uint64_t expect = 0;
            const uint8_t* src = names[i] ? initrd_get_file(names[i], &expect)
                                          : initrd_image(&expect);
            report(INSTALL_STEP_VERIFY, 35 + i * 6, files[i]);

            // Побайтовое сравнение с payload'ом кусками по 64 КиБ: ловит и
            // обрезанный файл, и испорченную цепочку кластеров, и сбой записи.
            uint64_t size = 0;
            for (uint64_t off = 0; ; off += sizeof(chunk)) {
                int64_t n = fat32_read_file_at(files[i], off, chunk, sizeof(chunk), &size);
                if (n < 0 || size != expect) {
                    set_error("A boot file on the new boot partition is missing or truncated");
                    return false;
                }
                if (n == 0) break;
                for (int64_t k = 0; k < n; k++) {
                    if (chunk[k] != src[off + (uint64_t)k]) {
                        set_error("A boot file on the new boot partition does not match the source");
                        return false;
                    }
                }
                if (off + (uint64_t)n >= size) break;
            }
        }
    }

    report(INSTALL_STEP_VERIFY, 55, "Mounting the system partition");
    if (cfg->encrypt) {
        // Проверяем ровно то, что сделает загрузка: найти LUKS-раздел по GPT,
        // открыть его ПАРОЛЕМ (а не мастер-ключом из памяти установщика),
        // отобразить заново и смонтировать.
        uint64_t lba = 0, secs = 0;
        uint8_t  mk[LUKS_KEY_BYTES];
        if (!part_find_luks(dev, &lba, &secs) || lba != l->sys_lba) {
            set_error("The encrypted system partition was not found after writing");
            return false;
        }
        report(INSTALL_STEP_VERIFY, 60, "Unlocking the system partition with the passphrase");
        if (luks_open(dev, lba, cfg->passphrase, mk, 0) != LUKS_OK) {
            set_error("The new encrypted partition does not accept the passphrase");
            return false;
        }
        fat32_unbind();
        blkcrypt_unmap(g_sys_dev);
        g_sys_dev = blkcrypt_map("install-verify", dev, lba + LUKS_PAYLOAD_OFFSET,
                                 secs - LUKS_PAYLOAD_OFFSET, mk);
        crypto_wipe(mk, sizeof(mk));
        if (g_sys_dev < 0 || fat32_bind(g_sys_dev, 0) != 0) {
            set_error("The encrypted system partition cannot be mounted");
            return false;
        }
    } else {
        int64_t sys = part_find_system_fat32(dev);
        if (sys <= 0) {
            set_error("The system partition was not found after writing");
            return false;
        }
        if (fat32_bind(dev, (uint64_t)sys) != 0) {
            set_error("The system partition cannot be mounted");
            return false;
        }
    }

    report(INSTALL_STEP_VERIFY, 80, "Reading back the install marker");
    static char check[64];
    int64_t n = fat32_read_file("/Sanctum/install.stamp", check, sizeof(check) - 1);
    if (n <= 0) {
        set_error("The install marker cannot be read back");
        return false;
    }
    report(INSTALL_STEP_VERIFY, 100, 0);
    return true;
}

// ----------------------------------------------------------------------------
// Шаг: модель AMPLIFY MIND с установочного носителя.
// ----------------------------------------------------------------------------
// Веса лежат на ISO отдельными файлами /mind/*.gguf — в initrd им не место:
// initrd грузится в RAM на каждой загрузке. С этапа 49.1 (Qwen3-1.7B,
// 1.8 ГБ) файл копируется ПОТОКОВО: кусок с CD (ATAPI + ISO9660) ->
// fat32_stream_write на новый системный том (через blkcrypt, если он
// шифруется); в памяти — только буфер куска. Попутно считается 64-битная
// контрольная сумма, затем файл читается обратно с тома и сумма сверяется
// (раньше сверка шла побайтово с копией всего файла в RAM).
// Модель, которой этой машине не хватит памяти (оценка — как у MIND при
// выборе модели), не копируется: Qwen3-1.7B на ВМ с 1–2 ГБ бесполезна, а
// копирование 1.8 ГБ через IDE PIO заняло бы полчаса. Нет места на томе —
// тоже пропуск. MIND установленной системы сам выберет лучшую из
// скопированных. Нет носителя/моделей — шаг пропускается: система работает
// и со встроенной stories260K. Сбой записи или сверки — ошибка установки.
#define MIND_MAX_MODELS 4
#define MIND_COPY_CHUNK (4ull * 1024 * 1024)

typedef struct {
    char        name[96];
    iso_entry_t e;
} mind_file_t;

static mind_file_t g_mind_files[MIND_MAX_MODELS];
static int         g_mind_count;

static int mind_list_cb(const char* name, const iso_entry_t* e, void* user) {
    (void)user;
    int n = s_len(name);
    if (e->is_dir || n < 6 || n >= 96 || g_mind_count >= MIND_MAX_MODELS) return 0;
    const char* ext = name + n - 5;
    if (!(ext[0] == '.' && (ext[1] | 0x20) == 'g' && (ext[2] | 0x20) == 'g' &&
          (ext[3] | 0x20) == 'u' && (ext[4] | 0x20) == 'f')) return 0;
    s_copy(g_mind_files[g_mind_count].name, name, 96);
    g_mind_files[g_mind_count].e = *e;
    g_mind_count++;
    return 0;
}

// Контрольная сумма копии: 64-битный мультипликативный хэш по словам (не
// криптография — ловит сбои чтения/записи, а не злоумышленника).
static uint64_t sum_update(uint64_t h, const uint8_t* p, uint64_t n) {
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w = (uint64_t)p[i] | ((uint64_t)p[i + 1] << 8) | ((uint64_t)p[i + 2] << 16) |
                     ((uint64_t)p[i + 3] << 24) | ((uint64_t)p[i + 4] << 32) | ((uint64_t)p[i + 5] << 40) |
                     ((uint64_t)p[i + 6] << 48) | ((uint64_t)p[i + 7] << 56);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static bool copy_mind_models(void) {
    g_cb_step = INSTALL_STEP_MIND;
    g_mind_count = 0;
    if (!iso_mount_boot()) {
        report(INSTALL_STEP_MIND, 100, "No installation medium - skipped");
        return true;
    }
    (void)iso_list("/mind", mind_list_cb, 0);
    if (g_mind_count == 0) {
        report(INSTALL_STEP_MIND, 100, "No model on the medium - built-in model only");
        return true;
    }
    if (fat32_mkdir_p("/vault/mind") != 0) {
        set_error("Cannot create /vault/mind");
        return false;
    }
    const uint64_t cpages = MIND_COPY_CHUNK / PAGE_SIZE;
    uint64_t cphys = pmm_alloc_pages(cpages);
    if (!cphys) {
        report(INSTALL_STEP_MIND, 100, "Not enough RAM to copy the model - skipped");
        return true;
    }
    uint8_t* buf = (uint8_t*)(uintptr_t)cphys;
    static char path[128];
    static char msg[128];
    static fat32_stream_t st;
    bool ok = true;
    int copied = 0;
    for (int m = 0; m < g_mind_count && ok; m++) {
        mind_file_t* f = &g_mind_files[m];
        uint64_t size = f->e.size;
        uint64_t mb = size / (1024 * 1024);
        // Хватит ли этой машине памяти, чтобы модель запустить?
        uint64_t need = mind_model_estimate_bytes(size) + 256ull * 1024 * 1024;
        if (need > pmm_total_ram()) {
            int p = 0;
            p = append(msg, p, (int)sizeof(msg), "Skipped ");
            p = append(msg, p, (int)sizeof(msg), f->name);
            p = append(msg, p, (int)sizeof(msg), ": needs ");
            p = append_u64(msg, p, (int)sizeof(msg), need / (1024 * 1024));
            p = append(msg, p, (int)sizeof(msg), " MB of RAM");
            (void)p;
            report(INSTALL_STEP_MIND, 100 * (m + 1) / g_mind_count, msg);
            serial_puts("[install] mind model skipped (RAM): ");
            serial_puts(f->name);
            serial_puts("\n");
            continue;
        }
        int p = 0;
        p = append(path, p, (int)sizeof(path), "/vault/mind/");
        p = append(path, p, (int)sizeof(path), f->name);
        (void)p;
        if (fat32_stream_open(&st, path, size) != 0) {
            report(INSTALL_STEP_MIND, 100 * (m + 1) / g_mind_count, "Not enough space for the model - skipped");
            serial_puts("[install] mind model skipped (space): ");
            serial_puts(f->name);
            serial_puts("\n");
            continue;
        }
        // 1) Носитель -> том, по кусочку (0..70% шага этой модели).
        uint64_t h1 = 0x6A09E667F3BCC908ull;
        for (uint64_t off = 0; off < size && ok; off += MIND_COPY_CHUNK) {
            uint64_t n = size - off > MIND_COPY_CHUNK ? MIND_COPY_CHUNK : size - off;
            if (iso_read(&f->e, off, buf, n) != (int64_t)n) {
                set_error("Cannot read the MIND model from the installation medium");
                ok = false;
                break;
            }
            h1 = sum_update(h1, buf, n);
            if (fat32_stream_write(&st, buf, n) != (int64_t)n) {
                set_error("Cannot write the MIND model to the system partition");
                ok = false;
                break;
            }
            int q = 0;
            q = append(msg, q, (int)sizeof(msg), "Copying ");
            q = append(msg, q, (int)sizeof(msg), f->name);
            q = append(msg, q, (int)sizeof(msg), " ");
            q = append_u64(msg, q, (int)sizeof(msg), (off + n) / (1024 * 1024));
            q = append(msg, q, (int)sizeof(msg), " / ");
            q = append_u64(msg, q, (int)sizeof(msg), mb);
            q = append(msg, q, (int)sizeof(msg), " MB");
            (void)q;
            report(INSTALL_STEP_MIND, (int)((off + n) * 70 / size), msg);
        }
        if (!ok) { fat32_stream_abort(&st); break; }
        if (fat32_stream_close(&st) != 0) {
            set_error("Cannot finish writing the MIND model");
            ok = false;
            break;
        }
        // 2) Сверка: файл обратно с тома, та же сумма (70..100%).
        uint64_t h2 = 0x6A09E667F3BCC908ull;
        for (uint64_t off = 0; off < size; off += MIND_COPY_CHUNK) {
            uint64_t n = size - off > MIND_COPY_CHUNK ? MIND_COPY_CHUNK : size - off;
            uint64_t got_size = 0;
            int64_t r = fat32_read_file_at(path, off, buf, n, &got_size);
            if (r != (int64_t)n || got_size != size) {
                set_error("The copied MIND model is missing or truncated");
                ok = false;
                break;
            }
            h2 = sum_update(h2, buf, n);
            if ((off / MIND_COPY_CHUNK) % 4 == 0)
                report(INSTALL_STEP_MIND, 70 + (int)(off * 30 / size), "Verifying the model");
        }
        if (!ok) break;
        if (h1 != h2) {
            set_error("The copied MIND model does not match the medium");
            ok = false;
            break;
        }
        copied++;
        serial_puts("[install] mind model copied: ");
        serial_puts(path);
        serial_puts(" ");
        serial_print_dec(size);
        serial_puts(" bytes, verified (sum 0x");
        serial_print_hex(h1, 16);
        serial_puts(")\n");
    }
    pmm_free_pages(cphys, cpages);
    if (ok) report(INSTALL_STEP_MIND, 100, copied ? 0 : "No model fits this computer - built-in model only");
    return ok;
}

// ----------------------------------------------------------------------------
// [50.5] Образы гостей phantom с носителя: /guests/*.iso -> /vault/phantom.
// ----------------------------------------------------------------------------
// Чтобы у приложения Phantom в только что установленной системе было что
// запускать. Копируются тем же способом, что и модели (потоково, со сверкой
// суммы повторным чтением), но только если гостей на этой машине вообще можно
// запускать: без VT-x или второго ядра образы — мёртвый груз, а под TCG и на
// IDE-диске их копирование стоило бы минут. В отличие от моделей, сбой здесь
// установку не валит: недописанный образ удаляется, система ставится без него.
#define GUEST_MAX_IMAGES 4

static mind_file_t g_guest_files[GUEST_MAX_IMAGES];
static int         g_guest_count;

static int guest_list_cb(const char* name, const iso_entry_t* e, void* user) {
    (void)user;
    int n = s_len(name);
    if (e->is_dir || n < 5 || n >= 64 || g_guest_count >= GUEST_MAX_IMAGES) return 0;
    const char* ext = name + n - 4;
    if (!(ext[0] == '.' && (ext[1] | 0x20) == 'i' && (ext[2] | 0x20) == 's' && (ext[3] | 0x20) == 'o')) return 0;
    s_copy(g_guest_files[g_guest_count].name, name, 96);
    g_guest_files[g_guest_count].e = *e;
    g_guest_count++;
    return 0;
}

static void copy_guest_images(void) {
    static phantom_info_t pi;
    static char path[128];
    static char msg[128];
    static fat32_stream_t st;
    phantom_info(&pi);
    g_guest_count = 0;
    if (!iso_mount_boot()) return;
    (void)iso_list(PHANTOM_MEDIUM_DIR, guest_list_cb, 0);
    if (g_guest_count == 0) return;
    if (pi.unavailable[0]) {
        serial_puts("[install] guest images skipped: ");
        serial_puts(pi.unavailable);
        serial_puts("\n");
        return;
    }
    if (fat32_mkdir_p("/vault/phantom") != 0) {
        serial_puts("[install] guest images skipped: cannot create /vault/phantom\n");
        return;
    }
    const uint64_t cpages = MIND_COPY_CHUNK / PAGE_SIZE;
    uint64_t cphys = pmm_alloc_pages(cpages);
    if (!cphys) { serial_puts("[install] guest images skipped: no RAM for the copy buffer\n"); return; }
    uint8_t* buf = (uint8_t*)(uintptr_t)cphys;

    for (int m = 0; m < g_guest_count; m++) {
        mind_file_t* f = &g_guest_files[m];
        uint64_t size = f->e.size;
        int p = 0;
        p = append(path, p, (int)sizeof(path), "/vault/phantom/");
        p = append(path, p, (int)sizeof(path), f->name);
        (void)p;
        const char* why = 0;
        if (fat32_stream_open(&st, path, size) != 0) why = "no space";
        uint64_t h1 = 0x6A09E667F3BCC908ull, h2 = h1;
        for (uint64_t off = 0; !why && off < size; off += MIND_COPY_CHUNK) {
            uint64_t n = size - off > MIND_COPY_CHUNK ? MIND_COPY_CHUNK : size - off;
            if (iso_read(&f->e, off, buf, n) != (int64_t)n) { why = "read error on the medium"; break; }
            h1 = sum_update(h1, buf, n);
            if (fat32_stream_write(&st, buf, n) != (int64_t)n) { why = "write error"; break; }
            int q = 0;
            q = append(msg, q, (int)sizeof(msg), "Copying guest image ");
            q = append(msg, q, (int)sizeof(msg), f->name);
            q = append(msg, q, (int)sizeof(msg), " ");
            q = append_u64(msg, q, (int)sizeof(msg), (off + n) / (1024 * 1024));
            q = append(msg, q, (int)sizeof(msg), " / ");
            q = append_u64(msg, q, (int)sizeof(msg), size / (1024 * 1024));
            q = append(msg, q, (int)sizeof(msg), " MB");
            (void)q;
            report(INSTALL_STEP_MIND, 100, msg);
        }
        if (why && st.active) fat32_stream_abort(&st);
        if (!why && fat32_stream_close(&st) != 0) why = "cannot finish the file";
        for (uint64_t off = 0; !why && off < size; off += MIND_COPY_CHUNK) {
            uint64_t n = size - off > MIND_COPY_CHUNK ? MIND_COPY_CHUNK : size - off;
            if (fat32_read_file_at(path, off, buf, n, 0) != (int64_t)n) { why = "the copy is truncated"; break; }
            h2 = sum_update(h2, buf, n);
        }
        if (!why && h1 != h2) why = "the copy does not match the medium";
        if (why) {
            (void)fat32_delete_file(path);
            serial_puts("[install] guest image skipped ("); serial_puts(why); serial_puts("): ");
            serial_puts(f->name); serial_puts("\n");
            continue;
        }
        serial_puts("[install] guest image copied: ");
        serial_puts(path);
        serial_puts(" ");
        serial_print_dec(size);
        serial_puts(" bytes, verified (sum 0x");
        serial_print_hex(h1, 16);
        serial_puts(")\n");
    }
    pmm_free_pages(cphys, cpages);
    report(INSTALL_STEP_MIND, 100, 0);
}

// ----------------------------------------------------------------------------
// install_run
// ----------------------------------------------------------------------------
bool install_run(const install_config_t* cfg, install_progress_cb cb, void* user) {
    g_error[0] = 0;
    g_cb       = cb;
    g_cb_user  = user;

    if (!cfg) { set_error("No installation configuration"); return false; }

    const ata_info_t* info = ata_device(cfg->target_dev);
    if (!info || !info->present || info->atapi) {
        set_error("The selected drive is not available");
        g_cb = 0;
        return false;
    }

    // Запоминаем том живой системы, чтобы вернуть драйвер на место после
    // установки (вместе с флагом привязки к VFS).
    fat32_volume_t prev;
    fat32_save_volume(&prev);

    bool ok = false;
    part_layout_t layout;
    g_sys_dev    = -1;
    g_sys_base   = 0;
    g_vault_vdev = -1;

    do {
        // ---- 1. Планирование -------------------------------------------------
        report(INSTALL_STEP_PLAN, 20, "Measuring the drive");
        if (part_plan(info->total_sectors, cfg->vault, &layout) != 0) {
            set_error("The selected drive is too small for AMPLIFY-OS");
            break;
        }
        {
            static char msg[96];
            int p = 0;
            p = append(msg, p, (int)sizeof(msg), "Boot ");
            p = append_u64(msg, p, (int)sizeof(msg), layout.esp_sectors / 2048);
            p = append(msg, p, (int)sizeof(msg), " MB, system ");
            p = append_u64(msg, p, (int)sizeof(msg), layout.sys_sectors / 2048);
            p = append(msg, p, (int)sizeof(msg), " MB");
            report(INSTALL_STEP_PLAN, 100, msg);
        }
        layout.sys_luks = cfg->encrypt;

        // ---- 2. GPT ----------------------------------------------------------
        report(INSTALL_STEP_PARTITION, 30, "Erasing the old partition table");
        if (part_write_gpt(cfg->target_dev, info->total_sectors, &layout) != 0) {
            set_error("Cannot write the partition table");
            break;
        }
        report(INSTALL_STEP_PARTITION, 100, 0);

        // ---- 3. Форматирование ----------------------------------------------
        g_cb_step = INSTALL_STEP_FORMAT_ESP;
        report(INSTALL_STEP_FORMAT_ESP, 0, "AMPLIFY-BOOT");
        if (fat32_mkfs(cfg->target_dev, layout.esp_lba, layout.esp_sectors,
                       "AMPLIFYBOOT", mkfs_tick, 0) != 0) {
            set_error("Cannot format the boot partition");
            break;
        }

        // ---- 3a. [этап 48] LUKS1 на системном разделе -------------------------
        g_sys_dev  = cfg->target_dev;
        g_sys_base = layout.sys_lba;
        uint64_t sys_sectors = layout.sys_sectors;
        g_cb_step = INSTALL_STEP_ENCRYPT;
        if (cfg->encrypt) {
            uint8_t mk[LUKS_KEY_BYTES];
            if (luks_format(cfg->target_dev, layout.sys_lba, layout.sys_sectors,
                            cfg->passphrase, FDE_ITER_MS, mk, luks_tick, 0) != LUKS_OK) {
                set_error("Cannot create the LUKS header on the system partition");
                break;
            }
            int v = blkcrypt_map("install-system", cfg->target_dev,
                                 layout.sys_lba + LUKS_PAYLOAD_OFFSET,
                                 layout.sys_sectors - LUKS_PAYLOAD_OFFSET, mk);
            crypto_wipe(mk, sizeof(mk));
            if (v < 0) {
                set_error("Cannot map the encrypted system partition");
                break;
            }
            g_sys_dev   = v;
            g_sys_base  = 0;
            sys_sectors = layout.sys_sectors - LUKS_PAYLOAD_OFFSET;
            report(INSTALL_STEP_ENCRYPT, 100, "LUKS1 - aes-xts-plain64 - 512-bit key");
        } else {
            report(INSTALL_STEP_ENCRYPT, 100, "Not requested");
        }

        g_cb_step = INSTALL_STEP_FORMAT_SYSTEM;
        report(INSTALL_STEP_FORMAT_SYSTEM, 0, "AMPLIFY-SYSTEM");
        if (fat32_mkfs(g_sys_dev, g_sys_base, sys_sectors,
                       "AMPLIFY-OS", mkfs_tick, 0) != 0) {
            set_error("Cannot format the system partition");
            break;
        }

        if (layout.vault) {
            g_cb_step = INSTALL_STEP_FORMAT_VAULT;
            report(INSTALL_STEP_FORMAT_VAULT, 0, "AMPLIFY-VAULT");
            int      vdev  = cfg->target_dev;
            uint64_t vbase = layout.vault_lba, vsecs = layout.vault_sectors;
            if (cfg->encrypt) {
                // У vault — свой LUKS1 и свой мастер-ключ, пароль тот же.
                uint8_t mk[LUKS_KEY_BYTES];
                if (luks_format(cfg->target_dev, layout.vault_lba, layout.vault_sectors,
                                cfg->passphrase, FDE_ITER_MS, mk, luks_tick, 0) != LUKS_OK) {
                    set_error("Cannot create the LUKS header on the vault partition");
                    break;
                }
                g_vault_vdev = blkcrypt_map("install-vault", cfg->target_dev,
                                            layout.vault_lba + LUKS_PAYLOAD_OFFSET,
                                            layout.vault_sectors - LUKS_PAYLOAD_OFFSET, mk);
                crypto_wipe(mk, sizeof(mk));
                if (g_vault_vdev < 0) {
                    set_error("Cannot map the encrypted vault partition");
                    break;
                }
                vdev = g_vault_vdev; vbase = 0; vsecs -= LUKS_PAYLOAD_OFFSET;
            }
            if (fat32_mkfs(vdev, vbase, vsecs, "AMPLIFYVLT", mkfs_tick, 0) != 0) {
                set_error("Cannot format the vault partition");
                break;
            }
        } else {
            report(INSTALL_STEP_FORMAT_VAULT, 100, "Skipped");
        }

        // ---- 4-5. Содержимое -------------------------------------------------
        if (!populate_esp(&layout, cfg->target_dev)) break;
        if (!populate_system(&layout, cfg)) break;
        if (!copy_mind_models()) break;
        copy_guest_images();            // [50.5] довесок: его сбой установку не валит

        // ---- 6. Проверка -----------------------------------------------------
        if (!verify_install(cfg->target_dev, &layout, cfg)) break;

        // Всё записанное — из кеша диска на носитель.
        if (!disk_flush_dev(cfg->target_dev)) {
            set_error("The drive did not confirm the final cache flush");
            break;
        }

        ok = true;
    } while (0);

    // Возвращаем драйвер на исходный том живой системы.
    fat32_restore_volume(&prev);

    // Отображения установщика больше не нужны — ключи из памяти долой.
    if (g_sys_dev >= DISK_VDEV_CRYPT_BASE) blkcrypt_unmap(g_sys_dev);
    if (g_vault_vdev >= 0)                 blkcrypt_unmap(g_vault_vdev);
    g_sys_dev = -1;
    g_vault_vdev = -1;

    g_cb = 0;
    serial_puts(ok ? "[install] completed successfully\n"
                   : "[install] failed\n");
    return ok;
}

// ----------------------------------------------------------------------------
// Конфигурация установленной системы — применяется на каждой загрузке.
// ----------------------------------------------------------------------------
// Файлы пишет установщик (и позже Facet), читать их может только ядро,
// загрузившееся с установленного диска: в live-сессии том /disk — это
// либо dev-образ, либо ничего.
static bool conf_value(const char* buf, int len, const char* key,
                       char* out, int cap) {
    int kl = s_len(key);
    for (int i = 0; i < len; ) {
        int ls = i;
        while (i < len && buf[i] != '\n') i++;
        int le = i;
        if (i < len) i++;
        if (le - ls > kl && buf[ls + kl] == '=') {
            bool match = true;
            for (int k = 0; k < kl; k++) if (buf[ls + k] != key[k]) { match = false; break; }
            if (!match) continue;
            int o = 0;
            for (int k = ls + kl + 1; k < le && buf[k] != '\r' && o < cap - 1; k++) out[o++] = buf[k];
            out[o] = 0;
            return true;
        }
    }
    return false;
}

void install_apply_system_config(void) {
    if (!fat32_is_mounted()) return;
    static char buf[512];
    char val[INSTALL_HOSTNAME_MAX];

    // Логируем только первое применение — функция вызывается дважды за
    // загрузку (до splash и после desktop_init).
    static bool logged = false;

    int64_t n = fat32_read_file("/disk/Sanctum/system.conf", buf, sizeof(buf) - 1);
    if (n > 0 && conf_value(buf, (int)n, "hostname", val, (int)sizeof(val)) && val[0]) {
        ns_set_root_hostname(val);
        if (!logged) { serial_puts("[sysconf] hostname="); serial_puts(val); serial_puts("\n"); }
    }

    n = fat32_read_file("/disk/Sanctum/system/ui-config.ini", buf, sizeof(buf) - 1);
    if (n > 0 && conf_value(buf, (int)n, "dark", val, (int)sizeof(val))) {
        ui_theme_set_dark(val[0] == '1');
        if (!logged) serial_puts(val[0] == '1' ? "[sysconf] theme=dark\n" : "[sysconf] theme=light\n");
    }
    logged = true;
}
