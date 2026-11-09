// ============================================================================
// disk_io.c — диспетчер блочного I/O между ATA и USB
// ============================================================================

#include "disk_io.h"
#include "../drivers/ata.h"
#include "../drivers/usb/usb_block.h"

bool disk_present(uint8_t drive)
{
    if (drive >= USB_BLOCK_DRIVE) return usb_block_present();
    return ata_is_present(drive);
}

bool disk_read_sectors(uint8_t drive, uint32_t lba, uint32_t count, void* buf)
{
    if (drive >= USB_BLOCK_DRIVE) return usb_block_read_sectors(lba, count, buf);
    // ata_*_sectors принимает count как uint8_t (макс. 255 секторов за раз).
    // FAT32 всегда зовёт нас с count=1, поэтому усечение безопасно. Если же
    // потребуется больше — дробим итерациями.
    while (count > 0) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_read_sectors(drive, lba, chunk, buf)) return false;
        lba   += chunk;
        buf    = (uint8_t*)buf + (uint32_t)chunk * 512u;
        count -= chunk;
    }
    return true;
}

bool disk_write_sectors(uint8_t drive, uint32_t lba, uint32_t count, const void* buf)
{
    if (drive >= USB_BLOCK_DRIVE) return usb_block_write_sectors(lba, count, buf);
    while (count > 0) {
        uint8_t chunk = (count > 255) ? 255 : (uint8_t)count;
        if (!ata_write_sectors(drive, lba, chunk, buf)) return false;
        lba   += chunk;
        buf    = (const uint8_t*)buf + (uint32_t)chunk * 512u;
        count -= chunk;
    }
    return true;
}
