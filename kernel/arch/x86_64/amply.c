// ============================================================================
// amply.c — пакетный менеджер AMPLIFY-OS «Amply» (этап 45).
// ----------------------------------------------------------------------------
// Реализация:
//
//   * парсинг и подпись формата .amf (см. amply.h);
//   * загрузка пакета из VFS (initrd) или из FAT32 (/disk/Amply/cache/...);
//   * граф зависимостей с обнаружением циклов через DFS-стек visited[];
//   * транзакционный install: если хотя бы один распакованный файл не
//     записался — откатываем все ранее распакованные;
//   * персистентная БД установленных в /disk/Sanctum/Amply/db.txt;
//   * чтение индекса репо из /amply_index.txt (формат INI-block-per-pkg);
//   * упрощённая подпись AMPLIFY-Sig-v1 на двух FNV-1a (без знания SECRET_SALT
//     злоумышленник не подделает signature[8..15]).
// ============================================================================
#include "amply.h"
#include "vfs.h"
#include "fat32.h"
#include "heap.h"
#include "memory.h"
#include "serial.h"
#include "users.h"   // users_hash — переиспользуем FNV-1a
#include "http.h"    // для будущего скачивания удалённых пакетов
#include "timer.h"

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Константы хранения.
// ----------------------------------------------------------------------------
#define AMPLY_DIR              "/disk/Sanctum/Amply"
#define AMPLY_DB_PATH          "/disk/Sanctum/Amply/db.txt"
#define AMPLY_REPO_URL_PATH    "/disk/Sanctum/Amply/repo.txt"
#define AMPLY_INDEX_INITRD     "amply_index.txt"

// «Секрет» для второго FNV-подписи.  В этапе 47 заменим на Ed25519-ключ.
// Само значение хранится прямо в ядре (для симметрии с initrd-индексом —
// злоумышленник, имеющий доступ к ядру, может прочитать; защита от
// случайных поломок и от пакетов без подписи).
static const uint64_t AMPLY_KEY_DEFAULT = 0x414D504C59475231ULL; // "AMPLYGR1"

// ----------------------------------------------------------------------------
// Минимальные libc-замены.
// ----------------------------------------------------------------------------
static int s_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int s_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void s_memcpy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void s_memset(void* d, int v, uint64_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (uint64_t i = 0; i < n; i++) dd[i] = (uint8_t)v;
}
static void s_strcpy(char* d, const char* s, int cap) {
    int i; for (i = 0; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void s_strcat(char* d, const char* s, int cap) {
    int n = s_strlen(d);
    int i; for (i = 0; n + i < cap - 1 && s[i]; i++) d[n + i] = s[i];
    d[n + i] = 0;
}
static bool s_contains(const char* hay, const char* needle) {
    if (!*needle) return true;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i + j] && needle[j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}
static void itoa(uint64_t v, char* buf) {
    char tmp[24]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0 && n < 23) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0; while (n > 0) buf[o++] = tmp[--n]; buf[o] = 0;
}

// ----------------------------------------------------------------------------
// FNV-1a 64-bit (с настраиваемым seed) — используется для двойной подписи .amf
// и для дельта-проверки целостности.
// ----------------------------------------------------------------------------
static uint64_t fnv1a64_seed(const void* data, uint64_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (uint64_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}
#define FNV_OFFSET 0xCBF29CE484222325ULL

// ----------------------------------------------------------------------------
// Простой лог-сборщик: append к out_log с проверкой capacity.
// ----------------------------------------------------------------------------
static void log_append(char* out_log, size_t cap, const char* s) {
    if (!out_log || cap == 0) return;
    int n = s_strlen(out_log);
    int i; for (i = 0; (size_t)(n + i) < cap - 1 && s[i]; i++) out_log[n + i] = s[i];
    out_log[n + i] = 0;
}
static void log_dec(char* out_log, size_t cap, uint64_t v) {
    char buf[24]; itoa(v, buf); log_append(out_log, cap, buf);
}

// ----------------------------------------------------------------------------
// Состояние пакетного менеджера в памяти.
// ----------------------------------------------------------------------------
static amply_installed_t   g_db[AMPLY_MAX_INSTALLED];
static int                 g_db_count = 0;

static amply_index_entry_t g_repo[AMPLY_MAX_INDEX];
static int                 g_repo_count = 0;

static char                g_repo_url[256] = {0};
static bool                g_initialized = false;

// Текущий путь обхода зависимостей (для обнаружения циклов).
static char                g_dep_stack[AMPLY_MAX_INSTALLED][AMPLY_NAME_LEN];
static int                 g_dep_depth = 0;

// ----------------------------------------------------------------------------
// БД: persist форматом каждой записи строкой "name|version|fileN|file...".
// ----------------------------------------------------------------------------
static void db_clear(void) { g_db_count = 0; s_memset(g_db, 0, sizeof(g_db)); }

static void db_persist(void) {
    char buf[16 * 1024];
    int p = 0;
    for (int i = 0; i < g_db_count && p < (int)sizeof(buf) - 256; i++) {
        amply_installed_t* r = &g_db[i];
        for (int k = 0; r->name[k] && p < (int)sizeof(buf) - 1; k++)    buf[p++] = r->name[k];
        if (p < (int)sizeof(buf) - 1) buf[p++] = '|';
        for (int k = 0; r->version[k] && p < (int)sizeof(buf) - 1; k++) buf[p++] = r->version[k];
        if (p < (int)sizeof(buf) - 1) buf[p++] = '|';
        char num[24]; itoa((uint64_t)r->n_files, num);
        for (int k = 0; num[k] && p < (int)sizeof(buf) - 1; k++)        buf[p++] = num[k];
        if (p < (int)sizeof(buf) - 1) buf[p++] = '\n';
        for (int f = 0; f < r->n_files; f++) {
            for (int k = 0; r->files[f][k] && p < (int)sizeof(buf) - 1; k++) buf[p++] = r->files[f][k];
            if (p < (int)sizeof(buf) - 1) buf[p++] = '\n';
        }
    }
    fat32_save_file(AMPLY_DB_PATH, buf, (uint64_t)p);
    serial_puts("[amply] db persisted ("); serial_print_dec(p); serial_puts(" bytes)\n");
}

static void db_load(void) {
    db_clear();
    char buf[16 * 1024];
    int64_t got = fat32_read_file(AMPLY_DB_PATH, buf, sizeof(buf) - 1);
    if (got <= 0) return;
    buf[got] = 0;

    int i = 0;
    while (i < (int)got) {
        // Сначала строка-заголовок name|version|n
        int line_end = i; while (line_end < (int)got && buf[line_end] != '\n') line_end++;
        if (line_end == i) { i++; continue; }
        if (g_db_count >= AMPLY_MAX_INSTALLED) break;

        amply_installed_t* r = &g_db[g_db_count++];
        s_memset(r, 0, sizeof(*r));

        int p1 = i;
        while (p1 < line_end && buf[p1] != '|') p1++;
        int p2 = p1 + 1;
        while (p2 < line_end && buf[p2] != '|') p2++;

        int n_name = p1 - i; if (n_name > AMPLY_NAME_LEN - 1) n_name = AMPLY_NAME_LEN - 1;
        for (int k = 0; k < n_name; k++) r->name[k] = buf[i + k];
        int n_ver = p2 - p1 - 1; if (n_ver > AMPLY_VER_LEN - 1) n_ver = AMPLY_VER_LEN - 1;
        for (int k = 0; k < n_ver; k++) r->version[k] = buf[p1 + 1 + k];

        // n_files
        int n_files = 0;
        for (int k = p2 + 1; k < line_end; k++)
            if (buf[k] >= '0' && buf[k] <= '9') n_files = n_files * 10 + (buf[k] - '0');
        if (n_files > AMPLY_MAX_FILES) n_files = AMPLY_MAX_FILES;
        r->n_files = n_files;

        i = line_end + 1;
        for (int f = 0; f < n_files && i < (int)got; f++) {
            int fend = i; while (fend < (int)got && buf[fend] != '\n') fend++;
            int fl = fend - i; if (fl > AMPLY_PATH_LEN - 1) fl = AMPLY_PATH_LEN - 1;
            for (int k = 0; k < fl; k++) r->files[f][k] = buf[i + k];
            i = fend + 1;
        }
    }
    serial_puts("[amply] db loaded "); serial_print_dec((uint64_t)g_db_count); serial_puts(" pkg(s)\n");
}

// ----------------------------------------------------------------------------
// Индекс репо (initrd /amply_index.txt).
// Формат: блоки [pkg], каждый со строками key=value.  Например:
//   [pkg]
//   name=welcome
//   version=1.0
//   amf=/amply_welcome.amf
//   summary=Приветственный набор
// ----------------------------------------------------------------------------
static void repo_load(void) {
    g_repo_count = 0; s_memset(g_repo, 0, sizeof(g_repo));

    int fd = vfs_open(AMPLY_INDEX_INITRD);
    if (fd < 0) {
        serial_puts("[amply] no /amply_index.txt in initrd (empty repo)\n");
        return;
    }

    static char buf[16 * 1024];
    int64_t got = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (got <= 0) return;
    buf[got] = 0;

    int i = 0;
    amply_index_entry_t* cur = 0;
    while (i < (int)got) {
        int eol = i; while (eol < (int)got && buf[eol] != '\n') eol++;
        // Trim CR.
        int line_len = eol - i;
        if (line_len > 0 && buf[i + line_len - 1] == '\r') line_len--;
        if (line_len > 0) {
            const char* line = buf + i;
            // [pkg] начинает новую запись.
            if (line[0] == '[' && line_len >= 5 && s_strncmp(line, "[pkg]", 5) == 0) {
                if (g_repo_count < AMPLY_MAX_INDEX) {
                    cur = &g_repo[g_repo_count++];
                    s_memset(cur, 0, sizeof(*cur));
                } else cur = 0;
            } else if (cur) {
                int eq = 0; while (eq < line_len && line[eq] != '=') eq++;
                if (eq < line_len) {
                    int kl = eq;
                    int vl = line_len - eq - 1;
                    const char* k = line;
                    const char* v = line + eq + 1;
                    if (kl == 4 && s_strncmp(k, "name", 4) == 0) {
                        int n = vl < AMPLY_NAME_LEN - 1 ? vl : AMPLY_NAME_LEN - 1;
                        for (int q = 0; q < n; q++) cur->name[q] = v[q];
                    } else if (kl == 7 && s_strncmp(k, "version", 7) == 0) {
                        int n = vl < AMPLY_VER_LEN - 1 ? vl : AMPLY_VER_LEN - 1;
                        for (int q = 0; q < n; q++) cur->version[q] = v[q];
                    } else if (kl == 3 && s_strncmp(k, "amf", 3) == 0) {
                        int n = vl < AMPLY_PATH_LEN - 1 ? vl : AMPLY_PATH_LEN - 1;
                        for (int q = 0; q < n; q++) cur->amf_path[q] = v[q];
                    } else if (kl == 7 && s_strncmp(k, "summary", 7) == 0) {
                        int n = vl < (int)sizeof(cur->summary) - 1 ? vl : (int)sizeof(cur->summary) - 1;
                        for (int q = 0; q < n; q++) cur->summary[q] = v[q];
                    }
                }
            }
        }
        i = eol + 1;
    }
    serial_puts("[amply] repo loaded "); serial_print_dec((uint64_t)g_repo_count);
    serial_puts(" entry(ies)\n");
}

// ----------------------------------------------------------------------------
// URL основного репо (по умолчанию пуст).
// ----------------------------------------------------------------------------
static void repo_url_load(void) {
    char buf[256] = {0};
    int64_t got = fat32_read_file(AMPLY_REPO_URL_PATH, buf, sizeof(buf) - 1);
    if (got > 0) {
        // Trim trailing newline/space.
        while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' ')) got--;
        buf[got] = 0;
        s_strcpy(g_repo_url, buf, sizeof(g_repo_url));
        serial_puts("[amply] repo url = "); serial_puts(g_repo_url); serial_puts("\n");
    }
}

const char* amply_repo_url(void) { return g_repo_url; }

int amply_set_repo_url(const char* url) {
    if (!url) return -1;
    s_strcpy(g_repo_url, url, sizeof(g_repo_url));
    return (int)fat32_save_file(AMPLY_REPO_URL_PATH, url, (uint64_t)s_strlen(url));
}

// ----------------------------------------------------------------------------
// Public init.
// ----------------------------------------------------------------------------
int amply_init(void) {
    if (g_initialized) return 0;
    g_initialized = true;

    // Создаём системный каталог.
    if (fat32_is_mounted()) {
        fat32_mkdir_p("/disk/Sanctum");
        fat32_mkdir_p(AMPLY_DIR);
    }

    repo_load();
    db_load();
    repo_url_load();

    serial_puts("[amply] initialized: db="); serial_print_dec((uint64_t)g_db_count);
    serial_puts(" repo="); serial_print_dec((uint64_t)g_repo_count); serial_puts("\n");
    return 0;
}

// ----------------------------------------------------------------------------
// Геттеры.
// ----------------------------------------------------------------------------
int amply_db_count(void) { return g_db_count; }
const amply_installed_t* amply_db_at(int i) {
    if (i < 0 || i >= g_db_count) return 0;
    return &g_db[i];
}

int amply_repo_count(void) { return g_repo_count; }
const amply_index_entry_t* amply_repo_at(int i) {
    if (i < 0 || i >= g_repo_count) return 0;
    return &g_repo[i];
}

const amply_installed_t* amply_db_find(const char* name) {
    for (int i = 0; i < g_db_count; i++)
        if (s_strcmp(g_db[i].name, name) == 0) return &g_db[i];
    return 0;
}

const amply_index_entry_t* amply_repo_find(const char* name) {
    for (int i = 0; i < g_repo_count; i++)
        if (s_strcmp(g_repo[i].name, name) == 0) return &g_repo[i];
    return 0;
}

// ----------------------------------------------------------------------------
// Парсер manifest'а: INI-блоки [package] и [depends].
// ----------------------------------------------------------------------------
static void parse_manifest(const char* m, uint32_t len, amply_manifest_t* out) {
    s_memset(out, 0, sizeof(*out));
    enum { S_NONE, S_PKG, S_DEPS } sect = S_NONE;
    uint32_t i = 0;
    while (i < len) {
        uint32_t eol = i;
        while (eol < len && m[eol] != '\n') eol++;
        int line_len = (int)(eol - i);
        if (line_len > 0 && m[i + line_len - 1] == '\r') line_len--;

        if (line_len == 0) { i = eol + 1; continue; }
        const char* line = m + i;

        if (line[0] == '[' && line_len >= 2) {
            if (line_len == 9 && s_strncmp(line, "[package]", 9) == 0) sect = S_PKG;
            else if (line_len == 9 && s_strncmp(line, "[depends]", 9) == 0) sect = S_DEPS;
            else sect = S_NONE;
        } else {
            int eq = 0; while (eq < line_len && line[eq] != '=') eq++;
            if (eq < line_len) {
                int kl = eq, vl = line_len - eq - 1;
                const char* k = line; const char* v = line + eq + 1;
                if (sect == S_PKG) {
                    if (kl == 4 && s_strncmp(k, "name", 4) == 0) {
                        int n = vl < AMPLY_NAME_LEN - 1 ? vl : AMPLY_NAME_LEN - 1;
                        for (int q = 0; q < n; q++) out->name[q] = v[q];
                    } else if (kl == 7 && s_strncmp(k, "version", 7) == 0) {
                        int n = vl < AMPLY_VER_LEN - 1 ? vl : AMPLY_VER_LEN - 1;
                        for (int q = 0; q < n; q++) out->version[q] = v[q];
                    } else if (kl == 7 && s_strncmp(k, "summary", 7) == 0) {
                        int n = vl < (int)sizeof(out->summary) - 1 ? vl : (int)sizeof(out->summary) - 1;
                        for (int q = 0; q < n; q++) out->summary[q] = v[q];
                    } else if (kl == 6 && s_strncmp(k, "author", 6) == 0) {
                        int n = vl < (int)sizeof(out->author) - 1 ? vl : (int)sizeof(out->author) - 1;
                        for (int q = 0; q < n; q++) out->author[q] = v[q];
                    }
                } else if (sect == S_DEPS) {
                    if (out->n_deps < AMPLY_MAX_DEPS) {
                        int n = kl < AMPLY_NAME_LEN - 1 ? kl : AMPLY_NAME_LEN - 1;
                        for (int q = 0; q < n; q++) out->deps[out->n_deps][q] = k[q];
                        out->n_deps++;
                    }
                }
            }
        }
        i = eol + 1;
    }
}

// ----------------------------------------------------------------------------
// amply_parse + verify.
// ----------------------------------------------------------------------------
const amply_file_entry_t* amply_parse(const void* data, uint64_t data_len,
                                      amply_header_t* out_hdr,
                                      amply_manifest_t* out_manifest,
                                      const char** err) {
    static const char* e_short = "amf truncated";
    static const char* e_magic = "bad magic";
    static const char* e_ver   = "unsupported version";
    static const char* e_size  = "size mismatch";

    if (data_len < sizeof(amply_header_t)) { if (err) *err = e_short; return 0; }
    const amply_header_t* hdr = (const amply_header_t*)data;
    if (hdr->magic != AMPLY_MAGIC)            { if (err) *err = e_magic; return 0; }
    if (hdr->version != AMPLY_VERSION)        { if (err) *err = e_ver;   return 0; }

    uint64_t need = sizeof(amply_header_t)
                  + hdr->manifest_len
                  + (uint64_t)hdr->file_count * sizeof(amply_file_entry_t)
                  + hdr->payload_len;
    if (data_len < need) { if (err) *err = e_size; return 0; }
    if (hdr->file_count > AMPLY_MAX_FILES) { if (err) *err = e_size; return 0; }

    const uint8_t* p = (const uint8_t*)data + sizeof(amply_header_t);
    if (out_hdr) *out_hdr = *hdr;
    if (out_manifest) parse_manifest((const char*)p, hdr->manifest_len, out_manifest);
    return (const amply_file_entry_t*)(p + hdr->manifest_len);
}

bool amply_verify(const void* data, uint64_t data_len, const char** err) {
    amply_header_t hdr;
    amply_manifest_t mf;
    const amply_file_entry_t* tbl = amply_parse(data, data_len, &hdr, &mf, err);
    if (!tbl) return false;

    // Считаем оба FNV по содержимому manifest+table+payload.
    const uint8_t* p = (const uint8_t*)data + sizeof(amply_header_t);
    uint64_t signed_len = (uint64_t)hdr.manifest_len
                        + (uint64_t)hdr.file_count * sizeof(amply_file_entry_t)
                        + hdr.payload_len;

    uint64_t h1 = fnv1a64_seed(p, signed_len, FNV_OFFSET);
    uint64_t h2 = fnv1a64_seed(p, signed_len, AMPLY_KEY_DEFAULT);

    uint64_t s1, s2;
    s_memcpy(&s1, hdr.signature + 0, 8);
    s_memcpy(&s2, hdr.signature + 8, 8);
    if (s1 != h1 || s2 != h2) {
        if (err) *err = "signature mismatch";
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Загрузить .amf по абсолютному пути в VFS (initrd) или FAT32.
// ----------------------------------------------------------------------------
void* amply_load_amf(const char* path, uint64_t* out_size, const char** err) {
    static const char* e_open = "amf not found";
    static const char* e_oom  = "out of memory";

    int fd = vfs_open(path);
    if (fd >= 0) {
        int64_t sz = vfs_size_fd(fd);
        if (sz <= 0) { vfs_close(fd); if (err) *err = e_open; return 0; }
        void* buf = kmalloc((size_t)sz);
        if (!buf) { vfs_close(fd); if (err) *err = e_oom; return 0; }
        int64_t got = vfs_read(fd, buf, (uint64_t)sz);
        vfs_close(fd);
        if (got != sz) { kfree(buf); if (err) *err = e_open; return 0; }
        if (out_size) *out_size = (uint64_t)sz;
        return buf;
    }

    // FAT32 fallback (для пакетов в /disk/Amply/cache).
    if (fat32_is_mounted()) {
        // Пробуем читать в kmalloc-буфер фиксированного потолка (1 MB достаточно
        // для всех пакетов этапа 45; в этапе 51 заменим на стрим с zstd).
        const size_t MAX_AMF = 1024 * 1024;
        void* tmp = kmalloc(MAX_AMF);
        if (!tmp) { if (err) *err = e_oom; return 0; }
        int64_t got = fat32_read_file(path, tmp, MAX_AMF);
        if (got > 0) {
            void* buf = kmalloc((size_t)got);
            if (!buf) { kfree(tmp); if (err) *err = e_oom; return 0; }
            s_memcpy(buf, tmp, (uint64_t)got);
            kfree(tmp);
            if (out_size) *out_size = (uint64_t)got;
            return buf;
        }
        kfree(tmp);
    }
    if (err) *err = e_open;
    return 0;
}

// ----------------------------------------------------------------------------
// Регистрация / удаление пакета в БД.
// ----------------------------------------------------------------------------
static int db_register(const amply_manifest_t* mf,
                       const amply_file_entry_t* tbl, uint32_t file_count) {
    if (g_db_count >= AMPLY_MAX_INSTALLED) return -1;
    amply_installed_t* r = &g_db[g_db_count++];
    s_memset(r, 0, sizeof(*r));
    s_strcpy(r->name, mf->name, AMPLY_NAME_LEN);
    s_strcpy(r->version, mf->version, AMPLY_VER_LEN);
    int n = (int)file_count > AMPLY_MAX_FILES ? AMPLY_MAX_FILES : (int)file_count;
    r->n_files = n;
    for (int i = 0; i < n; i++) s_strcpy(r->files[i], tbl[i].path, AMPLY_PATH_LEN);
    return 0;
}

static void db_unregister(const char* name) {
    for (int i = 0; i < g_db_count; i++) {
        if (s_strcmp(g_db[i].name, name) == 0) {
            for (int k = i; k < g_db_count - 1; k++) g_db[k] = g_db[k + 1];
            g_db_count--;
            s_memset(&g_db[g_db_count], 0, sizeof(g_db[0]));
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// Создать родительский каталог под /disk/X/Y/file.txt.
// ----------------------------------------------------------------------------
static int ensure_parent_dirs(const char* path) {
    char tmp[AMPLY_PATH_LEN]; s_strcpy(tmp, path, sizeof(tmp));
    int last = -1;
    for (int i = 0; tmp[i]; i++) if (tmp[i] == '/') last = i;
    if (last <= 0) return 0;
    tmp[last] = 0;
    return fat32_mkdir_p(tmp);
}

// ----------------------------------------------------------------------------
// Цикл-детект для install: проверка, что name не находится на стеке обхода.
// ----------------------------------------------------------------------------
static bool dep_stack_has(const char* name) {
    for (int i = 0; i < g_dep_depth; i++)
        if (s_strcmp(g_dep_stack[i], name) == 0) return true;
    return false;
}
static void dep_stack_push(const char* name) {
    if (g_dep_depth < AMPLY_MAX_INSTALLED)
        s_strcpy(g_dep_stack[g_dep_depth++], name, AMPLY_NAME_LEN);
}
static void dep_stack_pop(void) {
    if (g_dep_depth > 0) g_dep_depth--;
}

// ----------------------------------------------------------------------------
// install (рекурсивно с обнаружением циклов и журналом для отката).
// ----------------------------------------------------------------------------
static int install_one(const char* name, char* log, size_t logcap);

int amply_install(const char* name, char* out_log, size_t out_log_cap) {
    g_dep_depth = 0;
    int r = install_one(name, out_log, out_log_cap);
    if (r == 0) db_persist();
    return r;
}

static int install_one(const char* name, char* log, size_t logcap) {
    if (amply_db_find(name)) {
        log_append(log, logcap, "[skip] "); log_append(log, logcap, name);
        log_append(log, logcap, " already installed\n");
        return 0;
    }
    if (dep_stack_has(name)) {
        log_append(log, logcap, "[error] dependency cycle on ");
        log_append(log, logcap, name); log_append(log, logcap, "\n");
        return -10;
    }
    dep_stack_push(name);

    const amply_index_entry_t* idx = amply_repo_find(name);
    if (!idx) {
        log_append(log, logcap, "[error] no such package in repo: ");
        log_append(log, logcap, name); log_append(log, logcap, "\n");
        dep_stack_pop();
        return -1;
    }

    log_append(log, logcap, "[fetch] "); log_append(log, logcap, name);
    log_append(log, logcap, " "); log_append(log, logcap, idx->version);
    log_append(log, logcap, " from "); log_append(log, logcap, idx->amf_path);
    log_append(log, logcap, "\n");

    const char* err = 0;
    uint64_t sz = 0;
    void* amf = amply_load_amf(idx->amf_path, &sz, &err);
    if (!amf) {
        log_append(log, logcap, "[error] load: ");
        log_append(log, logcap, err ? err : "unknown");
        log_append(log, logcap, "\n");
        dep_stack_pop();
        return -2;
    }

    if (!amply_verify(amf, sz, &err)) {
        log_append(log, logcap, "[error] verify: ");
        log_append(log, logcap, err ? err : "unknown");
        log_append(log, logcap, "\n");
        kfree(amf);
        dep_stack_pop();
        return -3;
    }
    log_append(log, logcap, "[ok] signature valid (");
    log_dec(log, logcap, sz);
    log_append(log, logcap, " bytes)\n");

    amply_header_t hdr;
    amply_manifest_t mf;
    const amply_file_entry_t* tbl = amply_parse(amf, sz, &hdr, &mf, &err);
    if (!tbl) {
        log_append(log, logcap, "[error] parse: ");
        log_append(log, logcap, err ? err : "unknown"); log_append(log, logcap, "\n");
        kfree(amf); dep_stack_pop(); return -4;
    }

    // Зависимости (рекурсивно).
    for (int i = 0; i < mf.n_deps; i++) {
        log_append(log, logcap, "[dep] "); log_append(log, logcap, name);
        log_append(log, logcap, " -> "); log_append(log, logcap, mf.deps[i]);
        log_append(log, logcap, "\n");
        int dr = install_one(mf.deps[i], log, logcap);
        if (dr != 0) {
            kfree(amf); dep_stack_pop(); return dr;
        }
    }

    // Распаковка с журналом отката.
    const uint8_t* payload = (const uint8_t*)amf
                           + sizeof(amply_header_t)
                           + hdr.manifest_len
                           + (uint64_t)hdr.file_count * sizeof(amply_file_entry_t);

    int unpacked_count = 0;
    for (uint32_t i = 0; i < hdr.file_count; i++) {
        const amply_file_entry_t* fe = &tbl[i];
        if (fe->offset + fe->size > hdr.payload_len) {
            log_append(log, logcap, "[error] payload OOB at file ");
            log_append(log, logcap, fe->path); log_append(log, logcap, "\n");
            // Откат.
            for (int k = 0; k < unpacked_count; k++) fat32_delete_file(tbl[k].path);
            kfree(amf); dep_stack_pop(); return -5;
        }
        ensure_parent_dirs(fe->path);

        int64_t w = fat32_save_file(fe->path, payload + fe->offset, fe->size);
        if (w < 0 || (uint64_t)w != fe->size) {
            log_append(log, logcap, "[error] write failed: ");
            log_append(log, logcap, fe->path); log_append(log, logcap, "\n");
            for (int k = 0; k < unpacked_count; k++) fat32_delete_file(tbl[k].path);
            kfree(amf); dep_stack_pop(); return -6;
        }
        unpacked_count++;
        log_append(log, logcap, "[install] "); log_append(log, logcap, fe->path);
        log_append(log, logcap, " ("); log_dec(log, logcap, fe->size);
        log_append(log, logcap, " bytes)\n");
    }

    if (db_register(&mf, tbl, hdr.file_count) != 0) {
        log_append(log, logcap, "[error] db full\n");
        for (int k = 0; k < unpacked_count; k++) fat32_delete_file(tbl[k].path);
        kfree(amf); dep_stack_pop(); return -7;
    }

    log_append(log, logcap, "[done] "); log_append(log, logcap, mf.name);
    log_append(log, logcap, " "); log_append(log, logcap, mf.version);
    log_append(log, logcap, " installed\n");

    kfree(amf); dep_stack_pop();
    return 0;
}

// ----------------------------------------------------------------------------
// Remove.
// ----------------------------------------------------------------------------
int amply_remove(const char* name, char* out_log, size_t out_log_cap) {
    const amply_installed_t* r = amply_db_find(name);
    if (!r) {
        log_append(out_log, out_log_cap, "[error] not installed: ");
        log_append(out_log, out_log_cap, name); log_append(out_log, out_log_cap, "\n");
        return -1;
    }

    // Проверим, что никто не зависит от этого пакета (по индексу репо).
    for (int i = 0; i < g_db_count; i++) {
        if (s_strcmp(g_db[i].name, name) == 0) continue;
        const amply_index_entry_t* idx = amply_repo_find(g_db[i].name);
        if (!idx) continue;
        // Загружаем .amf и проверяем зависимости.  Дешевле кэшировать,
        // но для простоты этапа 45 делаем прямо.
        uint64_t sz = 0;
        void* amf = amply_load_amf(idx->amf_path, &sz, 0);
        if (!amf) continue;
        amply_header_t hdr; amply_manifest_t mf;
        if (amply_parse(amf, sz, &hdr, &mf, 0)) {
            for (int d = 0; d < mf.n_deps; d++) {
                if (s_strcmp(mf.deps[d], name) == 0) {
                    log_append(out_log, out_log_cap, "[error] ");
                    log_append(out_log, out_log_cap, g_db[i].name);
                    log_append(out_log, out_log_cap, " depends on ");
                    log_append(out_log, out_log_cap, name);
                    log_append(out_log, out_log_cap, "\n");
                    kfree(amf); return -2;
                }
            }
        }
        kfree(amf);
    }

    // Удаляем файлы.
    for (int i = 0; i < r->n_files; i++) {
        log_append(out_log, out_log_cap, "[remove] ");
        log_append(out_log, out_log_cap, r->files[i]);
        log_append(out_log, out_log_cap, "\n");
        fat32_delete_file(r->files[i]);
    }
    db_unregister(name);
    db_persist();
    log_append(out_log, out_log_cap, "[done] removed ");
    log_append(out_log, out_log_cap, name);
    log_append(out_log, out_log_cap, "\n");
    return 0;
}

// ----------------------------------------------------------------------------
// Сравнение версий.  Простое: лексикографическое по числам и точкам.
// 1.0 < 1.0.1 < 1.1 < 2.0
// ----------------------------------------------------------------------------
static int cmp_version(const char* a, const char* b) {
    while (*a || *b) {
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); b++; }
        if (va != vb) return va - vb;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return 1;
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return 0;
}

int amply_upgrade_all(char* out_log, size_t out_log_cap) {
    int upgraded = 0;
    for (int i = 0; i < g_db_count; i++) {
        const amply_index_entry_t* idx = amply_repo_find(g_db[i].name);
        if (!idx) continue;
        if (cmp_version(idx->version, g_db[i].version) > 0) {
            log_append(out_log, out_log_cap, "[upgrade] ");
            log_append(out_log, out_log_cap, g_db[i].name);
            log_append(out_log, out_log_cap, " ");
            log_append(out_log, out_log_cap, g_db[i].version);
            log_append(out_log, out_log_cap, " -> ");
            log_append(out_log, out_log_cap, idx->version);
            log_append(out_log, out_log_cap, "\n");
            // Сначала удаляем (без проверки depends — это была наша версия).
            char name_save[AMPLY_NAME_LEN];
            s_strcpy(name_save, g_db[i].name, AMPLY_NAME_LEN);
            for (int k = 0; k < g_db[i].n_files; k++)
                fat32_delete_file(g_db[i].files[k]);
            db_unregister(name_save);
            // Затем устанавливаем заново.
            if (amply_install(name_save, out_log, out_log_cap) == 0) upgraded++;
            // Поскольку g_db_count изменился — рестартуем цикл.
            i = -1;
        }
    }
    if (upgraded == 0) log_append(out_log, out_log_cap, "[ok] all packages up to date\n");
    return upgraded;
}

// ----------------------------------------------------------------------------
// Search.
// ----------------------------------------------------------------------------
int amply_search(const char* term, const amply_index_entry_t** results, int max_results) {
    int n = 0;
    for (int i = 0; i < g_repo_count && n < max_results; i++) {
        if (s_contains(g_repo[i].name, term) || s_contains(g_repo[i].summary, term))
            results[n++] = &g_repo[i];
    }
    return n;
}

// ----------------------------------------------------------------------------
// Verify installed.
// ----------------------------------------------------------------------------
int amply_verify_installed(const char* name, char* out_log, size_t out_log_cap) {
    const amply_installed_t* r = amply_db_find(name);
    if (!r) {
        log_append(out_log, out_log_cap, "[error] not installed: ");
        log_append(out_log, out_log_cap, name);
        log_append(out_log, out_log_cap, "\n");
        return -1;
    }

    // Все файлы должны быть на диске.
    int missing = 0;
    for (int i = 0; i < r->n_files; i++) {
        char dummy[16];
        if (fat32_read_file(r->files[i], dummy, 1) < 0) {
            log_append(out_log, out_log_cap, "[missing] ");
            log_append(out_log, out_log_cap, r->files[i]);
            log_append(out_log, out_log_cap, "\n");
            missing++;
        }
    }

    // .amf тоже должен сходиться (если в индексе есть).
    int sig_bad = 0;
    const amply_index_entry_t* idx = amply_repo_find(name);
    if (idx) {
        uint64_t sz = 0; const char* err = 0;
        void* amf = amply_load_amf(idx->amf_path, &sz, &err);
        if (amf) {
            if (!amply_verify(amf, sz, &err)) {
                log_append(out_log, out_log_cap, "[bad-sig] ");
                log_append(out_log, out_log_cap, err ? err : "?");
                log_append(out_log, out_log_cap, "\n");
                sig_bad = 1;
            }
            kfree(amf);
        }
    }

    if (missing == 0 && sig_bad == 0) {
        log_append(out_log, out_log_cap, "[ok] verify ");
        log_append(out_log, out_log_cap, name);
        log_append(out_log, out_log_cap, " OK\n");
        return 0;
    }
    return -2;
}
