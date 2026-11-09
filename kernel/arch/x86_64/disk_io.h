// ============================================================================
// disk_io.h — абстракция блочного устройства поверх ATA.
// ----------------------------------------------------------------------------
// Этап 57: добавлены варианты с явным индексом устройства — установщику надо
// писать на выбранный пользователем диск, а не на «тот самый единственный».
// Функции без суффикса `_dev` работают на устройстве по умолчанию и
// сохраняют прежнее поведение.
// ============================================================================
#ifndef AOS_X86_64_DISK_IO_H
#define AOS_X86_64_DISK_IO_H

#include <stdint.h>
#include <stdbool.h>

#define DISK_SECTOR_SIZE  512

bool  disk_present(void);
bool  disk_read_sectors(uint64_t lba, uint64_t count, void* buf);
bool  disk_write_sectors(uint64_t lba, uint64_t count, const void* buf);

bool  disk_present_dev(int dev);
bool  disk_read_sectors_dev(int dev, uint64_t lba, uint64_t count, void* buf);
bool  disk_write_sectors_dev(int dev, uint64_t lba, uint64_t count, const void* buf);
// Заполнить диапазон нулями (используется при форматировании FAT-таблиц).
bool  disk_zero_sectors_dev(int dev, uint64_t lba, uint64_t count);

// Сбросить кеш записи устройства (ATA FLUSH CACHE).  Запись сектора сама
// кеш не сбрасывает — это делается один раз в конце логической операции
// (сохранение файла, форматирование, установка), а не после каждой команды.
bool disk_flush_dev(int dev);

// [41.2] Замер последовательного чтения системного диска, строка в serial.
void disk_bench_report(void);
bool  disk_flush(void);

// ---- Виртуальные устройства (этап 48) ---------------------------------------
// Номера 0..7 — физические (0..3 IDE, 4..7 SATA AHCI).  Выше — виртуальные устройства в том же
// пространстве номеров, поэтому fat32/mkfs/partition работают с ними без
// изменений.
#define DISK_ATA_DEVICES      8     // 0..3 IDE, 4..7 AHCI (ata.c)
#define DISK_VDEV_CRYPT_BASE  8     // 8..15 — blkcrypt (dm-crypt-аналог)
#define DISK_VDEV_CRYPT_MAX   8
#define DISK_VDEV_RAM         16    // RAM-диск для self-test'ов

// Размер устройства в секторах (0 — нет такого устройства).
uint64_t disk_total_sectors_dev(int dev);

// RAM-диск: один на систему, для self-test'ов FDE.  create → DISK_VDEV_RAM
// или -1.  destroy затирает содержимое и освобождает память.
int      disk_ramdisk_create(uint64_t sectors);
void     disk_ramdisk_destroy(void);
uint8_t* disk_ramdisk_raw(void);    // сырые байты (проверка «на диске — шум»)

#endif
