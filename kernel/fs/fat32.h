// ============================================================================
// fat32.h - Драйвер файловой системы FAT32 для MyOS
// ============================================================================
// Поддерживает:
//   - Чтение файлов и каталогов
//   - Создание файлов и каталогов
//   - Запись данных в файлы
//   - Удаление файлов
//   - Навигацию по дереву каталогов
//
// Формат FAT32:
//   [Boot Sector (BPB)] [Reserved Sectors] [FAT1] [FAT2] [Data Region]
//   Кластеры начинаются с номера 2
//   Каждый кластер = N секторов (обычно 8 = 4096 байт)
// ============================================================================

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

// ============================================================================
// Константы FAT32
// ============================================================================

// Сигнатуры
#define FAT32_BOOT_SIGNATURE    0xAA55  // Сигнатура загрузочного сектора
#define FAT32_FSINFO_SIG1       0x41615252  // FSInfo сигнатура 1
#define FAT32_FSINFO_SIG2       0x61417272  // FSInfo сигнатура 2

// Специальные значения кластеров в FAT
#define FAT32_FREE_CLUSTER      0x00000000  // Свободный кластер
#define FAT32_RESERVED_MIN      0x00000002  // Минимальный номер кластера данных
#define FAT32_BAD_CLUSTER       0x0FFFFFF7  // Повреждённый кластер
#define FAT32_EOC_MIN           0x0FFFFFF8  // End Of Chain (минимум)
#define FAT32_EOC               0x0FFFFFFF  // End Of Chain (стандартное)

// Маска для номера кластера (FAT32 использует 28 бит)
#define FAT32_CLUSTER_MASK      0x0FFFFFFF

// Атрибуты записи каталога
#define FAT32_ATTR_READ_ONLY    0x01
#define FAT32_ATTR_HIDDEN       0x02
#define FAT32_ATTR_SYSTEM       0x04
#define FAT32_ATTR_VOLUME_ID    0x08
#define FAT32_ATTR_DIRECTORY    0x10
#define FAT32_ATTR_ARCHIVE      0x20
#define FAT32_ATTR_LONG_NAME    (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | \
                                 FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

// Максимальное количество кэшированных VFS-узлов
#define FAT32_MAX_NODES         128

// Максимальная длина имени 8.3
#define FAT32_SHORT_NAME_LEN    11

// Размер записи каталога
#define FAT32_DIRENT_SIZE       32

// ============================================================================
// Структуры FAT32 (формат на диске)
// ============================================================================

// BIOS Parameter Block (BPB) — первые 512 байт раздела
typedef struct {
    uint8_t  jump[3];               // Инструкция перехода (EB xx 90)
    char     oem_name[8];           // OEM-имя (напр. "MSDOS5.0")
    uint16_t bytes_per_sector;      // Байт на сектор (обычно 512)
    uint8_t  sectors_per_cluster;   // Секторов на кластер (1, 2, 4, 8, 16...)
    uint16_t reserved_sectors;      // Зарезервированные секторы до FAT
    uint8_t  num_fats;              // Количество таблиц FAT (обычно 2)
    uint16_t root_entry_count;      // Записей в корне (0 для FAT32)
    uint16_t total_sectors_16;      // Всего секторов (0 для FAT32)
    uint8_t  media_type;            // Тип носителя (0xF8 = HDD)
    uint16_t fat_size_16;           // Размер FAT (0 для FAT32)
    uint16_t sectors_per_track;     // Секторов на дорожку
    uint16_t num_heads;             // Количество головок
    uint32_t hidden_sectors;        // Скрытые секторы до раздела
    uint32_t total_sectors_32;      // Всего секторов (для FAT32)

    // Расширенные поля FAT32 (EBPB)
    uint32_t fat_size_32;           // Размер одной FAT в секторах
    uint16_t ext_flags;             // Флаги расширения
    uint16_t fs_version;            // Версия FAT32
    uint32_t root_cluster;          // Номер кластера корневого каталога
    uint16_t fs_info_sector;        // Номер сектора FSInfo
    uint16_t backup_boot_sector;    // Резервный загрузочный сектор
    uint8_t  reserved[12];          // Зарезервировано
    uint8_t  drive_number;          // Номер устройства BIOS
    uint8_t  reserved1;
    uint8_t  boot_signature;        // 0x29 если есть следующие поля
    uint32_t volume_id;             // Серийный номер тома
    char     volume_label[11];      // Метка тома
    char     fs_type[8];            // "FAT32   "
} __attribute__((packed)) fat32_bpb_t;

// Запись каталога FAT32 (32 байта)
typedef struct {
    char     name[11];              // Имя 8.3 (без точки, дополнено пробелами)
    uint8_t  attr;                  // Атрибуты файла
    uint8_t  nt_reserved;           // Зарезервировано для Windows NT
    uint8_t  create_time_tenths;    // Десятые доли секунды создания
    uint16_t create_time;           // Время создания
    uint16_t create_date;           // Дата создания
    uint16_t access_date;           // Дата последнего доступа
    uint16_t first_cluster_hi;      // Старшие 16 бит номера кластера
    uint16_t modify_time;           // Время последней модификации
    uint16_t modify_date;           // Дата последней модификации
    uint16_t first_cluster_lo;      // Младшие 16 бит номера кластера
    uint32_t file_size;             // Размер файла в байтах
} __attribute__((packed)) fat32_dirent_t;

// Структура FSInfo (сектор с информацией о свободном пространстве)
typedef struct {
    uint32_t lead_signature;        // 0x41615252
    uint8_t  reserved1[480];
    uint32_t struct_signature;      // 0x61417272
    uint32_t free_count;            // Количество свободных кластеров
    uint32_t next_free;             // Подсказка: следующий свободный кластер
    uint8_t  reserved2[12];
    uint32_t trail_signature;       // 0xAA550000
} __attribute__((packed)) fat32_fsinfo_t;

// ============================================================================
// Структура состояния примонтированной FAT32
// ============================================================================

typedef struct {
    bool     mounted;               // Примонтирована ли ФС
    uint8_t  drive;                 // Номер ATA-устройства
    uint32_t partition_lba;         // LBA начала раздела

    // Параметры из BPB
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;              // Размер одной FAT в секторах
    uint32_t root_cluster;          // Кластер корневого каталога
    uint32_t total_sectors;
    uint32_t total_clusters;

    // Вычисленные значения
    uint32_t fat_start_lba;         // LBA начала первой FAT
    uint32_t data_start_lba;        // LBA начала области данных
    uint32_t cluster_size;          // Размер кластера в байтах

    // FSInfo
    uint32_t free_clusters;         // Свободные кластеры
    uint32_t next_free_cluster;     // Подсказка для поиска свободного кластера
    uint16_t fsinfo_sector;         // Сектор FSInfo

    // VFS интеграция
    vfs_node_t root_node;           // Корневой VFS-узел
    vfs_node_t nodes[FAT32_MAX_NODES]; // Пул VFS-узлов для файлов/каталогов
    uint32_t   node_count;          // Количество используемых узлов
} fat32_fs_t;

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация FAT32 драйвера
void fat32_init(void);

// Монтирование FAT32 раздела
// Параметры:
//   drive         — номер ATA-устройства (0-3)
//   partition_lba — LBA начала раздела (0 для суперфлоппи)
// Возвращает указатель на корневой VFS-узел или NULL при ошибке
vfs_node_t* fat32_mount(uint8_t drive, uint32_t partition_lba);

// Получение информации о FAT32
const fat32_fs_t* fat32_get_info(void);

// Форматирование диска в FAT32 (создание пустой ФС)
// Параметры:
//   drive         — номер ATA-устройства
//   total_sectors — общий размер в секторах
// Возвращает true при успехе
bool fat32_format(uint8_t drive, uint32_t total_sectors);

#endif // FAT32_H
