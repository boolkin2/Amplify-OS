// ============================================================================
// disk_io.c — блочный слой AMPLIFY-OS: физические ATA-диски + виртуальные
// устройства (этап 48).
// ----------------------------------------------------------------------------
// Номер устройства — единое пространство для всех потребителей (fat32,
// fat32_mkfs, partition, luks, installer):
//   0..3                     — ATA-слоты (ata.c)
//   DISK_VDEV_CRYPT_BASE..   — расшифрованные отображения blkcrypt.c
//                              (dm-crypt-аналог: сектор N виртуального
//                              устройства = XTS-расшифровка сектора
//                              start+N нижнего)
//   DISK_VDEV_RAM            — RAM-диск для self-test'ов FDE
// Потребителям не нужно знать, что именно под номером: FAT32 поверх
// зашифрованного раздела работает тем же кодом, что поверх голого диска.
// ============================================================================
#include "disk_io.h"
#include "serial.h"
#include "timer.h"
#include "ata.h"
#include "blkcrypt.h"
#include "heap.h"

// Один PIO-запрос ограничен 256 секторами (LBA28); режем на куски здесь,
// чтобы вызывающий код мог просить произвольный диапазон.
#define DISK_CHUNK_SECTORS 128

// ----------------------------------------------------------------------------
// RAM-диск (self-test).  Живёт в куче, создаётся и удаляется явно.
// ----------------------------------------------------------------------------
static uint8_t* g_ram      = 0;
static uint64_t g_ram_secs = 0;

int disk_ramdisk_create(uint64_t sectors) {
    if (g_ram || sectors == 0) return -1;
    g_ram = (uint8_t*)kmalloc((size_t)(sectors * DISK_SECTOR_SIZE));
    if (!g_ram) return -1;
    for (uint64_t i = 0; i < sectors * DISK_SECTOR_SIZE; i++) g_ram[i] = 0;
    g_ram_secs = sectors;
    return DISK_VDEV_RAM;
}

void disk_ramdisk_destroy(void) {
    if (!g_ram) return;
    // Там лежали ключевой материал и шифротекст self-test'а — затираем.
    for (uint64_t i = 0; i < g_ram_secs * DISK_SECTOR_SIZE; i++) g_ram[i] = 0;
    kfree(g_ram);
    g_ram = 0;
    g_ram_secs = 0;
}

uint8_t* disk_ramdisk_raw(void) { return g_ram; }

static bool ram_io(uint64_t lba, uint64_t count, void* buf, bool write) {
    if (!g_ram || lba + count > g_ram_secs) return false;
    uint8_t* disk = g_ram + lba * DISK_SECTOR_SIZE;
    uint8_t* mem  = (uint8_t*)buf;
    uint64_t n    = count * DISK_SECTOR_SIZE;
    if (write) for (uint64_t i = 0; i < n; i++) disk[i] = mem[i];
    else       for (uint64_t i = 0; i < n; i++) mem[i]  = disk[i];
    return true;
}

static bool is_ata(int dev) { return dev >= 0 && dev < DISK_ATA_DEVICES; }

// ----------------------------------------------------------------------------
bool disk_present(void) { return ata_present(); }

bool disk_present_dev(int dev) {
    if (is_ata(dev)) {
        const ata_info_t* d = ata_device(dev);
        return d && d->present && !d->atapi;
    }
    if (dev == DISK_VDEV_RAM) return g_ram != 0;
    return blkcrypt_is(dev);
}

uint64_t disk_total_sectors_dev(int dev) {
    if (is_ata(dev)) {
        const ata_info_t* d = ata_device(dev);
        return (d && d->present && !d->atapi) ? d->total_sectors : 0;
    }
    if (dev == DISK_VDEV_RAM) return g_ram_secs;
    return blkcrypt_sectors(dev);
}

bool disk_read_sectors_dev(int dev, uint64_t lba, uint64_t count, void* buf) {
    if (dev == DISK_VDEV_RAM) return ram_io(lba, count, buf, false);
    if (!is_ata(dev))         return blkcrypt_read(dev, lba, count, buf);
    while (count > 0) {
        uint32_t chunk = (count > DISK_CHUNK_SECTORS) ? DISK_CHUNK_SECTORS : (uint32_t)count;
        if (!ata_read_sectors_dev(dev, lba, chunk, buf)) return false;
        lba   += chunk;
        buf    = (uint8_t*)buf + (uint64_t)chunk * DISK_SECTOR_SIZE;
        count -= chunk;
    }
    return true;
}

bool disk_write_sectors_dev(int dev, uint64_t lba, uint64_t count, const void* buf) {
    if (dev == DISK_VDEV_RAM) return ram_io(lba, count, (void*)buf, true);
    if (!is_ata(dev))         return blkcrypt_write(dev, lba, count, buf);
    while (count > 0) {
        uint32_t chunk = (count > DISK_CHUNK_SECTORS) ? DISK_CHUNK_SECTORS : (uint32_t)count;
        if (!ata_write_sectors_dev(dev, lba, chunk, buf)) return false;
        lba   += chunk;
        buf    = (const uint8_t*)buf + (uint64_t)chunk * DISK_SECTOR_SIZE;
        count -= chunk;
    }
    return true;
}

bool disk_zero_sectors_dev(int dev, uint64_t lba, uint64_t count) {
    // Буфер на целый PIO-кусок: обнуление FAT-таблиц нового раздела — это
    // мегабайты, и каждая лишняя команда стоит нескольких VM-exit'ов.
    // Лежит в .bss, поэтому уже нулевой — повторно не чистим.
    static uint8_t zeros[DISK_SECTOR_SIZE * DISK_CHUNK_SECTORS];
    const uint64_t step = sizeof(zeros) / DISK_SECTOR_SIZE;
    while (count > 0) {
        uint64_t n = count > step ? step : count;
        if (!disk_write_sectors_dev(dev, lba, n, zeros)) return false;
        lba   += n;
        count -= n;
    }
    return true;
}

bool disk_flush_dev(int dev) {
    if (dev == DISK_VDEV_RAM) return g_ram != 0;
    if (!is_ata(dev))         return blkcrypt_flush(dev);
    return ata_flush_dev(dev);
}

bool disk_flush(void) {
    return ata_flush_dev(ata_default_device());
}

bool disk_read_sectors(uint64_t lba, uint64_t count, void* buf) {
    return disk_read_sectors_dev(ata_default_device(), lba, count, buf);
}

bool disk_write_sectors(uint64_t lba, uint64_t count, const void* buf) {
    return disk_write_sectors_dev(ata_default_device(), lba, count, buf);
}

// ----------------------------------------------------------------------------
// [41.2] Замер скорости последовательного чтения: сравнить IDE (PIO), AHCI и
// virtio-blk на одном и том же образе. Включается строкой `diskbench=1` в
// /disk/Sanctum/perf.conf, результат — строка `[diskbench] ...` в serial.
// Читаем куски по 1 МиБ с начала диска, пока не наберётся 256 МиБ или не
// пройдёт 3 секунды (IDE под KVM — единицы МБ/с, ждать его минуту незачем).
// Буфер — из кучи и только когда замер попросили: на стеке ядра 1 МиБ уронил
// бы систему, а статический массив лежал мегабайтом нулей в образе ядра
// ([38.1.0], CLAUDE.md §5.50). Не освобождаем: замер бывает раз за загрузку.
// ----------------------------------------------------------------------------
#define DISK_BENCH_BYTES (1024 * 1024)
void disk_bench_report(void) {
    static uint8_t* buf;
    if (!buf) buf = (uint8_t*)kmalloc_aligned(DISK_BENCH_BYTES, 4096);
    if (!buf) { serial_puts("[diskbench] no memory\n"); return; }
    // Самый большой диск: в тестовой ВМ рядом с системным диском висит
    // маленький образ ESP на IDE, и «диск по умолчанию» — это он.
    int dev = -1; uint64_t best = 0;
    for (int i = 0; i < DISK_ATA_DEVICES; i++) {
        if (!disk_present_dev(i)) continue;
        uint64_t n = disk_total_sectors_dev(i);
        if (n > best) { best = n; dev = i; }
    }
    if (dev < 0) { serial_puts("[diskbench] no disk\n"); return; }
    uint64_t total = disk_total_sectors_dev(dev);
    const uint64_t chunk = DISK_BENCH_BYTES / DISK_SECTOR_SIZE;
    uint64_t t0 = timer_get_uptime_ms(), done = 0, lba = 0;
    while (done < 256 && lba + chunk <= total) {
        if (!disk_read_sectors_dev(dev, lba, chunk, buf)) { serial_puts("[diskbench] read error\n"); return; }
        lba += chunk; done++;
        if (timer_get_uptime_ms() - t0 >= 3000) break;
    }
    uint64_t ms = timer_get_uptime_ms() - t0;
    if (ms == 0) ms = 1;
    serial_puts("[diskbench] ");
    serial_puts(ata_device_bus_name(dev));
    serial_puts(": sequential read ");
    serial_print_dec(done); serial_puts(" MiB in ");
    serial_print_dec(ms);   serial_puts(" ms = ");
    serial_print_dec(done * 1000 / ms); serial_puts(" MiB/s\n");
}
