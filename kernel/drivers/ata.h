// ============================================================================
// ata.h - ATA/IDE PIO драйвер для MyOS
// ============================================================================
// Обеспечивает чтение и запись секторов на жёсткий диск через PIO (Program I/O)
// Поддерживает LBA28 адресацию (до 128 GB)
// Используется для работы с виртуальным диском QEMU (-hda disk.img)
// ============================================================================

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Порты ATA контроллера
// ============================================================================

// Primary ATA Controller (IRQ 14)
#define ATA_PRIMARY_DATA        0x1F0   // Порт данных (16 бит)
#define ATA_PRIMARY_ERROR       0x1F1   // Регистр ошибок (чтение) / Features (запись)
#define ATA_PRIMARY_SECCOUNT    0x1F2   // Количество секторов
#define ATA_PRIMARY_LBA_LO      0x1F3   // LBA биты 0-7
#define ATA_PRIMARY_LBA_MID     0x1F4   // LBA биты 8-15
#define ATA_PRIMARY_LBA_HI      0x1F5   // LBA биты 16-23
#define ATA_PRIMARY_DRIVE       0x1F6   // Выбор диска + LBA биты 24-27
#define ATA_PRIMARY_STATUS      0x1F7   // Регистр статуса (чтение)
#define ATA_PRIMARY_COMMAND     0x1F7   // Регистр команд (запись)
#define ATA_PRIMARY_CONTROL     0x3F6   // Регистр управления (альтернативный статус)

// Secondary ATA Controller (IRQ 15)
#define ATA_SECONDARY_DATA      0x170
#define ATA_SECONDARY_ERROR     0x171
#define ATA_SECONDARY_SECCOUNT  0x172
#define ATA_SECONDARY_LBA_LO    0x173
#define ATA_SECONDARY_LBA_MID   0x174
#define ATA_SECONDARY_LBA_HI    0x175
#define ATA_SECONDARY_DRIVE     0x176
#define ATA_SECONDARY_STATUS    0x177
#define ATA_SECONDARY_COMMAND   0x177
#define ATA_SECONDARY_CONTROL   0x376

// ============================================================================
// Биты регистра статуса ATA
// ============================================================================

#define ATA_SR_BSY              0x80    // Busy — контроллер занят
#define ATA_SR_DRDY             0x40    // Drive Ready — диск готов
#define ATA_SR_DF               0x20    // Drive Fault — ошибка диска
#define ATA_SR_DSC              0x10    // Drive Seek Complete
#define ATA_SR_DRQ              0x08    // Data Request — данные готовы
#define ATA_SR_CORR             0x04    // Corrected Data — исправленные данные
#define ATA_SR_IDX              0x02    // Index
#define ATA_SR_ERR              0x01    // Error — ошибка (детали в Error Register)

// ============================================================================
// Команды ATA
// ============================================================================

#define ATA_CMD_READ_PIO        0x20    // Чтение секторов (PIO, 28-bit LBA)
#define ATA_CMD_WRITE_PIO       0x30    // Запись секторов (PIO, 28-bit LBA)
#define ATA_CMD_CACHE_FLUSH     0xE7    // Сброс кэша на диск
#define ATA_CMD_IDENTIFY        0xEC    // Идентификация устройства

// ============================================================================
// Константы
// ============================================================================

#define ATA_SECTOR_SIZE         512     // Размер сектора в байтах
#define ATA_MASTER              0x00    // Master-диск
#define ATA_SLAVE               0x01    // Slave-диск

// Максимальное число секторов за одну операцию (LBA28)
#define ATA_MAX_SECTORS         256

// Таймаут ожидания (циклов опроса)
#define ATA_TIMEOUT             100000

// ============================================================================
// Структуры
// ============================================================================

// Информация об ATA-устройстве
typedef struct {
    bool     present;           // Устройство обнаружено?
    bool     is_ata;            // Это ATA-диск (а не ATAPI CD)?
    bool     lba_supported;     // Поддержка LBA адресации
    uint16_t base_port;         // Базовый порт (0x1F0 или 0x170)
    uint16_t control_port;      // Порт управления (0x3F6 или 0x376)
    uint8_t  drive_select;      // Master (0) или Slave (1)
    uint32_t total_sectors;     // Общее количество секторов (LBA28)
    uint32_t size_mb;           // Размер в мегабайтах
    char     model[41];         // Модель диска (строка из IDENTIFY)
    char     serial[21];        // Серийный номер
} ata_device_t;

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация ATA подсистемы
// Определяет все подключённые ATA-устройства
void ata_init(void);

// Чтение секторов с диска
// Параметры:
//   drive  — номер устройства (0 = Primary Master, 1 = Primary Slave и т.д.)
//   lba    — начальный логический номер сектора (LBA)
//   count  — количество секторов для чтения (1-256, 0 = 256)
//   buffer — буфер для данных (должен вмещать count * 512 байт)
// Возвращает: true при успехе, false при ошибке
bool ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buffer);

// Запись секторов на диск
// Параметры:
//   drive  — номер устройства
//   lba    — начальный LBA
//   count  — количество секторов (1-256, 0 = 256)
//   buffer — буфер с данными для записи
// Возвращает: true при успехе, false при ошибке
bool ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buffer);

// Сброс кэша записи на диск
// Гарантирует, что все записанные данные физически сохранены
bool ata_flush(uint8_t drive);

// Получение информации об устройстве
// Параметры:
//   drive — номер устройства (0-3)
// Возвращает: указатель на структуру ata_device_t или NULL
const ata_device_t* ata_get_device(uint8_t drive);

// Проверка, доступен ли диск
bool ata_is_present(uint8_t drive);

// Получение размера диска в секторах
uint32_t ata_get_size(uint8_t drive);

#endif // ATA_H
