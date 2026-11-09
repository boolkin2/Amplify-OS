// ============================================================================
// fat32_mkfs.c — mkfs.fat32 внутри ядра.
// ============================================================================
#include "fat32_mkfs.h"
#include "disk_io.h"
#include "crypto.h"
#include "serial.h"

#define SECTOR            512
#define RESERVED_SECTORS  32
#define NUM_FATS          2
#define ROOT_CLUSTER      2
#define FSINFO_SECTOR     1
#define BACKUP_BOOT_SECTOR 6
// Ниже этого числа кластеров том по спецификации считался бы FAT16,
// и наш драйвер (как и любой другой) прочитал бы его неправильно.
#define FAT32_MIN_CLUSTERS 65525

static void mem_zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Размер кластера по размеру тома — та же шкала, что использует mkfs.fat.
static uint8_t pick_sectors_per_cluster(uint64_t sectors) {
    const uint64_t GIB = (1024ULL * 1024 * 1024) / SECTOR;
    if (sectors <=  1 * GIB) return 4;    // 2 КиБ
    if (sectors <=  8 * GIB) return 8;    // 4 КиБ
    if (sectors <= 16 * GIB) return 16;
    if (sectors <= 32 * GIB) return 32;
    return 64;
}

int fat32_mkfs(int dev, uint64_t lba, uint64_t sectors, const char* label,
               fat32_mkfs_tick_t tick, void* user) {
    if (!disk_present_dev(dev) || sectors < 8192) return -1;

    uint8_t spc = pick_sectors_per_cluster(sectors);

    // Формула из fatgen103: подбирает размер FAT так, чтобы таблица покрывала
    // всё оставшееся под данные место и при этом сама в него влезала.
    uint64_t tmp1    = sectors - RESERVED_SECTORS;
    uint64_t tmp2    = ((256ULL * spc) + NUM_FATS) / 2;
    uint64_t fat_size = (tmp1 + (tmp2 - 1)) / tmp2;

    uint64_t data_sectors = sectors - RESERVED_SECTORS - NUM_FATS * fat_size;
    uint64_t clusters     = data_sectors / spc;
    if (clusters < FAT32_MIN_CLUSTERS) {
        serial_puts("[mkfs] volume too small for FAT32\n");
        return -1;
    }

    if (tick) tick(0, user);

    // ---- Загрузочный сектор (BPB) -----------------------------------------
    uint8_t boot[SECTOR];
    mem_zero(boot, SECTOR);
    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;          // jmp short +0x58; nop
    const char oem[8] = { 'A','M','P','L','I','F','Y',' ' };
    for (int i = 0; i < 8; i++) boot[3 + i] = (uint8_t)oem[i];

    put16(boot + 11, SECTOR);              // bytes_per_sector
    boot[13] = spc;                        // sectors_per_cluster
    put16(boot + 14, RESERVED_SECTORS);
    boot[16] = NUM_FATS;
    put16(boot + 17, 0);                   // root_entries (0 для FAT32)
    put16(boot + 19, 0);                   // total_sectors_16
    boot[21] = 0xF8;                       // media type: фиксированный диск
    put16(boot + 22, 0);                   // fat_size_16
    put16(boot + 24, 63);                  // sectors_per_track (легаси-геометрия)
    put16(boot + 26, 255);                 // num_heads
    put32(boot + 28, (uint32_t)lba);       // hidden_sectors = начало раздела
    put32(boot + 32, (uint32_t)sectors);   // total_sectors_32
    put32(boot + 36, (uint32_t)fat_size);  // fat_size_32
    put16(boot + 40, 0);                   // flags: обе FAT активны
    put16(boot + 42, 0);                   // version
    put32(boot + 44, ROOT_CLUSTER);
    put16(boot + 48, FSINFO_SECTOR);
    put16(boot + 50, BACKUP_BOOT_SECTOR);
    boot[64] = 0x80;                       // drive_number
    boot[66] = 0x29;                       // extended boot signature
    put32(boot + 67, crypto_rand_u32());   // volume id
    for (int i = 0; i < 11; i++)
        boot[71 + i] = (uint8_t)((label && label[i]) ? label[i] : ' ');
    const char fstype[8] = { 'F','A','T','3','2',' ',' ',' ' };
    for (int i = 0; i < 8; i++) boot[82 + i] = (uint8_t)fstype[i];
    boot[510] = 0x55; boot[511] = 0xAA;

    // ---- FSInfo ------------------------------------------------------------
    uint8_t fsinfo[SECTOR];
    mem_zero(fsinfo, SECTOR);
    put32(fsinfo + 0,   0x41615252);
    put32(fsinfo + 484, 0x61417272);
    // Счётчик свободных кластеров и подсказку «следующий свободный» пишем как
    // 0xFFFFFFFF («неизвестно, пересчитай»): наш драйвер FSInfo не ведёт, и
    // точное значение устарело бы после первой же записи файла — chkdsk и
    // fsck.fat ругались бы на расхождение.
    put32(fsinfo + 488, 0xFFFFFFFFu);
    put32(fsinfo + 492, 0xFFFFFFFFu);
    put32(fsinfo + 508, 0xAA550000);

    // ---- Очистка зарезервированной области и таблиц FAT -------------------
    if (!disk_zero_sectors_dev(dev, lba, RESERVED_SECTORS)) return -1;
    if (tick) tick(5, user);

    // Обнуление FAT — самая долгая часть, отдаём прогресс кусками по 1 МиБ.
    uint64_t fat_total = NUM_FATS * fat_size;
    uint64_t done = 0;
    while (done < fat_total) {
        uint64_t chunk = fat_total - done;
        if (chunk > 2048) chunk = 2048;
        if (!disk_zero_sectors_dev(dev, lba + RESERVED_SECTORS + done, chunk)) return -1;
        done += chunk;
        if (tick) tick(5 + (int)((done * 85) / fat_total), user);
    }

    // ---- Первый сектор каждой FAT -----------------------------------------
    // FAT[0] — media descriptor, FAT[1] — end-of-chain, FAT[2] — корневой
    // каталог: цепочка из одного кластера.
    uint8_t fat0[SECTOR];
    mem_zero(fat0, SECTOR);
    put32(fat0 + 0, 0x0FFFFFF8);
    put32(fat0 + 4, 0x0FFFFFFF);
    put32(fat0 + 8, 0x0FFFFFF8);
    for (int i = 0; i < NUM_FATS; i++) {
        uint64_t at = lba + RESERVED_SECTORS + (uint64_t)i * fat_size;
        if (!disk_write_sectors_dev(dev, at, 1, fat0)) return -1;
    }

    // ---- Пустой корневой каталог ------------------------------------------
    uint64_t data_start = lba + RESERVED_SECTORS + NUM_FATS * fat_size;
    if (!disk_zero_sectors_dev(dev, data_start, spc)) return -1;

    // Метка тома дублируется записью с атрибутом VOLUME_ID в корне — так
    // делает mkfs.fat, и fsck.fat / Windows сверяют её с полем BPB.  Наш
    // драйвер такие записи пропускает при обходе каталогов.
    if (label && label[0]) {
        uint8_t root0[SECTOR];
        mem_zero(root0, SECTOR);
        for (int i = 0; i < 11; i++) root0[i] = boot[71 + i];
        root0[11] = 0x08;                                  // ATTR_VOLUME_ID
        if (!disk_write_sectors_dev(dev, data_start, 1, root0)) return -1;
    }
    if (tick) tick(95, user);

    // ---- Загрузочный сектор, FSInfo и их резервные копии -------------------
    if (!disk_write_sectors_dev(dev, lba + 0, 1, boot)) return -1;
    if (!disk_write_sectors_dev(dev, lba + FSINFO_SECTOR, 1, fsinfo)) return -1;
    if (!disk_write_sectors_dev(dev, lba + BACKUP_BOOT_SECTOR, 1, boot)) return -1;
    if (!disk_write_sectors_dev(dev, lba + BACKUP_BOOT_SECTOR + FSINFO_SECTOR, 1, fsinfo)) return -1;
    if (!disk_flush_dev(dev)) return -1;

    if (tick) tick(100, user);

    serial_puts("[mkfs] FAT32 @lba ");
    serial_print_dec(lba);
    serial_puts(": ");
    serial_print_dec(clusters);
    serial_puts(" clusters x ");
    serial_print_dec(spc);
    serial_puts(" sectors, fat_size ");
    serial_print_dec(fat_size);
    serial_puts("\n");
    return 0;
}
