// ============================================================================
// usb_msd.c — Bulk-Only Transport для USB Mass Storage
// ============================================================================

#include "usb_msd.h"
#include "usb.h"
#include "../../memory/memory.h"
#include "../../vga.h"

static uint32_t g_msd_tag = 0xCAFE0000u;

// ----------------------------------------------------------------------------
// Класс-запросы
// ----------------------------------------------------------------------------

int msd_get_max_lun(usb_device_t* dev)
{
    if (!dev) return -1;
    uint8_t lun = 0;
    int r = usb_control(dev,
        USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        0xFE,                           // Get Max LUN
        0,
        dev->ifdesc.bInterfaceNumber,
        1,
        &lun);
    if (r < 0) return 0;                // STALL → LUN=0
    return lun;
}

int msd_reset(usb_device_t* dev)
{
    if (!dev) return -1;
    int r = usb_control(dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        0xFF,                           // Bulk-Only Mass Storage Reset
        0,
        dev->ifdesc.bInterfaceNumber,
        0,
        NULL);
    // По спеке — после reset нужно Clear Feature(HALT) на оба endpoint'а;
    // в нашем UHCI-стеке это эмулируется обнулением data toggle.
    usb_reset_toggle_in(dev);
    usb_reset_toggle_out(dev);
    return r;
}

// ----------------------------------------------------------------------------
// Полный BBB-цикл
// ----------------------------------------------------------------------------

int msd_send_command(usb_device_t* dev,
                     const uint8_t* cdb, uint8_t cdb_len,
                     void* data, uint32_t data_len, bool data_in,
                     uint32_t* out_transferred)
{
    if (!dev || !cdb || cdb_len == 0 || cdb_len > 16) return -1;
    if (out_transferred) *out_transferred = 0;

    // 1. Сформировать CBW.
    msd_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature          = MSD_CBW_SIGNATURE;
    cbw.dCBWTag                = ++g_msd_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags             = data_in ? MSD_CBW_FLAG_IN : 0x00;
    cbw.bCBWLUN                = 0;
    cbw.bCBWCBLength           = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    int r = usb_bulk_out(dev, &cbw, sizeof(cbw));
    if (r != (int)sizeof(cbw)) {
        vga_puts("[MSD] CBW out failed\n");
        return -2;
    }

    // 2. Data phase (если есть).
    int got = 0;
    if (data_len > 0 && data) {
        if (data_in) {
            got = usb_bulk_in(dev, data, data_len);
        } else {
            got = usb_bulk_out(dev, data, data_len);
        }
        if (got < 0) {
            vga_puts("[MSD] data phase failed\n");
            // По спеке: при STALL — Clear Halt; здесь просто провалим CSW
            // (контроллер всё равно вернёт residue).
        }
    }

    // 3. CSW.
    msd_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    r = usb_bulk_in(dev, &csw, sizeof(csw));
    if (r != (int)sizeof(csw)) {
        // Иногда после STALL endpoint'а нужно повторно прочитать CSW.
        usb_reset_toggle_in(dev);
        r = usb_bulk_in(dev, &csw, sizeof(csw));
        if (r != (int)sizeof(csw)) {
            vga_puts("[MSD] CSW read failed\n");
            return -3;
        }
    }

    if (csw.dCSWSignature != MSD_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
        vga_puts("[MSD] CSW invalid\n");
        return -4;
    }

    if (out_transferred) {
        if (csw.dCSWDataResidue <= data_len)
            *out_transferred = data_len - csw.dCSWDataResidue;
        else
            *out_transferred = 0;
    }

    if (csw.bCSWStatus != 0) {
        // Команда отвергнута / phase error. Вернём положительный код, чтобы
        // верхний слой (SCSI) мог отличить от транспортного сбоя.
        return (int)csw.bCSWStatus;
    }
    return 0;
}
