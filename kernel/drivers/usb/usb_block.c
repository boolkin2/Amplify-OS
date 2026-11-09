// ============================================================================
// usb_block.c — USB MSD как блочное устройство (sector-level R/W)
// ============================================================================

#include "usb_block.h"
#include "usb.h"
#include "scsi.h"
#include "../../memory/memory.h"
#include "../../vga.h"

static bool       g_ready = false;
static uint32_t   g_capacity_lba = 0;
static uint32_t   g_sector_size  = 512;
static char       g_vendor[9]    = "";
static char       g_product[17]  = "";

static usb_device_t* g_dev = NULL;

// ----------------------------------------------------------------------------
// Утилита: убрать хвостовые пробелы (SCSI INQUIRY заполняет ASCII пробелами).
// ----------------------------------------------------------------------------
static void rstrip(char* s) {
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

// ----------------------------------------------------------------------------
// Публичный API
// ----------------------------------------------------------------------------

bool usb_block_init(void)
{
    g_ready = false;
    g_dev = NULL;

    int n = usb_init();
    if (n <= 0) {
        vga_puts("[USB-BLK] no USB devices found\n");
        return false;
    }

    g_dev = usb_get_first_device();
    if (!g_dev) return false;

    // INQUIRY → vendor/product.
    scsi_inquiry_data_t inq;
    memset(&inq, 0, sizeof(inq));
    if (scsi_inquiry(g_dev, &inq) == 0) {
        memcpy(g_vendor,  inq.vendor_id,  8); g_vendor[8]   = 0;
        memcpy(g_product, inq.product_id, 16); g_product[16] = 0;
        rstrip(g_vendor);
        rstrip(g_product);
    }

    // Дать устройству прийти в норму (TEST UNIT READY до 10 раз).
    if (scsi_wait_ready(g_dev) != 0) {
        vga_puts("[USB-BLK] device not ready\n");
        return false;
    }

    // READ CAPACITY (10).
    scsi_capacity_t cap;
    if (scsi_read_capacity(g_dev, &cap) != 0) {
        vga_puts("[USB-BLK] READ CAPACITY failed\n");
        return false;
    }
    g_capacity_lba = cap.last_lba + 1;
    g_sector_size  = cap.block_size;

    if (g_sector_size != 512) {
        vga_puts("[USB-BLK] unsupported sector size, expected 512\n");
        return false;
    }

    vga_puts("[USB-BLK] flash drive ready: ");
    vga_puts(g_vendor);
    vga_puts(" ");
    vga_puts(g_product);
    vga_puts("  sectors=");
    vga_print_dec(g_capacity_lba);
    vga_puts("\n");

    g_ready = true;
    return true;
}

bool usb_block_present(void) { return g_ready; }

bool usb_block_read_sectors(uint32_t lba, uint32_t count, void* buf)
{
    if (!g_ready || !g_dev || count == 0) return false;
    // Дробим по 32 сектора за раз — UHCI bulk transfer ограничен длиной TD.
    uint8_t* dst = (uint8_t*)buf;
    while (count > 0) {
        uint32_t chunk = (count > 32) ? 32 : count;
        if (scsi_read10(g_dev, lba, (uint16_t)chunk, dst) != 0) return false;
        lba   += chunk;
        dst   += chunk * 512;
        count -= chunk;
    }
    return true;
}

bool usb_block_write_sectors(uint32_t lba, uint32_t count, const void* buf)
{
    if (!g_ready || !g_dev || count == 0) return false;
    const uint8_t* src = (const uint8_t*)buf;
    while (count > 0) {
        uint32_t chunk = (count > 32) ? 32 : count;
        if (scsi_write10(g_dev, lba, (uint16_t)chunk, src) != 0) return false;
        lba   += chunk;
        src   += chunk * 512;
        count -= chunk;
    }
    return true;
}

uint32_t    usb_block_capacity_lba(void) { return g_capacity_lba; }
uint32_t    usb_block_sector_size(void)  { return g_sector_size; }
const char* usb_block_vendor(void)        { return g_vendor; }
const char* usb_block_product(void)       { return g_product; }
