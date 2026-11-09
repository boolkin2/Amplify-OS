// ============================================================================
// fat32.h — FAT32 driver для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// На этапе 45 драйвер расширен с минимальной (root + 8.3) до полнофункциональной
// версии:
//
//   * Парсим BPB (Boot Sector / BIOS Parameter Block).
//   * Ходим по FAT32-таблице кластеров, авто-расширяем цепочки при write.
//   * Произвольные подкаталоги — пути вида "/disk/Sanctum/Amply/db.txt"
//     поддерживаются на чтение, запись, mkdir, удаление.
//   * Long File Names (LFN) — UCS-2/UTF-16LE через серию 0x0F-attr записей,
//     сборка/разборка в наш UTF-8/CP1251 mix (за счёт того, что в строках
//     ASCII + CP1251 → каждый символ уже 1 байт после транскодирования
//     -fexec-charset=CP1251, поэтому в LFN можем передавать прямо так).
//   * `fat32_iter_dir` — итератор директории по абсолютному пути, для
//     `Amply list` и Files-приложения.
//
// API совместим с этапом 44: старые fat32_read_file/write_file/save_file/
// create_file/delete_file работают на абсолютных путях, в т.ч. с подкаталогами.
// ============================================================================
#ifndef AOS_X86_64_FAT32_H
#define AOS_X86_64_FAT32_H

#include <stdint.h>
#include <stdbool.h>

#define FAT32_ATTR_RO         0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLLABEL   0x08
#define FAT32_ATTR_DIR        0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F   // mask: name=4|attr+ro+hidden+system

// Boot Sector / BIOS Parameter Block (BPB) для FAT32.
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extension:
    uint32_t fat_size_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved2;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} fat32_bpb_t;

// 8.3 directory entry (32 байта).
typedef struct __attribute__((packed)) {
    char     name[11];      // 8 имя + 3 расширение, padded spaces
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t size;
} fat32_dirent_t;

// Запись итератора директории: пользователю отдаём логические имя/тип/размер.
// Длинные имена приходят сюда уже собранными в UTF-8/CP1251 (до 255 символов
// FAT32-формата, но мы обрезаем до 192 для удобства).
#define FAT32_LONG_NAME_MAX 192
typedef struct {
    char     name[FAT32_LONG_NAME_MAX];   // длинное (или 8.3) имя
    bool     is_dir;
    uint32_t size;
    uint32_t first_cluster;               // для отладки/iterate
} fat32_dir_entry_t;

// Монтирует Primary Master FAT32 раздел.  Регистрирует все файлы из ROOT
// в VFS с префиксом "/disk/" (включая подкаталоги: "/disk/Sanctum/...").
// Возвращает 0 при успехе.
int fat32_mount(void);

// true если fat32_mount() выполнен успешно.
bool fat32_is_mounted(void);

// Прямой доступ для kernel-нужд: читает файл по абсолютному пути
// "/disk/<любая-глубина>/<имя>" (или просто "<имя>" — root).
// Возвращает количество прочитанных байт или -1.
int64_t fat32_read_file(const char* name, void* buf, uint64_t max_size);

// [этап 57] Чтение фрагмента файла: len байт начиная со смещения off.
// *size_out (если не NULL) получает полный размер файла из каталога.
// Нужно установщику, чтобы сверить записанный payload по кускам, не
// выделяя буфер на весь файл.
int64_t fat32_read_file_at(const char* name, uint64_t off, void* buf, uint64_t len,
                           uint64_t* size_out);

// Перезапись содержимого файла.  В отличие от прежней версии, теперь авто-
// расширяет цепочку кластеров если данные не помещаются.  Возвращает
// количество записанных байт или -1.
int64_t fat32_write_file(const char* name, const void* data, uint64_t size);

// Создание файла (включая родительские каталоги — НЕ создаёт, ожидает что
// они уже существуют, см. fat32_mkdir_p для рекурсивного варианта).  Если
// файл существует, перенаправляет на write_file.
int64_t fat32_create_file(const char* name, const void* data, uint64_t size);

// Удаление файла или пустой директории.
int fat32_delete_file(const char* name);

// [49.1] Потоковая запись файла известного размера (веса MIND, до 4 ГБ - 1)
// без буфера на весь файл: кластеры выделяются сериями при открытии, FAT
// пишется посекторно, данные — крупными командами. Сумма write == size,
// иначе close откажет. Ошибка — abort освобождает кластеры.
#define FAT32_STREAM_RUNS 256
typedef struct {
    bool     active;
    char     path[160];
    uint64_t size, written;
    uint32_t run_start[FAT32_STREAM_RUNS];
    uint32_t run_len[FAT32_STREAM_RUNS];
    int      nruns;
    int      cur_run;
    uint32_t cur_off;
    uint8_t* tail;              // неполный кластер (kmalloc, размер кластера)
    uint32_t tail_n;
} fat32_stream_t;
int     fat32_stream_open(fat32_stream_t* s, const char* path, uint64_t size);
int64_t fat32_stream_write(fat32_stream_t* s, const void* data, uint64_t n);
int     fat32_stream_close(fat32_stream_t* s);
void    fat32_stream_abort(fat32_stream_t* s);

// upsert (write, если есть, иначе create).
int64_t fat32_save_file(const char* name, const void* data, uint64_t size);

// ----------------------------------------------------------------------------
// Этап 45: подкаталоги.
// ----------------------------------------------------------------------------

// Создать директорию (одну, без mkdir -p).  Родитель должен существовать.
// Возвращает 0 если ОК, -1 при ошибке, 1 если директория уже есть.
int fat32_mkdir(const char* path);

// Создать директорию рекурсивно (mkdir -p).  Принимает абсолютный путь
// "/disk/Sanctum/Amply".  Возвращает 0 если все компоненты существуют или
// были созданы.
int fat32_mkdir_p(const char* path);

// Перечисление содержимого директории.  callback вызывается для каждой
// записи.  user_data передаётся прозрачно.  callback возвращает 0 для
// продолжения или ненулевое для остановки итерации (значение возвращается
// fat32_iter_dir).
typedef int (*fat32_dir_cb_t)(const fat32_dir_entry_t* e, void* user_data);
int fat32_iter_dir(const char* path, fat32_dir_cb_t cb, void* user_data);

// [51.2] Что лежит по пути: файл или каталог и размер. 0 — есть, -1 — нет.
int fat32_stat(const char* path, fat32_dir_entry_t* out);

// [51.2] Включить/выключить занесение НОВЫХ файлов в реестр VFS (по умолчанию
// включено). Возвращает прежнее значение; выключивший обязан вернуть его.
bool fat32_vfs_autoregister(bool on);

// ----------------------------------------------------------------------------
// Этап 57: привязка тома (для установщика).
// ----------------------------------------------------------------------------

// [этап 48] Смонтировать том (dev, base_lba) как /disk с регистрацией файлов
// в VFS — для системного раздела, открытого через blkcrypt после ввода
// пароля.  0 при успехе.
int fat32_mount_volume(int dev, uint64_t base_lba);

// Текущий том: индекс ATA-устройства и LBA начала раздела (-1 если не смонтирован).
void fat32_get_volume(int* dev, uint64_t* base_lba);

// Привязать драйвер к произвольному FAT32-разделу (dev, base_lba) без
// регистрации содержимого в VFS.  Используется установщиком, чтобы наполнить
// свежеотформатированный раздел на целевом диске.  Возвращает 0 при успехе.
int fat32_bind(int dev, uint64_t base_lba);

// Отвязать драйвер (после этого fat32_is_mounted() == false).
void fat32_unbind(void);

// Том живой системы (тот, что смонтирован как /disk и светится в VFS).
// Установщик запоминает его до работы и восстанавливает после — вместе с
// флагом «новые файлы регистрировать в VFS», иначе live-сессия после
// установки перестала бы видеть созданные файлы.  Повторной регистрации
// уже известных файлов restore не делает.
typedef struct {
    int      dev;          // -1 — ничего не было смонтировано
    uint64_t base_lba;
    bool     vfs_attached;
} fat32_volume_t;

void fat32_save_volume(fat32_volume_t* out);
void fat32_restore_volume(const fat32_volume_t* v);

#endif
