// ============================================================================
// disk_io.h — единый слой блочного I/O для FAT32
// ----------------------------------------------------------------------------
// FAT32-драйвер не должен знать, лежит ли его раздел на ATA или на USB.
// Для этого мы вводим тонкий шим, который маршрутизирует обращения по
// «номеру устройства» (drive):
//
//   drive  0 .. 3   → ATA (Primary Master / Slave / Secondary M / S)
//   drive  0xF0     → USB Mass Storage (см. usb_block.h)
//
// Шим сохраняет ту же сигнатуру, что и ata_*sectors, поэтому все 17 точек
// вызова в fat32.c заменяются механически.
// ============================================================================

#ifndef DISK_IO_H
#define DISK_IO_H

#include <stdint.h>
#include <stdbool.h>

bool disk_present       (uint8_t drive);
bool disk_read_sectors  (uint8_t drive, uint32_t lba, uint32_t count, void* buf);
bool disk_write_sectors (uint8_t drive, uint32_t lba, uint32_t count, const void* buf);

#endif // DISK_IO_H
