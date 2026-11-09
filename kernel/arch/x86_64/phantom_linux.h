// ============================================================================
// phantom_linux.h — прямая загрузка ядра Linux в гостя (стенд этапа 50.2).
// Что это и почему только стенд — в шапке phantom_linux.c.
// ============================================================================
#ifndef AOS_X86_64_PHANTOM_LINUX_H
#define AOS_X86_64_PHANTOM_LINUX_H

#include "hv.h"

typedef struct {
    uint64_t kernel_bytes, initrd_bytes;
    uint64_t initrd_gpa;
    uint32_t init_size;         // сколько памяти ядро просит после распаковки
    uint16_t protocol;          // версия протокола загрузки (0x020F и т.п.)
} plinux_info_t;

// BSP: читает bzImage и initrd с тома FAT32 прямо в память гостя, готовит параметры
// загрузки и начальное состояние vCPU. false — причина в *err.
bool plinux_load(hv_vm_t* vm, const char* kernel_path, const char* initrd_path,
                 const char* cmdline, hv_start_t* st, plinux_info_t* info, const char** err);

#endif
