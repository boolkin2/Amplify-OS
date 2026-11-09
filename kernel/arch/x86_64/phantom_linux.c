// ============================================================================
// phantom_linux.c — прямая загрузка ядра Linux в гостя phantom (этап 50.2).
// ----------------------------------------------------------------------------
// ЭТО СТЕНД, а не способ запуска гостей для пользователя: настоящий путь —
// через BIOS и штатный загрузчик с ISO (подэтап 50.3). Здесь мы сами делаем
// то, что обычно делает загрузчик (32-битный протокол загрузки Linux,
// Documentation/arch/x86/boot.rst), чтобы проверить устройства настоящим
// ядром раньше, чем появится BIOS, и не отлаживать всё разом.
//
// Раскладка памяти гостя (всё в первых мегабайтах — свободная «реальная»
// память, ядру она не нужна, пока оно не прочитает параметры):
//   0x01000  GDT из четырёх записей (селекторы 0x10 — код, 0x18 — данные)
//   0x10000  «нулевая страница» boot_params: заголовок из файла ядра + наше
//   0x20000  командная строка
//   0x30000  временно: начало файла ядра (только чтобы прочитать заголовок)
//   0x100000 защищённая часть ядра (сюда ведёт code32_start)
//   конец RAM — initrd
// Входим по code32_start в защищённом режиме без страниц: дальше ядро само
// строит таблицы и переходит в длинный режим — заодно это проверка нашей
// работы с CR0/EFER настоящим кодом, а не только проверочным гостем.
// ============================================================================
#include "phantom_linux.h"
#include "fat32.h"
#include <stddef.h>

#define GDT_GPA     0x1000ull
#define BP_GPA      0x10000ull
#define CMD_GPA     0x20000ull
#define HDR_GPA     0x30000ull
#define HDR_BYTES   0x10000ull
#define KERNEL_GPA  0x100000ull

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

// Файлы читаются напрямую из FAT32, как модели MIND: в реестре VFS большие
// файлы из подкаталогов не числятся. Кусками — чтобы один вызов драйвера не
// тянулся слишком долго.
static bool file_size(const char* path, uint64_t* size) {
    uint8_t probe;
    return fat32_read_file_at(path, 0, &probe, 0, size) >= 0;
}

static bool read_at(const char* path, uint64_t off, uint8_t* dst, uint64_t n) {
    uint64_t got = 0;
    while (got < n) {
        uint64_t part = n - got;
        if (part > (4ull << 20)) part = 4ull << 20;
        int64_t r = fat32_read_file_at(path, off + got, dst + got, part, 0);
        if (r <= 0) return false;
        got += (uint64_t)r;
    }
    return true;
}

static void e820_put(uint8_t* bp, int i, uint64_t addr, uint64_t size, uint32_t type) {
    uint8_t* e = bp + 0x2D0 + i * 20;
    wr64(e, addr);
    wr64(e + 8, size);
    wr32(e + 16, type);
}

bool plinux_load(hv_vm_t* vm, const char* kernel_path, const char* initrd_path,
                 const char* cmdline, hv_start_t* st, plinux_info_t* info, const char** err) {
    uint64_t ram = hv_ram_size(vm);
    *err = "";

    uint64_t ksz = 0, isz = 0;
    if (!file_size(kernel_path, &ksz)) { *err = "kernel file not found"; return false; }
    uint8_t* hdr = (uint8_t*)hv_gpa(vm, HDR_GPA, HDR_BYTES);
    if (ksz < 0x4000 || ksz > (64ull << 20) || !hdr || !read_at(kernel_path, 0, hdr, HDR_BYTES)) {
        *err = "kernel file is unreadable or has an impossible size";
        return false;
    }
    // Заголовок настройки: подпись «HdrS» по смещению 0x202, версия протокола.
    if (rd16(hdr + 0x1FE) != 0xAA55 || rd32(hdr + 0x202) != 0x53726448u || rd16(hdr + 0x206) < 0x0206) {
        *err = "not a Linux bzImage (no HdrS header or boot protocol older than 2.06)";
        return false;
    }
    if (!(hdr[0x211] & 1)) { *err = "kernel is not loaded high (zImage is not supported)"; return false; }

    int sects = hdr[0x1F1] ? hdr[0x1F1] : 4;
    uint64_t pm_off = (uint64_t)(sects + 1) * 512;      // начало защищённой части в файле
    uint64_t pm_size = ksz - pm_off;
    uint32_t init_size = rd32(hdr + 0x260);
    uint64_t pref = rd32(hdr + 0x258) | ((uint64_t)rd32(hdr + 0x25C) << 32);
    uint32_t initrd_max = rd32(hdr + 0x22C);
    if (pref < KERNEL_GPA) pref = 0x1000000ull;

    if (!file_size(initrd_path, &isz)) { *err = "initrd file not found"; return false; }
    // initrd — как можно выше: ядро распаковывается в pref и растёт вверх.
    uint64_t top = ram;
    if ((uint64_t)initrd_max + 1 < top) top = (uint64_t)initrd_max + 1;
    uint64_t initrd_gpa = isz ? ((top - isz) & ~0xFFFull) : 0;
    if (!isz || isz >= top || initrd_gpa < pref + init_size + (1ull << 20) ||
        pm_off >= ksz || KERNEL_GPA + pm_size > pref) {
        // Последнее условие: сжатое ядро в 0x100000 не должно налезать на
        // место, куда оно же будет распаковываться.
        *err = "guest RAM is too small for this kernel and initrd";
        return false;
    }

    uint8_t* kdst = (uint8_t*)hv_gpa(vm, KERNEL_GPA, pm_size);
    uint8_t* idst = (uint8_t*)hv_gpa(vm, initrd_gpa, isz);
    if (!kdst || !idst || !read_at(kernel_path, pm_off, kdst, pm_size) || !read_at(initrd_path, 0, idst, isz)) {
        *err = "reading the kernel or initrd failed";
        return false;
    }

    // ---- boot_params ----
    uint8_t* bp = (uint8_t*)hv_gpa(vm, BP_GPA, 4096);
    for (int i = 0; i < 4096; i++) bp[i] = 0;
    // Заголовок копируется из файла как есть: от 0x1F1 до конца, который
    // задан вторым байтом команды перехода по смещению 0x200.
    uint32_t hdr_end = 0x202u + hdr[0x201];
    if (hdr_end > 0x400) hdr_end = 0x400;
    for (uint32_t i = 0x1F1; i < hdr_end; i++) bp[i] = hdr[i];
    bp[0x210] = 0xFF;                                   // загрузчик «без номера»
    wr16(bp + 0x1FA, 0xFFFF);                           // видеорежим: «обычный», BIOS не трогать
    wr32(bp + 0x218, (uint32_t)initrd_gpa);
    wr32(bp + 0x21C, (uint32_t)isz);
    wr32(bp + 0x228, (uint32_t)CMD_GPA);
    // Карта памяти: обычная память, «дыра» под BIOS и видео, память выше 1 МБ.
    e820_put(bp, 0, 0, 0x9FC00, 1);
    e820_put(bp, 1, 0x9FC00, 0x100000 - 0x9FC00, 2);
    e820_put(bp, 2, 0x100000, ram - 0x100000, 1);
    bp[0x1E8] = 3;

    char* cmd = (char*)hv_gpa(vm, CMD_GPA, 2048);
    int n = 0;
    while (cmdline[n] && n < 2047) { cmd[n] = cmdline[n]; n++; }
    cmd[n] = 0;

    // GDT, которого требует протокол: 0x10 — код, 0x18 — данные, плоские 4 ГБ.
    uint8_t* gdt = (uint8_t*)hv_gpa(vm, GDT_GPA, 32);
    wr64(gdt, 0);
    wr64(gdt + 8, 0);
    wr64(gdt + 16, 0x00CF9B000000FFFFull);
    wr64(gdt + 24, 0x00CF93000000FFFFull);

    for (size_t i = 0; i < sizeof(*st); i++) ((uint8_t*)st)[i] = 0;
    st->mode = HV_START_PROT32;
    st->eip = rd32(hdr + 0x214) ? rd32(hdr + 0x214) : (uint32_t)KERNEL_GPA;
    st->esi = (uint32_t)BP_GPA;
    st->gdt_gpa = (uint32_t)GDT_GPA;

    info->kernel_bytes = ksz;
    info->initrd_bytes = isz;
    info->initrd_gpa = initrd_gpa;
    info->protocol = rd16(hdr + 0x206);
    info->init_size = init_size;
    return true;
}
