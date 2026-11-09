// ============================================================================
// initrd.h — Initial RAM Disk для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Формат идентичен i386 (kernel/fs/initrd.h, tools/mkinitrd.c):
//   [initrd_header_t]                    магия + кол-во файлов
//   [initrd_file_header_t × N]           имя + offset + size
//   [data file 1]
//   [data file 2]
//   ...
// Можем переиспользовать host-утилиту tools/mkinitrd для сборки.
// ============================================================================
#ifndef AOS_X86_64_INITRD_H
#define AOS_X86_64_INITRD_H

#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

#define INITRD_MAGIC            0x4D594F53  /* "MYOS" little-endian */
#define INITRD_MAX_FILENAME     64
#define INITRD_MAX_FILES        256

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t num_files;
} initrd_header_t;

typedef struct __attribute__((packed)) {
    char     name[INITRD_MAX_FILENAME];
    uint32_t offset;
    uint32_t size;
} initrd_file_header_t;

// Discover initrd через MB2 tag MODULE, парсит magic, регистрирует файлы в VFS.
// Возвращает 0 при успехе или -1 если initrd не найден / invalid magic.
int initrd_init_from_mb2(const mb2_info_t* info);

// Прямой доступ для kernel-внутренних нужд (например, ELF loader без VFS).
const uint8_t* initrd_get_file(const char* name, uint64_t* size_out);

// Весь образ initrd целиком — установщик копирует его на целевой ESP как
// initrd.img.  Возвращает 0 если initrd не смонтирован.
const uint8_t* initrd_image(uint64_t* size_out);

#endif
