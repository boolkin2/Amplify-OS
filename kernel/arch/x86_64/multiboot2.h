// ============================================================================
// multiboot2.h — определения для Multiboot2-загрузчика (этап 37.1).
// ----------------------------------------------------------------------------
// Спецификация: https://www.gnu.org/software/grub/manual/multiboot2/
// Используем тэги framebuffer (тип 8) и memory map (тип 6) — этого достаточно
// для скелетного 64-битного ядра. Полный разбор остальных тэгов добавим в 37.2.
// ============================================================================
#ifndef AOS_X86_64_MULTIBOOT2_H
#define AOS_X86_64_MULTIBOOT2_H

#include <stdint.h>
#include <stddef.h>

// Магическое значение, которое GRUB кладёт в EAX перед прыжком в _start.
#define MB2_BOOT_MAGIC          0x36D76289u

// Типы тэгов в загрузочной информации (то, что GRUB передаёт нам).
#define MB2_TAG_END             0
#define MB2_TAG_CMDLINE         1
#define MB2_TAG_BOOTLOADER_NAME 2
#define MB2_TAG_MODULE          3
#define MB2_TAG_BASIC_MEMINFO   4
#define MB2_TAG_BOOTDEV         5
#define MB2_TAG_MMAP            6
#define MB2_TAG_VBE             7
#define MB2_TAG_FRAMEBUFFER     8
#define MB2_TAG_ELF_SECTIONS    9
#define MB2_TAG_APM             10
#define MB2_TAG_EFI32           11
#define MB2_TAG_EFI64           12
#define MB2_TAG_SMBIOS          13
#define MB2_TAG_ACPI_OLD        14
#define MB2_TAG_ACPI_NEW        15
#define MB2_TAG_NETWORK         16
#define MB2_TAG_EFI_MMAP        17
#define MB2_TAG_EFI_BS          18

// Заголовок информации, на который указывает RSI/RBX (физ. адрес).
typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
    // Далее идёт цепочка тэгов, каждый выровнен по 8 байтам.
} mb2_info_t;

// Универсальная "шапка" любого тэга.
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

// Тэг базовой памяти (тип 4): mem_lower (KB ниже 1 MB), mem_upper (KB начиная с 1 MB).
typedef struct __attribute__((packed)) {
    uint32_t type;          // 4
    uint32_t size;          // 16
    uint32_t mem_lower;
    uint32_t mem_upper;
} mb2_tag_basic_meminfo_t;

// Запись memory map (тип 6).
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint64_t len;
    uint32_t type;          // 1 = available, 2 = reserved, 3 = ACPI reclaim, ...
    uint32_t zero;
} mb2_mmap_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t type;          // 6
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    mb2_mmap_entry_t entries[];
} mb2_tag_mmap_t;

// Тэг framebuffer (тип 8). Заполняется только если в заголовке ядра был
// запрос framebuffer-тэга — мы его кладём в boot.asm.
typedef struct __attribute__((packed)) {
    uint32_t type;          // 8
    uint32_t size;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;       // 0=indexed, 1=RGB, 2=EGA text
    uint8_t  reserved;
    // Далее color_info (зависит от fb_type) — нам в 32-bpp RGB не нужно.
} mb2_tag_framebuffer_t;

// Тэг ACPI RSDP (старый/новый — структуры RSDP внутри одинаковы по началу).
typedef struct __attribute__((packed)) {
    uint32_t type;          // 14 или 15
    uint32_t size;
    uint8_t  rsdp[];        // сырой RSDP
} mb2_tag_acpi_t;

// Тэг module (тип 3): GRUB передаёт нам initrd как загруженный модуль.
typedef struct __attribute__((packed)) {
    uint32_t type;          // 3
    uint32_t size;
    uint32_t mod_start;     // физ. адрес начала модуля
    uint32_t mod_end;       // физ. адрес конца (exclusive)
    char     cmdline[];     // null-terminated, например "amplify-initrd"
} mb2_tag_module_t;

// Поиск тэга нужного типа в инфо-структуре.
// Возвращает указатель на mb2_tag_t (можно скастовать к конкретному типу),
// либо NULL если не найден.
const mb2_tag_t* mb2_find_tag(const mb2_info_t* info, uint32_t type);

#endif
