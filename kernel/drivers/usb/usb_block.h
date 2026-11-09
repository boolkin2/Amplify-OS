// ============================================================================
// usb_block.h — USB Mass Storage как блочное устройство
// ----------------------------------------------------------------------------
// Поверх scsi.* мы выставляем тот же интерфейс, который ATA-драйвер
// предоставляет в kernel/drivers/ata.h: read/write_sectors. Это позволяет
// использовать общий FAT32-драйвер.
// ============================================================================

#ifndef USB_BLOCK_H
#define USB_BLOCK_H

#include <stdint.h>
#include <stdbool.h>

// Спец-«номер диска» для FAT32: всё, что >=0xF0, маршрутизируется на USB.
#define USB_BLOCK_DRIVE  0xF0

// Полная инициализация: usb_init() → scsi_inquiry → wait_ready →
// scsi_read_capacity. Возвращает true, если флешка готова к R/W.
bool      usb_block_init(void);

// Готова ли флешка (т.е. usb_block_init завершился успешно).
bool      usb_block_present(void);

// R/W секторов 512 байт.
bool      usb_block_read_sectors (uint32_t lba, uint32_t count, void* buf);
bool      usb_block_write_sectors(uint32_t lba, uint32_t count, const void* buf);

// Геометрия (для информационных целей и определения раздела).
uint32_t  usb_block_capacity_lba(void);   // last_lba + 1, в секторах 512
uint32_t  usb_block_sector_size(void);    // обычно 512

// Vendor / Product / Revision (для логов).
const char* usb_block_vendor(void);
const char* usb_block_product(void);

#endif // USB_BLOCK_H
