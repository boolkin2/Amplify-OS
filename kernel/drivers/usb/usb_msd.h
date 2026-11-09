// ============================================================================
// usb_msd.h — USB Mass Storage, Bulk-Only Transport (BBB)
// ----------------------------------------------------------------------------
// Поверх usb_bulk_in/usb_bulk_out этот слой обёртывает «универсальный»
// SCSI-командный канал по протоколу BBB:
//
//   Host → Dev (bulk OUT) : CBW  (31 байт)  — команда + ожидаемая длина
//   ── data phase ──     : bulk IN или OUT (если есть данные)
//   Host ← Dev (bulk IN) : CSW  (13 байт)  — статус выполнения
//
// CBW.signature = "USBC" (LE 0x43425355)
// CSW.signature = "USBS" (LE 0x53425355)
//
// Ниже — структуры пакетов и одна публичная функция msd_send_command(),
// которая выполняет полный цикл CBW → data → CSW.
// ============================================================================

#ifndef USB_MSD_H
#define USB_MSD_H

#include <stdint.h>
#include <stdbool.h>
#include "usb.h"

#define MSD_CBW_SIGNATURE  0x43425355u   // "USBC" little-endian
#define MSD_CSW_SIGNATURE  0x53425355u   // "USBS" little-endian

#define MSD_CBW_FLAG_IN    0x80          // bit7 → данные device→host

// 31-байтный Command Block Wrapper (точный wire-формат).
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;             // только младшие 4 бита
    uint8_t  bCBWCBLength;        // 1..16
    uint8_t  CBWCB[16];           // SCSI CDB
} msd_cbw_t;

// 13-байтный Command Status Wrapper.
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;          // 0 = pass, 1 = fail, 2 = phase error
} msd_csw_t;

// Класс-запрос «Get Max LUN». Возвращает число LUN-1 (0 для одиночной флешки),
// либо -1 если устройство ответило STALL (тогда считаем LUN=0).
int  msd_get_max_lun(usb_device_t* dev);

// Класс-запрос «Bulk-Only Mass Storage Reset» — рекомендуется при ошибках.
int  msd_reset(usb_device_t* dev);

// Полный BBB-цикл: послать CBW, передать data (data_in решает направление),
// прочитать CSW, проверить сигнатуру/статус.
//
// Возвращает 0 при успехе, ненулевое значение — при ошибке. data_len —
// сколько фактически передано (через CSW.residue вычисляется).
int  msd_send_command(usb_device_t* dev,
                      const uint8_t* cdb, uint8_t cdb_len,
                      void* data, uint32_t data_len, bool data_in,
                      uint32_t* out_transferred);

#endif // USB_MSD_H
