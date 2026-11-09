// ============================================================================
// amply.h — пакетный менеджер AMPLIFY-OS «Amply» (этап 45).
// ----------------------------------------------------------------------------
// Свой формат пакета .amf (AMPLIFY File). Подкоманды:
//
//   Amply install <pkg>          — установить пакет (с зависимостями)
//   Amply remove  <pkg>          — удалить пакет (если никто не зависит)
//   Amply upgrade                — обновить все пакеты по индексу репо
//   Amply search  <term>         — найти пакеты в индексе репо
//   Amply info    <pkg>          — показать manifest пакета
//   Amply list                   — список установленных
//   Amply verify  <pkg>          — проверить целостность установленного
//   Amply repo    <url>          — установить URL основного репо
//   Amply repo-show              — показать текущий URL репо
//
// Self-hosted сборка .amf отложена до 57-го этапа: пакеты собираются
// host-утилитой tools/mkamf и попадают в initrd как /amply_*.amf плюс
// /amply_index.txt — индекс репо.  В этапе 47 (TLS) добавим скачивание
// удалённых .amf через https.
//
// ============================================================================
//                              ФОРМАТ .amf
// ============================================================================
//   off=0      magic[4]      = "AMF1"
//   off=4      version       = uint32_t (=1)
//   off=8      manifest_len  = uint32_t
//   off=12     file_count    = uint32_t
//   off=16     payload_len   = uint64_t
//   off=24     compression   = uint32_t (0=NONE, 1=ZSTD — этап 51)
//   off=28     reserved      = uint32_t
//   off=32     signature[16] = uint8_t (AMPLIFY-Sig-v1: 2× FNV-1a 64-бит)
//   off=48     manifest      = manifest_len байт INI-подобного текста
//                              [package]
//                              name=...
//                              version=...
//                              summary=...
//                              author=...
//                              [depends]
//                              <pkg>=<version_spec>
//                              ...
//   off=...    file_table    = file_count × file_entry_t (по 120 байт)
//   off=...    payload       = байты файлов друг за другом
//
// FILE ENTRY (120 байт):
//   path[96]   — целевой путь, поддерживается полная глубина FAT32 после
//                stage 45 (например "/disk/Sanctum/Amply/welcome.txt").
//   offset     — uint64_t смещение от начала payload
//   size       — uint64_t размер
//   mode       — uint32_t POSIX permissions (для будущего chmod)
//   reserved   — uint32_t
//
// SIGNATURE v1:
//   sig[0..7]  = FNV-1a 64-bit от (manifest || file_table || payload)
//                с инициализацией = 0xCBF29CE484222325ULL
//   sig[8..15] = FNV-1a 64-bit от того же содержимого, но с инициализацией
//                = AMPLY_KEY_DEFAULT (см. amply.c).
//   В этапе 47 (Crypto + TLS) поле подписи будет заменено на Ed25519,
//   формат файла останется совместим (.amf версия → 2).
// ============================================================================
#ifndef AOS_X86_64_AMPLY_H
#define AOS_X86_64_AMPLY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AMPLY_MAGIC          0x31464D41u   // "AMF1"
#define AMPLY_VERSION        1u
#define AMPLY_PATH_LEN       96
#define AMPLY_NAME_LEN       48
#define AMPLY_VER_LEN        16
#define AMPLY_MAX_DEPS       8
#define AMPLY_MAX_FILES      32
#define AMPLY_MAX_INSTALLED  32
#define AMPLY_MAX_INDEX      32

// Заголовок .amf (48 байт).
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t manifest_len;
    uint32_t file_count;
    uint64_t payload_len;
    uint32_t compression;
    uint32_t reserved;
    uint8_t  signature[16];
} amply_header_t;

// Запись в file_table (всегда 120 байт).
typedef struct __attribute__((packed)) {
    char     path[AMPLY_PATH_LEN];
    uint64_t offset;
    uint64_t size;
    uint32_t mode;
    uint32_t reserved;
} amply_file_entry_t;

// Распарсенный manifest.
typedef struct {
    char     name[AMPLY_NAME_LEN];
    char     version[AMPLY_VER_LEN];
    char     summary[96];
    char     author[64];
    int      n_deps;
    char     deps[AMPLY_MAX_DEPS][AMPLY_NAME_LEN];
} amply_manifest_t;

// БД установленных (in-memory + persist в /disk/Sanctum/Amply/db.txt).
typedef struct {
    char     name[AMPLY_NAME_LEN];
    char     version[AMPLY_VER_LEN];
    int      n_files;
    char     files[AMPLY_MAX_FILES][AMPLY_PATH_LEN];
} amply_installed_t;

// Запись индекса репо (читаем из /amply_index.txt в initrd).
typedef struct {
    char     name[AMPLY_NAME_LEN];
    char     version[AMPLY_VER_LEN];
    char     amf_path[AMPLY_PATH_LEN];
    char     summary[96];
} amply_index_entry_t;

// ============================================================================
// Публичный API.
// ============================================================================

int  amply_init(void);

int  amply_db_count(void);
const amply_installed_t* amply_db_at(int idx);

int  amply_repo_count(void);
const amply_index_entry_t* amply_repo_at(int idx);

const amply_installed_t*    amply_db_find(const char* name);
const amply_index_entry_t*  amply_repo_find(const char* name);

const amply_file_entry_t* amply_parse(const void* data, uint64_t data_len,
                                      amply_header_t* out_hdr,
                                      amply_manifest_t* out_manifest,
                                      const char** err);
bool amply_verify(const void* data, uint64_t data_len, const char** err);

void* amply_load_amf(const char* path, uint64_t* out_size, const char** err);

int amply_install(const char* name, char* out_log, size_t out_log_cap);
int amply_remove (const char* name, char* out_log, size_t out_log_cap);
int amply_upgrade_all(char* out_log, size_t out_log_cap);
int amply_search(const char* term, const amply_index_entry_t** results, int max_results);
int amply_verify_installed(const char* name, char* out_log, size_t out_log_cap);

const char* amply_repo_url(void);
int         amply_set_repo_url(const char* url);

#endif
