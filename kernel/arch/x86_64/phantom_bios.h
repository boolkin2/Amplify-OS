// ============================================================================
// phantom_bios.h — BIOS гостя phantom (этап 50.3).
// ----------------------------------------------------------------------------
// «Паравиртуальный» BIOS: в памяти гостя — только таблица векторов и заглушки
// (phantom_bios_rom.asm), сами службы исполняет гипервизор. Гость о подмене не
// знает: он делает обычный INT 13h и получает обычный ответ.
//
// Объём — то, что нужно загрузчику isolinux/syslinux и коду реального режима
// ядра Linux, чтобы загрузиться с НЕИЗМЕНЁННОГО ISO:
//   INT 10h — текстовый экран 80x25 (он же память гостя по адресу B8000);
//   INT 13h — один привод CD, загрузка El Torito «без эмуляции», чтение
//             секторов по 2048 байт из образа ISO, лежащего в памяти хоста;
//   INT 15h — карта памяти (E820, E801, 88h), линия A20;
//   INT 16h — клавиатура из кольца ввода консоли гостя;
//   INT 1Ah — счётчик тиков и часы; INT 11h/12h — оборудование и память.
// Чего нет: видеорежимов кроме текстового, VBE, дискет и жёстких дисков,
// PCI BIOS, APM, ACPI. Всё, чего мы не умеем, отвечает «функции нет» и
// попадает в список (bios_stats_t.unknown) — по нему видно, чего не хватило.
// ============================================================================
#ifndef AOS_X86_64_PHANTOM_BIOS_H
#define AOS_X86_64_PHANTOM_BIOS_H

#include "hv.h"
#include "phantom_pc.h"

typedef struct bios bios_t;

enum {
    BIOS_END_NONE = 0,
    BIOS_END_BOOT_FAIL,         // гость вызвал INT 18h: загружаться не с чего
    BIOS_END_REBOOT,            // гость вызвал INT 19h
    BIOS_END_EXCEPTION,         // исключение процессора в реальном режиме
};

typedef struct {
    uint64_t calls;             // вызовов служб
    uint64_t disk_reads, disk_sectors;
    uint64_t chars;             // символов выведено на экран
    uint64_t keys;              // клавиш отдано гостю
    uint32_t unknown[16];       // (вектор << 8) | AH — службы, которых у нас нет
    int      unknown_n;
    int      end;               // BIOS_END_*
    uint8_t  end_vec;           // для исключения: вектор и адрес команды
    uint16_t end_cs, end_ip;
} bios_stats_t;

typedef struct {
    char     volume[33];        // метка тома ISO
    uint32_t boot_lba;          // где лежит образ загрузчика (в секторах по 2048)
    uint16_t boot_sectors;      // сколько 512-байтных секторов BIOS загрузил
    uint16_t load_seg;
} bios_boot_t;

// BSP. iso — образ диска целиком в памяти хоста; жить он должен дольше гостя.
bios_t* bios_create(hv_vm_t* vm, pc_t* pc, const uint8_t* iso, uint64_t iso_bytes);
void    bios_destroy(bios_t* b);
// «Самопроверка при включении» и загрузка с CD: таблица векторов, область
// данных BIOS, чистый экран, образ загрузчика по адресу из каталога El Torito.
// st — с чего стартовать vCPU. false — причина в *err.
bool    bios_boot_cd(bios_t* b, hv_start_t* st, bios_boot_t* info, const char** err);
const bios_stats_t* bios_stats(bios_t* b);
// Строка текстового экрана гостя (0..24) без хвостовых пробелов; out — 81 байт.
// origin — с какого знакоместа видеопамяти начинается экран (pc_vga_origin).
int     bios_screen_row(hv_vm_t* vm, uint32_t origin, int row, char* out);

#endif
