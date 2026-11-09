// ============================================================================
// ata.c - Реализация ATA/IDE PIO драйвера для MyOS
// ============================================================================
// Работает через программный ввод-вывод (PIO) — самый простой и надёжный
// режим ATA. Каждый сектор (512 байт) передаётся через порт 0x1F0.
//
// Поддерживает:
//   - Определение устройств (IDENTIFY)
//   - Чтение секторов (READ SECTORS, LBA28)
//   - Запись секторов (WRITE SECTORS, LBA28)
//   - Сброс кэша (CACHE FLUSH)
//
// QEMU автоматически эмулирует ATA-контроллер при использовании -hda
// ============================================================================

#include "ata.h"
#include "../io.h"
#include "../vga.h"
#include "../memory/memory.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Массив из 4 устройств: Primary Master/Slave, Secondary Master/Slave
static ata_device_t ata_devices[4];

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Маленькая задержка для ATA-контроллера (400ns минимум)
// Читаем альтернативный статус 4 раза — каждое чтение ~100ns
static void ata_io_wait(uint16_t control_port) {
    inb(control_port);
    inb(control_port);
    inb(control_port);
    inb(control_port);
}

// Ожидание снятия флага BSY (контроллер свободен)
// Возвращает true если BSY снят, false при таймауте
static bool ata_wait_bsy(uint16_t base_port) {
    int timeout = ATA_TIMEOUT;
    while ((inb(base_port + 7) & ATA_SR_BSY) && timeout > 0) {
        timeout--;
    }
    return timeout > 0;
}

// Ожидание установки флага DRQ (данные готовы)
// Возвращает true если DRQ установлен, false при ошибке/таймауте
static bool ata_wait_drq(uint16_t base_port) {
    int timeout = ATA_TIMEOUT;
    while (timeout > 0) {
        uint8_t status = inb(base_port + 7);
        if (status & ATA_SR_ERR) return false;  // Ошибка
        if (status & ATA_SR_DF)  return false;   // Ошибка диска
        if (status & ATA_SR_DRQ) return true;    // Данные готовы
        timeout--;
    }
    return false;  // Таймаут
}

// Функция ata_wait_ready временно закомментирована, так как пока не используется

// Копирование строки из IDENTIFY-буфера (байты в обратном порядке)
// ATA хранит строки в формате: каждые 2 байта переставлены местами
static void ata_string_copy(char* dest, const uint16_t* src, int words) {
    for (int i = 0; i < words; i++) {
        dest[i * 2]     = (char)(src[i] >> 8);      // Старший байт — первый символ
        dest[i * 2 + 1] = (char)(src[i] & 0xFF);    // Младший байт — второй символ
    }
    dest[words * 2] = '\0';

    // Убираем пробелы в конце строки
    int len = words * 2;
    while (len > 0 && dest[len - 1] == ' ') {
        dest[--len] = '\0';
    }
}

// ============================================================================
// Идентификация ATA-устройства (команда IDENTIFY)
// ============================================================================

// Выполняет IDENTIFY для одного устройства
// Параметры:
//   index    — индекс в массиве ata_devices (0-3)
//   base     — базовый порт (0x1F0 или 0x170)
//   control  — порт управления (0x3F6 или 0x376)
//   drive_no — 0 для Master, 1 для Slave
static void ata_identify_device(uint8_t index, uint16_t base, uint16_t control,
                                 uint8_t drive_no) {
    ata_device_t* dev = &ata_devices[index];

    // Инициализируем как отсутствующее
    memset(dev, 0, sizeof(ata_device_t));
    dev->base_port = base;
    dev->control_port = control;
    dev->drive_select = drive_no;

    // Выбираем диск: 0xA0 = Master, 0xB0 = Slave
    // Биты 5 и 7 всегда 1, бит 6 = LBA mode, бит 4 = slave
    outb(base + 6, 0xA0 | (drive_no << 4));
    ata_io_wait(control);

    // Обнуляем регистры секторов/LBA
    outb(base + 2, 0);  // Sector count
    outb(base + 3, 0);  // LBA low
    outb(base + 4, 0);  // LBA mid
    outb(base + 5, 0);  // LBA high

    // Отправляем команду IDENTIFY
    outb(base + 7, ATA_CMD_IDENTIFY);
    ata_io_wait(control);

    // Читаем статус
    uint8_t status = inb(base + 7);

    // Если статус = 0, устройства нет
    if (status == 0) {
        return;
    }

    // Ждём пока BSY снимется
    if (!ata_wait_bsy(base)) {
        return;  // Таймаут — устройство не отвечает
    }

    // Проверяем LBA mid и LBA high
    // Если не нули — это не ATA-диск (может быть ATAPI CD-ROM)
    uint8_t lba_mid = inb(base + 4);
    uint8_t lba_hi  = inb(base + 5);
    if (lba_mid != 0 || lba_hi != 0) {
        // Не ATA-устройство (ATAPI/SATA/другое)
        return;
    }

    // Ждём DRQ или ошибку
    if (!ata_wait_drq(base)) {
        return;  // Ошибка или таймаут
    }

    // Читаем 256 слов (512 байт) данных IDENTIFY
    uint16_t identify_buf[256];
    for (int i = 0; i < 256; i++) {
        identify_buf[i] = inw(base);
    }

    // Устройство обнаружено!
    dev->present = true;
    dev->is_ata = true;

    // Word 49, bit 9 — поддержка LBA
    dev->lba_supported = (identify_buf[49] & (1 << 9)) != 0;

    // Words 60-61 — общее количество LBA28 секторов
    dev->total_sectors = (uint32_t)identify_buf[60] | ((uint32_t)identify_buf[61] << 16);

    // Размер в мегабайтах
    dev->size_mb = dev->total_sectors / 2048;  // 1 MB = 2048 секторов

    // Копируем модель (words 27-46, 20 слов = 40 символов)
    ata_string_copy(dev->model, &identify_buf[27], 20);

    // Копируем серийный номер (words 10-19, 10 слов = 20 символов)
    ata_string_copy(dev->serial, &identify_buf[10], 10);
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация ATA подсистемы
void ata_init(void) {
    // Обнуляем все устройства
    memset(ata_devices, 0, sizeof(ata_devices));

    // Идентифицируем все 4 возможных устройства
    // 0: Primary Master
    ata_identify_device(0, ATA_PRIMARY_DATA, ATA_PRIMARY_CONTROL, ATA_MASTER);
    // 1: Primary Slave
    ata_identify_device(1, ATA_PRIMARY_DATA, ATA_PRIMARY_CONTROL, ATA_SLAVE);
    // 2: Secondary Master
    ata_identify_device(2, ATA_SECONDARY_DATA, ATA_SECONDARY_CONTROL, ATA_MASTER);
    // 3: Secondary Slave
    ata_identify_device(3, ATA_SECONDARY_DATA, ATA_SECONDARY_CONTROL, ATA_SLAVE);
}

// Чтение секторов с диска (PIO, LBA28)
bool ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buffer) {
    if (drive >= 4 || !ata_devices[drive].present || !buffer) {
        return false;
    }

    ata_device_t* dev = &ata_devices[drive];
    uint16_t base = dev->base_port;
    uint16_t ctrl = dev->control_port;

    // Количество секторов для чтения (0 = 256)
    uint8_t sec_count = count;

    // Ждём готовности
    if (!ata_wait_bsy(base)) {
        return false;
    }

    // Выбираем диск и устанавливаем старшие 4 бита LBA
    // Бит 6 = 1 (LBA mode), бит 5 = 1 (обязательно), бит 4 = slave
    outb(base + 6, 0xE0 | (dev->drive_select << 4) | ((lba >> 24) & 0x0F));
    ata_io_wait(ctrl);

    // Устанавливаем количество секторов
    outb(base + 2, sec_count);

    // Устанавливаем адрес LBA (младшие 24 бита)
    outb(base + 3, (uint8_t)(lba & 0xFF));          // LBA low  (биты 0-7)
    outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));   // LBA mid  (биты 8-15)
    outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));  // LBA high (биты 16-23)

    // Отправляем команду READ SECTORS
    outb(base + 7, ATA_CMD_READ_PIO);

    // Читаем данные по секторам
    uint16_t* buf16 = (uint16_t*)buffer;
    int sectors_to_read = (sec_count == 0) ? 256 : sec_count;

    for (int s = 0; s < sectors_to_read; s++) {
        // Ждём готовности данных
        if (!ata_wait_drq(base)) {
            return false;
        }

        // Читаем 256 слов (512 байт) — один сектор
        for (int i = 0; i < 256; i++) {
            buf16[s * 256 + i] = inw(base);
        }

        // Небольшая задержка между секторами
        ata_io_wait(ctrl);
    }

    return true;
}

// Запись секторов на диск (PIO, LBA28)
bool ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buffer) {
    if (drive >= 4 || !ata_devices[drive].present || !buffer) {
        return false;
    }

    ata_device_t* dev = &ata_devices[drive];
    uint16_t base = dev->base_port;
    uint16_t ctrl = dev->control_port;

    uint8_t sec_count = count;

    // Ждём готовности
    if (!ata_wait_bsy(base)) {
        return false;
    }

    // Выбираем диск и LBA
    outb(base + 6, 0xE0 | (dev->drive_select << 4) | ((lba >> 24) & 0x0F));
    ata_io_wait(ctrl);

    // Количество секторов
    outb(base + 2, sec_count);

    // Адрес LBA
    outb(base + 3, (uint8_t)(lba & 0xFF));
    outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));

    // Отправляем команду WRITE SECTORS
    outb(base + 7, ATA_CMD_WRITE_PIO);

    // Записываем данные по секторам
    const uint16_t* buf16 = (const uint16_t*)buffer;
    int sectors_to_write = (sec_count == 0) ? 256 : sec_count;

    for (int s = 0; s < sectors_to_write; s++) {
        // Ждём готовности к приёму данных
        if (!ata_wait_drq(base)) {
            return false;
        }

        // Записываем 256 слов (512 байт) — один сектор
        for (int i = 0; i < 256; i++) {
            outw(base, buf16[s * 256 + i]);
        }

        // Задержка между секторами
        ata_io_wait(ctrl);
    }

    // Сбрасываем кэш после записи
    ata_flush(drive);

    return true;
}

// Сброс кэша записи
bool ata_flush(uint8_t drive) {
    if (drive >= 4 || !ata_devices[drive].present) {
        return false;
    }

    ata_device_t* dev = &ata_devices[drive];
    uint16_t base = dev->base_port;

    // Выбираем диск
    outb(base + 6, 0xE0 | (dev->drive_select << 4));

    // Отправляем CACHE FLUSH
    outb(base + 7, ATA_CMD_CACHE_FLUSH);

    // Ждём завершения
    if (!ata_wait_bsy(base)) {
        return false;
    }

    // Проверяем на ошибки
    uint8_t status = inb(base + 7);
    if (status & ATA_SR_ERR) {
        return false;
    }

    return true;
}

// Получение информации об устройстве
const ata_device_t* ata_get_device(uint8_t drive) {
    if (drive >= 4) {
        return 0;
    }
    return &ata_devices[drive];
}

// Проверка наличия диска
bool ata_is_present(uint8_t drive) {
    if (drive >= 4) return false;
    return ata_devices[drive].present;
}

// Получение размера диска в секторах
uint32_t ata_get_size(uint8_t drive) {
    if (drive >= 4 || !ata_devices[drive].present) return 0;
    return ata_devices[drive].total_sectors;
}
