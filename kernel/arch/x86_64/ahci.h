// ============================================================================
// ahci.h — SATA-контроллер AHCI (после этапа 57/49: q35, реальные SATA-машины).
// ----------------------------------------------------------------------------
// До этого диски были только на legacy IDE (порты 0x1F0/0x170). На чипсете
// q35 (QEMU/Proxmox) и на реальном железе с SATA в режиме AHCI этих портов
// нет вовсе — установщик видел «0 дисков». Здесь — минимальный драйвер AHCI
// 1.x: PCI class 01/06/01, ABAR (BAR5), по одному командному слоту на порт,
// DMA через bounce-буфер 64 КБ, опрос без прерываний.
//
// Устройства AHCI встраиваются в общую нумерацию ata.c: слоты 4..7 (0..3 —
// legacy IDE). Поэтому установщик, FAT32, FDE, disk_io работают с SATA без
// единой правки — для них это просто ata_device(4) и т.д.
// ============================================================================
#ifndef AOS_X86_64_AHCI_H
#define AOS_X86_64_AHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "ata.h"

#define AHCI_MAX_SLOTS 4           // сколько портов AHCI отображаем в ata-слоты

// Находит контроллер и заполняет out[0..AHCI_MAX_SLOTS) (present/atapi/…).
// Возвращает число найденных устройств.
int  ahci_probe(ata_info_t* out, int max);

// Сектор 512 байт (диск); count <= 128 за вызов (bounce 64 КБ).
bool ahci_read (int slot, uint64_t lba, uint32_t count, void* buf);
bool ahci_write(int slot, uint64_t lba, uint32_t count, const void* buf);
bool ahci_flush(int slot);

// ATAPI (CD/DVD): READ(10), сектор 2048 байт, count <= 32 за вызов.
bool ahci_atapi_read(int slot, uint32_t lba, uint32_t count, void* buf);

#endif
