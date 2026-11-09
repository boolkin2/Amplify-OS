// ============================================================================
// scsi.c — SCSI-команды поверх USB MSD
// ============================================================================

#include "scsi.h"
#include "usb_msd.h"
#include "../timer.h"
#include "../../memory/memory.h"
#include "../../vga.h"

// Помощники: SCSI хочет big-endian, а x86 — little-endian.
static inline uint32_t be32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8 ) |
           ((v & 0x00FF0000u) >> 8 ) |
           ((v & 0xFF000000u) >> 24);
}
static inline uint16_t be16(uint16_t v) {
    return (uint16_t)(((v & 0xFFu) << 8) | ((v & 0xFF00u) >> 8));
}

// ----------------------------------------------------------------------------
// TEST UNIT READY (6 байт CDB, без данных)
// ----------------------------------------------------------------------------
int scsi_test_unit_ready(usb_device_t* dev)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    uint32_t got = 0;
    return msd_send_command(dev, cdb, sizeof(cdb), NULL, 0, true, &got);
}

// ----------------------------------------------------------------------------
// REQUEST SENSE (6 байт CDB, 18 байт IN)
// ----------------------------------------------------------------------------
int scsi_request_sense(usb_device_t* dev, scsi_sense_data_t* out)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint32_t got = 0;
    return msd_send_command(dev, cdb, sizeof(cdb), out, 18, true, &got);
}

// ----------------------------------------------------------------------------
// INQUIRY (6 байт CDB, 36 байт IN)
// ----------------------------------------------------------------------------
int scsi_inquiry(usb_device_t* dev, scsi_inquiry_data_t* out)
{
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint32_t got = 0;
    int r = msd_send_command(dev, cdb, sizeof(cdb), out, 36, true, &got);
    return r;
}

// ----------------------------------------------------------------------------
// READ CAPACITY 10 (10 байт CDB, 8 байт BE IN)
// ----------------------------------------------------------------------------
int scsi_read_capacity(usb_device_t* dev, scsi_capacity_t* out)
{
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0,0,0,0, 0,0,0, 0 };
    uint8_t buf[8];
    uint32_t got = 0;
    int r = msd_send_command(dev, cdb, sizeof(cdb), buf, 8, true, &got);
    if (r) return r;
    if (got != 8) return -10;

    uint32_t lba_be, blk_be;
    memcpy(&lba_be, &buf[0], 4);
    memcpy(&blk_be, &buf[4], 4);
    out->last_lba   = be32(lba_be);
    out->block_size = be32(blk_be);
    return 0;
}

// ----------------------------------------------------------------------------
// READ 10
// ----------------------------------------------------------------------------
int scsi_read10(usb_device_t* dev, uint32_t lba, uint16_t blocks, void* buf)
{
    uint32_t lba_be   = be32(lba);
    uint16_t blocks_be = be16(blocks);

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_10;
    memcpy(&cdb[2], &lba_be, 4);
    memcpy(&cdb[7], &blocks_be, 2);

    uint32_t got = 0;
    int r = msd_send_command(dev, cdb, sizeof(cdb),
                             buf, (uint32_t)blocks * 512u, true, &got);
    if (r == 0 && got == (uint32_t)blocks * 512u) return 0;
    return r ? r : -11;
}

// ----------------------------------------------------------------------------
// WRITE 10
// ----------------------------------------------------------------------------
int scsi_write10(usb_device_t* dev, uint32_t lba, uint16_t blocks,
                 const void* buf)
{
    uint32_t lba_be   = be32(lba);
    uint16_t blocks_be = be16(blocks);

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_WRITE_10;
    memcpy(&cdb[2], &lba_be, 4);
    memcpy(&cdb[7], &blocks_be, 2);

    uint32_t got = 0;
    // msd_send_command сам не модифицирует data при OUT-направлении,
    // поэтому const-cast безопасен.
    int r = msd_send_command(dev, cdb, sizeof(cdb),
                             (void*)buf, (uint32_t)blocks * 512u, false, &got);
    if (r == 0 && got == (uint32_t)blocks * 512u) return 0;
    return r ? r : -12;
}

// ----------------------------------------------------------------------------
// Wait until ready: до 10 попыток с маленькой паузой; после провала чистим
// REQUEST SENSE — некоторые устройства возвращают «not ready» один раз,
// затем приходят в норму.
// ----------------------------------------------------------------------------
int scsi_wait_ready(usb_device_t* dev)
{
    scsi_sense_data_t sense;
    for (int i = 0; i < 10; i++) {
        int r = scsi_test_unit_ready(dev);
        if (r == 0) return 0;
        // Сбрасываем sense, чтобы устройство пришло в нормальное состояние.
        scsi_request_sense(dev, &sense);
        timer_sleep(50);
    }
    vga_puts("[SCSI] device not ready\n");
    return -1;
}
