// ============================================================================
// partition.h — таблицы разделов GPT/MBR для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 57.  До сих пор система жила на «голом» FAT32-образе без таблицы
// разделов (mkfs.fat прямо на LBA 0).  Установщику этого мало: на реальном
// диске нужен UEFI-загрузочный ESP плюс системный раздел, а значит — GPT.
//
// Модуль умеет:
//   * прочитать существующую таблицу (GPT, с откатом на MBR) — чтобы
//     показать пользователю, что сейчас на диске, и чтобы fat32_mount()
//     находил системный раздел после установки;
//   * рассчитать раскладку под установку;
//   * записать GPT целиком (protective MBR + основной и резервный заголовки).
// ============================================================================
#ifndef AOS_X86_64_PARTITION_H
#define AOS_X86_64_PARTITION_H

#include <stdint.h>
#include <stdbool.h>

#define PART_MAX_ENTRIES     8
#define PART_NAME_MAX        36

typedef enum {
    PART_KIND_UNKNOWN = 0,
    PART_KIND_ESP,          // EFI System Partition
    PART_KIND_DATA,         // Microsoft Basic Data (наши FAT32-разделы)
    PART_KIND_LUKS          // Linux LUKS (этап 48: зашифрованный системный раздел)
} part_kind_t;

typedef struct {
    uint64_t    first_lba;
    uint64_t    last_lba;        // включительно
    part_kind_t kind;
    bool        fat32;           // в первом секторе лежит валидный FAT32 BPB
    bool        luks;            // в первом секторе — заголовок LUKS1
    char        name[PART_NAME_MAX + 1];
} part_entry_t;

typedef struct {
    bool         gpt;            // false → MBR или отсутствие таблицы
    int          count;
    part_entry_t entry[PART_MAX_ENTRIES];
} part_table_t;

// Раскладка, которую пишет установщик.  Значения в секторах по 512 байт.
typedef struct {
    uint64_t esp_lba,   esp_sectors;
    uint64_t sys_lba,   sys_sectors;
    uint64_t vault_lba, vault_sectors;   // 0, если раздел не создаётся
    bool     vault;
    bool     sys_luks;                   // системный раздел шифруется (тип Linux LUKS)
} part_layout_t;

// Минимальный размер целевого диска, при котором раскладка строится.
#define PART_MIN_DISK_SECTORS  (2ULL * 1024 * 1024 * 1024 / 512)

uint32_t part_crc32(const void* data, uint64_t len);

// Прочитать таблицу разделов диска. Возвращает 0, даже если таблицы нет
// (тогда out->count == 0); -1 только при ошибке чтения.
int  part_probe(int dev, part_table_t* out);

// Найти раздел, который следует смонтировать как системный FAT32.
// Возвращает LBA начала раздела, либо 0 если FAT32 лежит прямо на LBA 0
// (dev-образ без таблицы), либо -1 если ничего подходящего нет.
// Если на диске есть LUKS-раздел, ESP НЕ подставляется вместо системного:
// системный том зашифрован и будет смонтирован после разблокировки.
int64_t part_find_system_fat32(int dev);

// [этап 48] Первый LUKS1-раздел диска: true и его границы.
bool    part_find_luks(int dev, uint64_t* lba, uint64_t* sectors);

// Рассчитать раскладку под диск заданного размера.  Возвращает 0 при успехе,
// -1 если диск слишком мал.
int  part_plan(uint64_t total_sectors, bool vault, part_layout_t* out);

// Записать GPT на диск согласно раскладке (перезаписывает всё, что было).
int  part_write_gpt(int dev, uint64_t total_sectors, const part_layout_t* l);

#endif
