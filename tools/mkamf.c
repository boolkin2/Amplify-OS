// ============================================================================
// mkamf.c — host-утилита для сборки .amf пакетов AMPLIFY-OS Amply (этап 45).
// ----------------------------------------------------------------------------
// Использование:
//
//   ./mkamf <output.amf> <package_dir>
//
// Где package_dir — папка с:
//   manifest.ini       — блоки [package] и [depends]
//   files/<rel-path>   — файлы, которые надо разложить.  Структура внутри
//                        files/ переносится 1-в-1 в целевые пути:
//                        files/Sanctum/Amply/Welcome.txt → /disk/Sanctum/Amply/Welcome.txt
//                        files/vault/notes/howto.txt    → /disk/vault/notes/howto.txt
//
// Имя файла в файл-таблице получается префиксом "/disk/" + относительным
// путём.  Старший каталог всегда — это категория FAT32 (Sanctum, vault, и т.д.).
//
// Подпись AMPLIFY-Sig-v1: signature[0..7]  = FNV-1a от (manifest||table||payload)
//                                            seed = 0xCBF29CE484222325
//                         signature[8..15] = FNV-1a с seed = 0x414D504C59475231
//                                            ("AMPLYGR1" — синхронно с amply.c)
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define AMF_MAGIC      0x31464D41u
#define AMF_VERSION    1u
#define AMF_PATH_LEN   96
#define AMF_MAX_FILES  32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t manifest_len;
    uint32_t file_count;
    uint64_t payload_len;
    uint32_t compression;
    uint32_t reserved;
    uint8_t  signature[16];
} amf_header_t;

typedef struct __attribute__((packed)) {
    char     path[AMF_PATH_LEN];
    uint64_t offset;
    uint64_t size;
    uint32_t mode;
    uint32_t reserved;
} amf_file_entry_t;

static const uint64_t SEED_PUBLIC = 0xCBF29CE484222325ULL;
static const uint64_t SEED_SECRET = 0x414D504C59475231ULL;

static uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001B3ULL; }
    return h;
}

// ----------------------------------------------------------------------------
// Читаем весь файл в память.  Возвращает указатель + размер.
// ----------------------------------------------------------------------------
static uint8_t* slurp(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

// ----------------------------------------------------------------------------
// Рекурсивный обход files/.  Заполняет таблицу.
// ----------------------------------------------------------------------------
typedef struct {
    char     fs_path[1024];     // host: full path
    char     dst_path[AMF_PATH_LEN];  // amf: "/disk/<...>"
    uint8_t* data;
    size_t   size;
} pkg_file_t;

static int n_files = 0;
static pkg_file_t files[AMF_MAX_FILES];

static int collect_files(const char* root, const char* sub) {
    char dirpath[1024];
    if (sub && sub[0]) snprintf(dirpath, sizeof(dirpath), "%s/%s", root, sub);
    else               snprintf(dirpath, sizeof(dirpath), "%s", root);

    DIR* d = opendir(dirpath);
    if (!d) { fprintf(stderr, "mkamf: cannot open %s\n", dirpath); return -1; }

    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char child_sub[1024];
        if (sub && sub[0]) snprintf(child_sub, sizeof(child_sub), "%s/%s", sub, de->d_name);
        else               snprintf(child_sub, sizeof(child_sub), "%s", de->d_name);
        char child_full[1024];
        snprintf(child_full, sizeof(child_full), "%s/%s", root, child_sub);

        struct stat st;
        if (stat(child_full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (collect_files(root, child_sub) != 0) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            if (n_files >= AMF_MAX_FILES) {
                fprintf(stderr, "mkamf: too many files (max %d)\n", AMF_MAX_FILES);
                closedir(d); return -1;
            }
            pkg_file_t* p = &files[n_files++];
            snprintf(p->fs_path, sizeof(p->fs_path), "%s", child_full);
            // dst path = "/disk/" + child_sub (с нормализацией '\\' → '/').
            snprintf(p->dst_path, sizeof(p->dst_path), "/disk/%s", child_sub);
            for (int i = 0; p->dst_path[i]; i++) if (p->dst_path[i] == '\\') p->dst_path[i] = '/';
        }
    }
    closedir(d);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <out.amf> <package_dir>\n", argv[0]);
        return 1;
    }
    const char* out_path = argv[1];
    const char* pkg_dir  = argv[2];

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.ini", pkg_dir);
    size_t manifest_size = 0;
    uint8_t* manifest = slurp(manifest_path, &manifest_size);
    if (!manifest) { fprintf(stderr, "mkamf: missing %s\n", manifest_path); return 2; }

    char files_dir[1024];
    snprintf(files_dir, sizeof(files_dir), "%s/files", pkg_dir);
    n_files = 0;
    struct stat st;
    if (stat(files_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (collect_files(files_dir, "") != 0) { free(manifest); return 3; }
    }

    // Собираем file_table + payload.
    amf_file_entry_t* tbl = (amf_file_entry_t*)calloc((size_t)n_files, sizeof(amf_file_entry_t));
    if (!tbl) { free(manifest); return 4; }

    uint64_t payload_total = 0;
    for (int i = 0; i < n_files; i++) {
        files[i].data = slurp(files[i].fs_path, &files[i].size);
        if (!files[i].data) { fprintf(stderr, "mkamf: cannot read %s\n", files[i].fs_path); free(tbl); free(manifest); return 5; }
        memset(&tbl[i], 0, sizeof(tbl[i]));
        snprintf(tbl[i].path, AMF_PATH_LEN, "%s", files[i].dst_path);
        tbl[i].offset = payload_total;
        tbl[i].size   = files[i].size;
        tbl[i].mode   = 0644;
        tbl[i].reserved = 0;
        payload_total += files[i].size;
    }

    // Подписываем (manifest || table || payload).
    size_t signed_len = manifest_size + (size_t)n_files * sizeof(amf_file_entry_t) + (size_t)payload_total;
    uint8_t* signed_blob = (uint8_t*)malloc(signed_len);
    if (!signed_blob) { free(tbl); free(manifest); return 6; }
    size_t off = 0;
    memcpy(signed_blob + off, manifest, manifest_size); off += manifest_size;
    memcpy(signed_blob + off, tbl, (size_t)n_files * sizeof(amf_file_entry_t));
    off += (size_t)n_files * sizeof(amf_file_entry_t);
    for (int i = 0; i < n_files; i++) {
        memcpy(signed_blob + off, files[i].data, files[i].size);
        off += files[i].size;
    }

    uint64_t h_pub = fnv1a64(signed_blob, signed_len, SEED_PUBLIC);
    uint64_t h_sec = fnv1a64(signed_blob, signed_len, SEED_SECRET);

    amf_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic        = AMF_MAGIC;
    hdr.version      = AMF_VERSION;
    hdr.manifest_len = (uint32_t)manifest_size;
    hdr.file_count   = (uint32_t)n_files;
    hdr.payload_len  = payload_total;
    hdr.compression  = 0;
    memcpy(hdr.signature + 0, &h_pub, 8);
    memcpy(hdr.signature + 8, &h_sec, 8);

    FILE* out = fopen(out_path, "wb");
    if (!out) { perror(out_path); free(signed_blob); free(tbl); free(manifest); return 7; }
    fwrite(&hdr, sizeof(hdr), 1, out);
    fwrite(manifest, manifest_size, 1, out);
    fwrite(tbl, sizeof(amf_file_entry_t), (size_t)n_files, out);
    for (int i = 0; i < n_files; i++) fwrite(files[i].data, files[i].size, 1, out);
    fclose(out);

    fprintf(stdout, "[mkamf] %s: manifest=%zu B, files=%d, payload=%llu B, total=%zu B\n",
            out_path, manifest_size, n_files, (unsigned long long)payload_total,
            sizeof(hdr) + signed_len);
    for (int i = 0; i < n_files; i++) {
        fprintf(stdout, "        %s -> %s (%zu B)\n", files[i].fs_path, files[i].dst_path, files[i].size);
        free(files[i].data);
    }
    free(signed_blob); free(tbl); free(manifest);
    return 0;
}
