// ============================================================================
// ata.c — ATA/IDE PIO: перечисление 4 устройств + LBA28/LBA48 read/write.
// ----------------------------------------------------------------------------
// Этап 57.  Раньше драйвер знал только Primary Master, потому что системе
// хватало одного диска с образом FAT32.  Установщику нужен список дисков
// (куда ставить?) и запись на произвольный из них, поэтому здесь полный
// проход по обоим каналам и оба варианта адресации.
// ============================================================================
#include "ata.h"
#include "ahci.h"
#include "virtio_blk.h"
#include "io.h"
#include "serial.h"
#include "timer.h"

static ata_info_t g_dev[ATA_MAX_DEVICES];
static int        g_default = -1;

// Таймауты считаются по часам, а не по числу итераций: под KVM каждый inb —
// это VM-exit (~1-2 мкс), под TCG — десятки наносекунд, и «миллион
// итераций» означал то 2 секунды, то 0.1.  FLUSH CACHE на хосте превращается
// в fsync образа (на /mnt/<диск> в WSL это сотни миллисекунд), поэтому для
// него отдельный длинный таймаут.
#define ATA_TIMEOUT_IO_MS     5000
#define ATA_TIMEOUT_FLUSH_MS  30000
// Страховка на случай, когда прерывания ещё выключены и uptime не растёт:
// тогда выходим по числу опросов.  ata_init() работает ДО первого sti, и
// на время перечисления слотов лимит снижается до прежнего миллиона —
// иначе «зависший» пустой слот стоил бы десятков секунд загрузки.
#define ATA_SPIN_CAP          50000000u
#define ATA_SPIN_CAP_PROBE    1000000u
static uint32_t g_spin_cap = ATA_SPIN_CAP;

// Геометрия каналов: индекс/2 = канал, индекс%2 = slave.
static const uint16_t k_io_base[2]   = { 0x1F0, 0x170 };
static const uint16_t k_ctrl_base[2] = { 0x3F6, 0x376 };

static const char* k_bus_names[ATA_IDE_DEVICES] = {
    "IDE0 master", "IDE0 slave", "IDE1 master", "IDE1 slave"
};
// Имена слотов AHCI ("SATA port N") — номер порта известен только после probe.
static char g_sata_names[AHCI_MAX_SLOTS][16];
// [41.2] Имена слотов, занятых дисками virtio-blk ("VirtIO disk N").
static char g_virtio_names[ATA_MAX_DEVICES][16];

// ----------------------------------------------------------------------------
// Низкоуровневые ожидания.  400 нс задержки после смены DRIVE — требование
// спецификации: контроллеру нужно время, чтобы выставить актуальный статус.
// ----------------------------------------------------------------------------
static void io_wait400(uint16_t ctrl) {
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

// true, если с момента start прошло больше timeout_ms (проверяем не на
// каждой итерации — timer_get_uptime_ms() сам по себе не бесплатный).
static bool timed_out(uint32_t spin, uint64_t start, uint32_t timeout_ms) {
    if (spin >= g_spin_cap) return true;
    if ((spin & 0xFF) != 0) return false;
    return timer_get_uptime_ms() - start > timeout_ms;
}

// Ждём, пока устройство снимет BSY.  ERR здесь не проверяем: после IDENTIFY
// на ATAPI-устройстве ERR выставлен штатно (команда отвергнута).
static bool wait_bsy_ms(uint16_t io, uint32_t timeout_ms) {
    uint64_t start = timer_get_uptime_ms();
    for (uint32_t spin = 0; ; spin++) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) return true;
        if (timed_out(spin, start, timeout_ms)) return false;
    }
}

static bool wait_bsy(uint16_t io) { return wait_bsy_ms(io, ATA_TIMEOUT_IO_MS); }

static bool wait_drq(uint16_t io) {
    uint64_t start = timer_get_uptime_ms();
    for (uint32_t spin = 0; ; spin++) {
        uint8_t s = inb(io + ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) {
            if (s & (ATA_SR_ERR | ATA_SR_DF)) return false;
            if (s & ATA_SR_DRQ) return true;
        }
        if (timed_out(spin, start, ATA_TIMEOUT_IO_MS)) return false;
    }
}

// IDENTIFY-строки в ATA лежат с переставленными байтами внутри слова.
static void ata_string_copy(char* dst, const uint16_t* src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i*2]   = (char)(src[i] >> 8);
        dst[i*2+1] = (char)(src[i] & 0xFF);
    }
    dst[words*2] = 0;
    int len = words*2;
    while (len > 0 && (dst[len-1] == ' ' || dst[len-1] == 0)) dst[--len] = 0;
}

static void select_drive(const ata_info_t* d, uint8_t extra) {
    outb(d->io_base + ATA_REG_DRIVE, (uint8_t)(extra | ((uint8_t)d->slave << 4)));
    io_wait400(d->ctrl_base);
}

// ----------------------------------------------------------------------------
// IDENTIFY одного слота.
// ----------------------------------------------------------------------------
static void probe_device(int index) {
    ata_info_t* d = &g_dev[index];
    d->present   = false;
    d->atapi     = false;
    d->io_base   = k_io_base[index / 2];
    d->ctrl_base = k_ctrl_base[index / 2];
    d->slave     = (uint8_t)(index % 2);

    uint16_t io = d->io_base;

    select_drive(d, 0xA0);
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_CMD, ATA_CMD_IDENTIFY);
    io_wait400(d->ctrl_base);

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) return;       // слот пуст / шина плавает
    if (!wait_bsy(io)) return;

    // Сигнатура в LBA1/LBA2 отличает обычный ATA от ATAPI (CD-ROM) и SATA.
    uint8_t lba_mid = inb(io + ATA_REG_LBA1);
    uint8_t lba_hi  = inb(io + ATA_REG_LBA2);
    if (lba_mid == 0x14 && lba_hi == 0xEB) {
        // ATAPI: перезапрашиваем IDENTIFY PACKET, чтобы получить модель.
        d->atapi = true;
        select_drive(d, 0xA0);
        outb(io + ATA_REG_CMD, ATA_CMD_IDENTIFY_PACKET);
        io_wait400(d->ctrl_base);
        if (!wait_bsy(io) || !wait_drq(io)) return;
    } else if (lba_mid != 0 || lba_hi != 0) {
        return;                                       // SATA/неизвестное устройство
    } else if (!wait_drq(io)) {
        return;
    }

    uint16_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(io + ATA_REG_DATA);

    d->present       = true;
    d->lba_supported = (buf[49] & (1u << 9)) != 0;
    d->lba48         = (buf[83] & (1u << 10)) != 0;
    if (d->atapi) {
        d->total_sectors = 0;                         // размер CD нас не интересует
    } else if (d->lba48) {
        d->total_sectors = (uint64_t)buf[100] | ((uint64_t)buf[101] << 16) |
                           ((uint64_t)buf[102] << 32) | ((uint64_t)buf[103] << 48);
        // Некоторые прошивки объявляют LBA48, но заполняют только LBA28-поле.
        if (d->total_sectors == 0)
            d->total_sectors = (uint64_t)buf[60] | ((uint64_t)buf[61] << 16);
    } else {
        d->total_sectors = (uint64_t)buf[60] | ((uint64_t)buf[61] << 16);
    }
    d->size_mb = d->total_sectors / 2048;
    ata_string_copy(d->model,  &buf[27], 20);
    ata_string_copy(d->serial, &buf[10], 10);

    serial_puts("[ata] ");
    serial_puts(k_bus_names[index]);
    serial_puts(": ");
    serial_puts(d->model);
    if (d->atapi) {
        serial_puts(" (ATAPI)\n");
    } else {
        serial_puts(" — ");
        serial_print_dec(d->size_mb);
        serial_puts(" MB, ");
        serial_puts(d->lba48 ? "LBA48\n" : "LBA28\n");
    }
}

// ----------------------------------------------------------------------------
void ata_init(void) {
    g_default  = -1;
    g_spin_cap = ATA_SPIN_CAP_PROBE;
    for (int i = 0; i < ATA_IDE_DEVICES; i++) probe_device(i);
    g_spin_cap = ATA_SPIN_CAP;
    // SATA-контроллер AHCI: на q35 и на реальных SATA-машинах legacy-портов
    // IDE нет вовсе, и без этого система не видела бы ни одного диска.
    int n = ahci_probe(&g_dev[ATA_AHCI_FIRST], AHCI_MAX_SLOTS);
    for (int k = 0; k < AHCI_MAX_SLOTS; k++) {
        char* nm = g_sata_names[k];
        const char* pfx = "SATA port ";
        int j = 0;
        while (pfx[j]) { nm[j] = pfx[j]; j++; }
        int port = g_dev[ATA_AHCI_FIRST + k].port;
        if (port >= 10) nm[j++] = (char)('0' + port / 10);
        nm[j++] = (char)('0' + port % 10);
        nm[j] = 0;
    }
    (void)n;

    // [41.2] Диски virtio-blk занимают свободные слоты (сначала 4..7, потом
    // 0..3): для остального ядра это обычные диски с номером слота, поэтому
    // установщик, GPT, FAT32 и FDE работают с ними без правок.
    int nv = virtio_blk_init();
    for (int v = 0; v < nv; v++) {
        int slot = -1;
        for (int i = ATA_AHCI_FIRST; i < ATA_MAX_DEVICES && slot < 0; i++) if (!g_dev[i].present) slot = i;
        for (int i = 0; i < ATA_AHCI_FIRST && slot < 0; i++)                if (!g_dev[i].present) slot = i;
        if (slot < 0) { serial_puts("[ata] no free slot for a virtio-blk disk\n"); break; }
        ata_info_t* d = &g_dev[slot];
        d->present = true; d->atapi = false; d->lba_supported = true; d->lba48 = true;
        d->io_base = 0; d->ctrl_base = 0; d->slave = 0;
        d->kind = ATA_KIND_VIRTIO; d->port = (uint8_t)v;
        d->total_sectors = virtio_blk_sectors(v);
        d->size_mb = d->total_sectors / 2048;
        const char* model = "VIRTIO BLOCK DEVICE";
        int j = 0; while (model[j] && j < 40) { d->model[j] = model[j]; j++; } d->model[j] = 0;
        d->serial[0] = 0;
        const char* pfx = "VirtIO disk ";
        char* nm = g_virtio_names[slot];
        j = 0; while (pfx[j]) { nm[j] = pfx[j]; j++; }
        nm[j++] = (char)('0' + v); nm[j] = 0;
        serial_puts("[ata] slot "); serial_print_dec((uint64_t)slot);
        serial_puts(": "); serial_puts(nm); serial_puts(" - ");
        serial_print_dec(d->size_mb); serial_puts(" MB\n");
    }

    for (int i = 0; i < ATA_MAX_DEVICES; i++) {
        if (g_default < 0 && g_dev[i].present && !g_dev[i].atapi) g_default = i;
    }
    if (g_default < 0) serial_puts("[ata] no usable ATA disk found\n");
}

int ata_device_count(void) { return ATA_MAX_DEVICES; }

const ata_info_t* ata_device(int index) {
    if (index < 0 || index >= ATA_MAX_DEVICES) return 0;
    return &g_dev[index];
}

int ata_default_device(void) { return g_default; }

const char* ata_device_bus_name(int index) {
    if (index < 0 || index >= ATA_MAX_DEVICES) return "?";
    if (g_dev[index].present && g_dev[index].kind == ATA_KIND_VIRTIO) return g_virtio_names[index];
    if (index >= ATA_AHCI_FIRST) return g_sata_names[index - ATA_AHCI_FIRST];
    return k_bus_names[index];
}

// Запросы к слоту AHCI: ahci_* принимают не больше 128 секторов (bounce 64 КБ),
// а legacy-API (ata_read_sectors) может попросить 256 — режем на куски.
static bool ahci_transfer(int index, uint64_t lba, uint32_t count, void* buf, bool write) {
    int slot = index - ATA_AHCI_FIRST;
    uint8_t* p = (uint8_t*)buf;
    while (count > 0) {
        uint32_t n = count > 128 ? 128 : count;
        bool ok = write ? ahci_write(slot, lba, n, p) : ahci_read(slot, lba, n, p);
        if (!ok) return false;
        lba += n;
        p += (uint64_t)n * 512;
        count -= n;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Общий путь чтения/записи.  `write == false` → READ, иначе WRITE.
// count ограничен 65536 секторов (LBA48) / 256 (LBA28) — вызывающий код
// в disk_io режет запросы на куски.
// ----------------------------------------------------------------------------
static bool pio_transfer(int index, uint64_t lba, uint32_t count,
                         void* buf, bool write) {
    if (index < 0 || index >= ATA_MAX_DEVICES) return false;
    ata_info_t* d = &g_dev[index];
    if (!d->present || d->atapi || !buf || count == 0) return false;
    if (d->total_sectors && lba + count > d->total_sectors) return false;
    if (d->kind == ATA_KIND_AHCI) return ahci_transfer(index, lba, count, buf, write);
    if (d->kind == ATA_KIND_VIRTIO)
        return write ? virtio_blk_write(d->port, lba, count, buf)
                     : virtio_blk_read (d->port, lba, count, buf);

    uint16_t io = d->io_base;
    bool use48 = d->lba48 && (lba + count > (1ULL << 28) || count > 256);
    if (!use48 && lba + count > (1ULL << 28)) return false;
    if (use48 ? (count > 65536) : (count > 256)) return false;

    if (!wait_bsy(io)) return false;

    if (use48) {
        select_drive(d, 0x40);
        outb(io + ATA_REG_SECCOUNT, (uint8_t)((count >> 8) & 0xFF));
        outb(io + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));
        outb(io + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_CMD, write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT);
    } else {
        select_drive(d, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
        outb(io + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));   // 256 → 0, как требует спека
        outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
        outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
        outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
        outb(io + ATA_REG_CMD, write ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
    }

    // [этап 49] Сектор (256 слов) — одной строковой инструкцией rep insw/outsw.
    // Раньше был цикл по inw/outw: под KVM каждое обращение к порту — отдельный
    // VM-exit, т.е. 256 выходов в гипервизор на сектор (~1–2 МБ/с). Строковый
    // PIO гипервизор обслуживает пачкой — загрузка весов MIND (сотни МБ)
    // ускоряется на порядок. Для реального контроллера это тот же протокол:
    // после DRQ устройство отдаёт ровно 256 слов подряд.
    uint16_t* bp = (uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!wait_drq(io)) return false;
        uint16_t* p = bp + (uint64_t)s * 256;
        uint64_t n = 256;
        if (write) {
            __asm__ __volatile__("rep outsw" : "+S"(p), "+c"(n) : "d"((uint16_t)(io + ATA_REG_DATA)) : "memory");
        } else {
            __asm__ __volatile__("rep insw" : "+D"(p), "+c"(n) : "d"((uint16_t)(io + ATA_REG_DATA)) : "memory");
        }
        // [41.2] Между секторами — ОДНО чтение альтернативного статуса вместо
        // четырёх. Четыре чтения — это правило «400 нс» для смены устройства и
        // подачи команды; после передачи сектора хватает одного (обращение к
        // порту на железе — не меньше ~1 мкс). Под KVM каждое такое чтение —
        // выход в процесс QEMU, и четыре лишних на сектор съедали две трети
        // времени чтения с IDE.
        (void)inb(d->ctrl_base);
    }
    if (!write) return true;

    // Запись завершена, когда устройство сняло BSY после последнего сектора;
    // только тогда можно честно проверить ERR/DF.  FLUSH CACHE здесь НЕ
    // делаем: раньше он шёл после каждой команды, и форматирование раздела
    // превращалось в тысячи fsync на хосте.  Сброс кеша — забота вызывающего
    // (disk_flush_dev в конце логической операции).
    if (!wait_bsy(io)) return false;
    return (inb(io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

bool ata_read_sectors_dev(int index, uint64_t lba, uint32_t count, void* buf) {
    return pio_transfer(index, lba, count, buf, false);
}

bool ata_write_sectors_dev(int index, uint64_t lba, uint32_t count, const void* buf) {
    return pio_transfer(index, lba, count, (void*)buf, true);
}

bool ata_flush_dev(int index) {
    if (index < 0 || index >= ATA_MAX_DEVICES) return false;
    ata_info_t* d = &g_dev[index];
    if (!d->present || d->atapi) return false;
    if (d->kind == ATA_KIND_AHCI) return ahci_flush(index - ATA_AHCI_FIRST);
    if (d->kind == ATA_KIND_VIRTIO) return virtio_blk_flush(d->port);
    // Регистр DRIVE нельзя трогать, пока устройство занято.
    if (!wait_bsy(d->io_base)) return false;
    select_drive(d, 0xE0);
    outb(d->io_base + ATA_REG_CMD, d->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH);
    io_wait400(d->ctrl_base);
    if (!wait_bsy_ms(d->io_base, ATA_TIMEOUT_FLUSH_MS)) {
        serial_puts("[ata] flush timeout\n");
        return false;
    }
    return (inb(d->io_base + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

// ----------------------------------------------------------------------------
// ATAPI: READ(10) по 2048-байтным секторам.
// ----------------------------------------------------------------------------
// IDE: команда PACKET (0xA0) в режиме PIO. Предел байт на один DRQ-блок
// (LBA1/LBA2 до команды) = 2048: устройство отдаёт данные посекторно,
// фактический размер блока читаем обратно из тех же регистров.
static bool atapi_pio_read(ata_info_t* d, uint32_t lba, uint32_t count, void* buf) {
    uint16_t io = d->io_base;
    if (!wait_bsy(io)) return false;
    select_drive(d, 0xA0);
    outb(io + ATA_REG_FEATURES, 0);               // PIO, не DMA
    outb(io + ATA_REG_LBA1, 0x00);                // byte count limit = 0x0800
    outb(io + ATA_REG_LBA2, 0x08);
    outb(io + ATA_REG_CMD, 0xA0);                 // PACKET
    io_wait400(d->ctrl_base);
    if (!wait_drq(io)) return false;
    uint8_t cdb[12] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                        (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0, 0, 0 };
    const uint16_t* cw = (const uint16_t*)cdb;
    for (int i = 0; i < 6; i++) outw(io + ATA_REG_DATA, cw[i]);
    uint8_t* p = (uint8_t*)buf;
    uint64_t left = (uint64_t)count * 2048;
    while (left > 0) {
        if (!wait_drq(io)) return false;
        uint32_t n = (uint32_t)inb(io + ATA_REG_LBA1) | ((uint32_t)inb(io + ATA_REG_LBA2) << 8);
        if (n == 0 || n > left) n = (uint32_t)left;
        uint16_t* w = (uint16_t*)p;
        uint64_t words = n / 2;
        __asm__ __volatile__("rep insw" : "+D"(w), "+c"(words) : "d"((uint16_t)(io + ATA_REG_DATA)) : "memory");
        p += n;
        left -= n;
    }
    if (!wait_bsy(io)) return false;
    return (inb(io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) == 0;
}

bool ata_atapi_read(int index, uint32_t lba, uint32_t count, void* buf) {
    if (index < 0 || index >= ATA_MAX_DEVICES || !buf || count == 0) return false;
    ata_info_t* d = &g_dev[index];
    if (!d->present || !d->atapi) return false;
    uint8_t* p = (uint8_t*)buf;
    while (count > 0) {
        uint32_t n = count > 32 ? 32 : count;     // 32 × 2 КБ = bounce AHCI
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {   // UNIT ATTENTION после сброса
            ok = d->kind == ATA_KIND_AHCI ? ahci_atapi_read(index - ATA_AHCI_FIRST, lba, n, p)
                                          : atapi_pio_read(d, lba, n, p);
        }
        if (!ok) return false;
        lba += n;
        p += (uint64_t)n * 2048;
        count -= n;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Legacy-обёртки: работают на устройстве по умолчанию.
// ----------------------------------------------------------------------------
bool ata_present(void) {
    return g_default >= 0 && g_dev[g_default].present;
}

const ata_info_t* ata_info(void) {
    static const ata_info_t empty = { 0 };
    return g_default >= 0 ? &g_dev[g_default] : &empty;
}

bool ata_read_sectors(uint64_t lba, uint8_t count, void* buf) {
    return ata_read_sectors_dev(g_default, lba, count == 0 ? 256 : count, buf);
}

bool ata_write_sectors(uint64_t lba, uint8_t count, const void* buf) {
    return ata_write_sectors_dev(g_default, lba, count == 0 ? 256 : count, buf);
}

bool ata_flush(void) { return ata_flush_dev(g_default); }
