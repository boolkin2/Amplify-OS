// ============================================================================
// scsi.h — Минимальный набор SCSI-команд поверх USB MSD (BBB)
// ----------------------------------------------------------------------------
// Все команды отправляются через msd_send_command().
// Все многобайтные поля в SCSI — BIG-ENDIAN (это историческая особенность
// IBM/SCSI), в отличие от little-endian USB descriptors.
//
// Реализованный минимум для Mass Storage:
//   • TEST UNIT READY  — флешка готова?
//   • REQUEST SENSE    — диагностика после ошибки
//   • INQUIRY          — vendor / product / version
//   • READ CAPACITY 10 — последний LBA + размер сектора
//   • READ 10 / WRITE 10 — основной R/W
// ============================================================================

#ifndef USB_SCSI_H
#define USB_SCSI_H

#include <stdint.h>
#include <stdbool.h>
#include "usb.h"

// SCSI Operation Codes
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A

// Стандартный 36-байтный INQUIRY response.
typedef struct __attribute__((packed)) {
    uint8_t  peripheral;            // младшие 5 бит = тип устройства, 0x00=disk
    uint8_t  removable;             // bit7 = removable media
    uint8_t  version;
    uint8_t  resp_format;
    uint8_t  add_length;
    uint8_t  reserved[3];
    char     vendor_id[8];          // "USB     "
    char     product_id[16];        // "FlashDisk       "
    char     product_rev[4];
} scsi_inquiry_data_t;

// READ CAPACITY 10: 8 байт BE.
typedef struct {
    uint32_t last_lba;              // последний доступный LBA
    uint32_t block_size;            // 512 для флешек
} scsi_capacity_t;

// REQUEST SENSE: 18 байт.
typedef struct __attribute__((packed)) {
    uint8_t  response_code;
    uint8_t  reserved1;
    uint8_t  sense_key;             // младшие 4 бита значимы
    uint8_t  information[4];
    uint8_t  add_length;
    uint8_t  cmd_specific[4];
    uint8_t  asc;
    uint8_t  ascq;
    uint8_t  fru;
    uint8_t  ksb[3];
} scsi_sense_data_t;

// ----------------------------------------------------------------------------
// API
// ----------------------------------------------------------------------------
int  scsi_test_unit_ready(usb_device_t* dev);
int  scsi_request_sense(usb_device_t* dev, scsi_sense_data_t* out);
int  scsi_inquiry(usb_device_t* dev, scsi_inquiry_data_t* out);
int  scsi_read_capacity(usb_device_t* dev, scsi_capacity_t* out);
int  scsi_read10 (usb_device_t* dev, uint32_t lba, uint16_t blocks, void* buf);
int  scsi_write10(usb_device_t* dev, uint32_t lba, uint16_t blocks,
                  const void* buf);

// Помощник: ждать пока устройство станет ready (TEST UNIT READY до 10 раз).
int  scsi_wait_ready(usb_device_t* dev);

#endif // USB_SCSI_H
