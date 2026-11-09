// ============================================================================
// ata.h — ATA/IDE PIO драйвер для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 57: драйвер перечисляет все 4 устройства обоих IDE-каналов
// (primary 0x1F0/0x3F6, secondary 0x170/0x376; master + slave) и умеет
// LBA28 и LBA48 read/write.  Это нужно установщику: он показывает список
// дисков и пишет на выбранный, а не на единственный Primary Master.
//
// Старый однодисковый API (ata_present/ata_info/ata_read_sectors/...)
// сохранён и работает поверх «устройства по умолчанию» — первого
// не-ATAPI диска.  Существующий код (fat32, swap, shell) не меняется.
//
// QEMU цепляется через `-drive file=disk.img,format=raw,if=ide,index=N`.
// ============================================================================
#ifndef AOS_X86_64_ATA_H
#define AOS_X86_64_ATA_H

#include <stdint.h>
#include <stdbool.h>

// Порты Primary канала (сохранены для совместимости со старым кодом).
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_CMD        0x1F7
#define ATA_CONTROL    0x3F6

// Смещения регистров относительно io_base канала.
#define ATA_REG_DATA       0
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCOUNT   2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_DRIVE      6
#define ATA_REG_STATUS     7
#define ATA_REG_CMD        7

// Биты статуса.
#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

// Команды.
#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_READ_PIO_EXT   0x24
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_WRITE_PIO_EXT  0x34
#define ATA_CMD_FLUSH          0xE7
#define ATA_CMD_FLUSH_EXT      0xEA
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

// Слоты 0..3 — legacy IDE (primary/secondary × master/slave), 4..7 — порты
// SATA-контроллера AHCI (ahci.c). Номера слотов = номера устройств disk_io.
#define ATA_IDE_DEVICES  4
#define ATA_AHCI_FIRST   4
#define ATA_MAX_DEVICES  8

#define ATA_KIND_IDE     0
#define ATA_KIND_AHCI    1
#define ATA_KIND_VIRTIO  2      // [41.2] virtio-blk; port = номер диска virtio_blk.c

// Информация об одном устройстве.  Имя типа сохранено (ata_info_t), чтобы
// shell/swap/kernel64_main продолжали компилироваться без правок.
typedef struct {
    bool     present;
    bool     atapi;          // CD/DVD — не является целью установки
    bool     lba_supported;
    bool     lba48;
    uint16_t io_base;        // 0x1F0 (primary) / 0x170 (secondary); 0 у AHCI
    uint16_t ctrl_base;      // 0x3F6 / 0x376
    uint8_t  slave;          // 0 = master, 1 = slave
    uint8_t  kind;           // ATA_KIND_IDE / ATA_KIND_AHCI
    uint8_t  port;           // номер порта AHCI
    uint64_t total_sectors;
    uint64_t size_mb;
    char     model[41];
    char     serial[21];
} ata_info_t;

void  ata_init(void);

// ---- Многодисковый API (этап 57) -------------------------------------------
// Индекс устройства: 0 = primary master, 1 = primary slave,
//                    2 = secondary master, 3 = secondary slave.
int   ata_device_count(void);                   // всегда ATA_MAX_DEVICES слотов
const ata_info_t* ata_device(int index);        // NULL если index вне диапазона
int   ata_default_device(void);                 // первый не-ATAPI диск, -1 если нет
// Человекочитаемое имя шины: "IDE0 master" и т.п.
const char* ata_device_bus_name(int index);

bool  ata_read_sectors_dev (int index, uint64_t lba, uint32_t count, void* buf);
bool  ata_write_sectors_dev(int index, uint64_t lba, uint32_t count, const void* buf);
bool  ata_flush_dev(int index);

// CD/DVD (ATAPI): READ(10), сектора по 2048 байт. IDE — PACKET через PIO,
// AHCI — через DMA. Нужен установщику и MIND, чтобы читать файлы с
// установочного диска (iso9660.c).
bool  ata_atapi_read(int index, uint32_t lba, uint32_t count, void* buf);

// ---- Legacy однодисковый API (работает на ata_default_device()) ------------
bool  ata_present(void);
bool  ata_read_sectors(uint64_t lba, uint8_t count, void* buf);
bool  ata_write_sectors(uint64_t lba, uint8_t count, const void* buf);
bool  ata_flush(void);
const ata_info_t* ata_info(void);

#endif
