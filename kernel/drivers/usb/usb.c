// ============================================================================
// usb.c — Ядро USB (энумерация, стандартные запросы)
// ============================================================================

#include "usb.h"
#include "uhci.h"
#include "../timer.h"
#include "../../memory/heap.h"
#include "../../memory/memory.h"
#include "../../vga.h"

// На UHCI у нас максимум 2 порта на контроллер; для учебной OS этого хватит.
#define USB_MAX_DEVICES 2

static usb_device_t g_devs[USB_MAX_DEVICES];
static int          g_dev_count = 0;
static uint8_t      g_next_address = 1;

// ----------------------------------------------------------------------------
// Простой busy-wait
// ----------------------------------------------------------------------------
static inline void usb_delay_ms(uint32_t ms) { timer_sleep(ms); }

// ----------------------------------------------------------------------------
// Низкоуровневые хелперы
// ----------------------------------------------------------------------------

int usb_control(usb_device_t* dev, uint8_t bmReqType, uint8_t bReq,
                uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                void* data)
{
    if (!dev) return -1;

    usb_setup_t setup;
    setup.bmRequestType = bmReqType;
    setup.bRequest      = bReq;
    setup.wValue        = wValue;
    setup.wIndex        = wIndex;
    setup.wLength       = wLength;

    bool data_in = (bmReqType & 0x80) != 0;
    int  data_len = wLength;

    return uhci_control_transfer(dev->address, dev->max_packet0,
                                 &setup,
                                 data, data_len, data_in,
                                 dev->low_speed);
}

int usb_bulk_in(usb_device_t* dev, void* data, int len)
{
    if (!dev) return -1;
    uint16_t mp = dev->ep_in.wMaxPacketSize ? dev->ep_in.wMaxPacketSize : 64;
    return uhci_bulk_transfer(dev->address,
                              USB_EP_NUM(dev->ep_in.bEndpointAddress),
                              mp, data, len, true,
                              &dev->toggle_in);
}

int usb_bulk_out(usb_device_t* dev, const void* data, int len)
{
    if (!dev) return -1;
    uint16_t mp = dev->ep_out.wMaxPacketSize ? dev->ep_out.wMaxPacketSize : 64;
    // UHCI bulk_transfer берёт void*, делаем явное снятие const'a — буфер
    // только читается при OUT-направлении.
    return uhci_bulk_transfer(dev->address,
                              USB_EP_NUM(dev->ep_out.bEndpointAddress),
                              mp, (void*)data, len, false,
                              &dev->toggle_out);
}

void usb_reset_toggle_in(usb_device_t* dev)  { if (dev) dev->toggle_in  = false; }
void usb_reset_toggle_out(usb_device_t* dev) { if (dev) dev->toggle_out = false; }

// ----------------------------------------------------------------------------
// Стандартные запросы (помощники)
// ----------------------------------------------------------------------------

static int usb_get_descriptor(usb_device_t* dev, uint8_t type, uint8_t index,
                              uint16_t lang, void* buf, uint16_t len)
{
    return usb_control(dev,
                       USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_GET_DESCRIPTOR,
                       ((uint16_t)type << 8) | index,
                       lang,
                       len,
                       buf);
}

static int usb_set_address(usb_device_t* dev, uint8_t addr)
{
    int r = usb_control(dev,
                        USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_SET_ADDRESS,
                        addr, 0, 0,
                        NULL);
    if (r < 0) return r;
    dev->address = addr;
    // По спеке устройству нужно ≥2 ms на принятие нового адреса.
    usb_delay_ms(5);
    return 0;
}

static int usb_set_configuration(usb_device_t* dev, uint8_t cfg)
{
    int r = usb_control(dev,
                        USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_SET_CONFIGURATION,
                        cfg, 0, 0,
                        NULL);
    if (r < 0) return r;
    dev->configuration = cfg;
    usb_delay_ms(5);
    return 0;
}

// ----------------------------------------------------------------------------
// Парсер дескрипторов конфигурации: ищем Mass Storage (BBB+SCSI) интерфейс
// и его bulk IN / bulk OUT endpoints.
// ----------------------------------------------------------------------------

static bool usb_pick_msd_endpoints(usb_device_t* dev, const uint8_t* buf, int len)
{
    bool have_if = false, have_in = false, have_out = false;

    int off = 0;
    while (off + 2 <= len) {
        uint8_t blen = buf[off + 0];
        uint8_t btype = buf[off + 1];
        if (blen < 2 || off + blen > len) break;

        if (btype == USB_DT_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t* ifd =
                (const usb_interface_descriptor_t*)(buf + off);
            if (ifd->bAlternateSetting == 0 &&
                ifd->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
                ifd->bInterfaceSubClass == USB_MSD_SUBCLASS_SCSI &&
                ifd->bInterfaceProtocol == USB_MSD_PROTOCOL_BBB) {
                dev->ifdesc = *ifd;
                have_if = true;
                have_in = have_out = false;   // ищем endpoints только этого if
            } else {
                // Другой интерфейс: пропускаем endpoint'ы, идущие за ним.
                have_if = false;
            }
        } else if (btype == USB_DT_ENDPOINT && blen >= sizeof(usb_endpoint_descriptor_t)) {
            const usb_endpoint_descriptor_t* epd =
                (const usb_endpoint_descriptor_t*)(buf + off);
            if (have_if && (epd->bmAttributes & USB_EP_TYPE_MASK) == USB_EP_TYPE_BULK) {
                if (USB_EP_DIR_IN(epd->bEndpointAddress) && !have_in) {
                    dev->ep_in = *epd;
                    have_in = true;
                } else if (!USB_EP_DIR_IN(epd->bEndpointAddress) && !have_out) {
                    dev->ep_out = *epd;
                    have_out = true;
                }
            }
        }

        off += blen;
        if (have_if && have_in && have_out) return true;
    }
    return have_if && have_in && have_out;
}

// ----------------------------------------------------------------------------
// Энумерация одного порта
// ----------------------------------------------------------------------------

static bool enumerate_port(int port)
{
    if (g_dev_count >= USB_MAX_DEVICES) return false;

    if (!uhci_port_reset(port)) return false;          // нет устройства
    bool low_speed = false;
    if (!uhci_port_connected(port, &low_speed)) return false;

    usb_device_t* dev = &g_devs[g_dev_count];
    memset(dev, 0, sizeof(*dev));
    dev->present     = true;
    dev->address     = 0;          // ещё default
    dev->low_speed   = low_speed;
    dev->max_packet0 = low_speed ? 8 : 8;   // безопасный старт
    dev->port        = port;

    // 1. Прочитать первые 8 байт DEVICE_DESCRIPTOR — там лежит реальный
    //    bMaxPacketSize0; до этого мы бьём transferами по 8 байт.
    uint8_t small[8];
    int r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, small, 8);
    if (r < 0) {
        vga_puts("[USB] GET_DESCRIPTOR(DEVICE,8) failed\n");
        dev->present = false;
        return false;
    }
    dev->max_packet0 = small[7];
    if (dev->max_packet0 != 8 && dev->max_packet0 != 16 &&
        dev->max_packet0 != 32 && dev->max_packet0 != 64) {
        dev->max_packet0 = 8;   // защита от мусора
    }

    // 2. Повторный reset для надёжности (так делает Linux).
    if (!uhci_port_reset(port)) { dev->present = false; return false; }

    // 3. SET_ADDRESS.
    uint8_t new_addr = g_next_address++;
    if (usb_set_address(dev, new_addr) < 0) {
        vga_puts("[USB] SET_ADDRESS failed\n");
        dev->present = false;
        return false;
    }

    // 4. Полный DEVICE_DESCRIPTOR.
    if (usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0,
                           &dev->devdesc, sizeof(dev->devdesc)) < 0) {
        vga_puts("[USB] GET_DESCRIPTOR(DEVICE) full failed\n");
        dev->present = false;
        return false;
    }

    vga_puts("[USB] device VID:");
    vga_print_hex(dev->devdesc.idVendor);
    vga_puts(" PID:");
    vga_print_hex(dev->devdesc.idProduct);
    vga_puts(" class:");
    vga_print_hex(dev->devdesc.bDeviceClass);
    vga_puts("\n");

    // 5. CONFIG_DESCRIPTOR — сначала 9 байт чтобы узнать wTotalLength.
    usb_config_descriptor_t cfg9;
    if (usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, &cfg9, sizeof(cfg9)) < 0) {
        vga_puts("[USB] GET_DESCRIPTOR(CONFIG,9) failed\n");
        dev->present = false;
        return false;
    }

    uint16_t total = cfg9.wTotalLength;
    if (total > 512) total = 512;   // саниметризация
    uint8_t* full = (uint8_t*)kmalloc(total);
    if (!full) { dev->present = false; return false; }

    int got = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, full, total);
    if (got < (int)sizeof(usb_config_descriptor_t)) {
        vga_puts("[USB] GET_DESCRIPTOR(CONFIG,full) failed\n");
        kfree(full);
        dev->present = false;
        return false;
    }

    // 6. Найти MSD интерфейс и его bulk endpoints.
    bool have = usb_pick_msd_endpoints(dev, full, got);
    uint8_t cfg_value = ((usb_config_descriptor_t*)full)->bConfigurationValue;
    kfree(full);

    if (!have) {
        vga_puts("[USB] device is not Mass Storage BBB/SCSI\n");
        dev->present = false;
        return false;
    }

    // 7. SET_CONFIGURATION.
    if (usb_set_configuration(dev, cfg_value) < 0) {
        vga_puts("[USB] SET_CONFIGURATION failed\n");
        dev->present = false;
        return false;
    }

    vga_puts("[USB] enumerated MSD addr=");
    vga_print_dec(dev->address);
    vga_puts(" ep_in=");
    vga_print_hex(dev->ep_in.bEndpointAddress);
    vga_puts(" ep_out=");
    vga_print_hex(dev->ep_out.bEndpointAddress);
    vga_puts(" max_packet=");
    vga_print_dec(dev->ep_in.wMaxPacketSize);
    vga_puts("\n");

    g_dev_count++;
    return true;
}

// ----------------------------------------------------------------------------
// Публичная инициализация
// ----------------------------------------------------------------------------

int usb_init(void)
{
    g_dev_count = 0;
    g_next_address = 1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) g_devs[i].present = false;

    if (!uhci_init()) {
        vga_puts("[USB] UHCI not present\n");
        return 0;
    }

    // Прогоняем оба порта.
    for (int port = 0; port < 2; port++) {
        if (enumerate_port(port)) {
            // нашли — продолжаем для второго порта
        }
    }
    return g_dev_count;
}

usb_device_t* usb_get_first_device(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (g_devs[i].present) return &g_devs[i];
    }
    return NULL;
}
